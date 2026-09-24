#!/usr/bin/env python3
"""Interleaved A/B timing of one opcode (docs/prd-simd-autoresearch.md §5.3, §6.1).

    bench.py --opcode op_mul --baseline build/release/bin/cedar_bench \
             [--candidate path/to/cedar_bench] [--rounds 5] [--reps 500]
    bench.py --pin                  # re-record baselines.json from --baseline

Baseline and candidate alternate within each round, so the speedup is always a
ratio to a fresh baseline, never to a number from an hour ago. Prints one JSON
object. The run is `unstable` if the governor is not `performance` or the fresh
baseline drifts > 10 % from the pinned one in baselines.json.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PINNED = HERE / "baselines.json"
CPU = os.environ.get("AUTORESEARCH_CPU", "2")  # a P-core on the i7-12700KF
DRIFT_LIMIT = 0.10
MIN_SPEEDUP = 1.05


def run(binary, opcode, reps):
    cmd = ["taskset", "-c", CPU, str(binary), "--opcode", opcode, "--reps", str(reps), "--json"]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def measure(opcode, baseline, candidate=None, rounds=5, reps=500):
    base, cand = [], []
    for _ in range(rounds):
        base.append(run(baseline, opcode, reps))
        if candidate:
            cand.append(run(candidate, opcode, reps))
    bmeds = [r["ns_per_block_median"] for r in base]
    bmed = statistics.median(bmeds)
    # Noise band: run-to-run spread of the baseline median against itself.
    # A speedup inside it is not a speedup. The within-run p10/p90 spread is
    # reported too, but it measures interrupt tails, not median noise.
    band = (max(bmeds) - min(bmeds)) / bmed
    spread = max((r["ns_p90"] - r["ns_p10"]) / r["ns_per_block_median"] for r in base)
    res = {"opcode": opcode, "baseline_ns": round(bmed, 2), "noise_band": round(band, 4),
           "p10_p90_spread": round(spread, 4),
           "isa_baseline": base[0]["isa"], "governor": base[0]["governor"], "cpu": base[0]["cpu"]}
    unstable = []
    if res["governor"] != "performance":
        unstable.append(f"governor={res['governor']}")
    pinned = json.loads(PINNED.read_text()).get("opcodes", {}) if PINNED.exists() else {}
    if opcode in pinned:
        drift = abs(bmed - pinned[opcode]) / pinned[opcode]
        res["baseline_drift"] = round(drift, 4)
        if drift > DRIFT_LIMIT:
            unstable.append(f"baseline drift {drift:.1%} vs pinned")
    if candidate:
        cmed = statistics.median(r["ns_per_block_median"] for r in cand)
        speedup = bmed / cmed
        res.update(candidate_ns=round(cmed, 2), speedup=round(speedup, 3),
                   isa_candidate=cand[0]["isa"],
                   faster=speedup >= MIN_SPEEDUP and (1 - cmed / bmed) > band)
    res["unstable"] = unstable or False
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--opcode")
    ap.add_argument("--baseline", default="build/release/bin/cedar_bench")
    ap.add_argument("--candidate")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--reps", type=int, default=500)
    ap.add_argument("--pin", action="store_true", help="re-record baselines.json for every opcode")
    a = ap.parse_args()

    if a.pin:
        names = subprocess.run([a.baseline, "--list"], check=True, capture_output=True,
                               text=True).stdout.split()
        rows = {n: measure(n, a.baseline, rounds=a.rounds, reps=a.reps) for n in names}
        first = next(iter(rows.values()))
        PINNED.write_text(json.dumps({
            "cpu": first["cpu"], "governor": first["governor"], "pinned_cpu": CPU,
            "opcodes": {n: r["baseline_ns"] for n, r in rows.items()},
            "noise_band": {n: r["noise_band"] for n, r in rows.items()},
        }, indent=2) + "\n")
        for n, r in sorted(rows.items(), key=lambda kv: -kv[1]["baseline_ns"]):
            print(f"{n:22s} {r['baseline_ns']:10.2f} ns/block  band {r['noise_band']:.1%}")
        return
    if not a.opcode:
        ap.error("--opcode required")
    print(json.dumps(measure(a.opcode, a.baseline, a.candidate, a.rounds, a.reps)))


if __name__ == "__main__":
    sys.exit(main())
