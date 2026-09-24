#!/usr/bin/env python3
"""Reported (non-gating) platform legs for accepted kernels (PRD §4.4).

    legs.py --run runs/<id> --wasm              WASM SIMD128, headless in node
    legs.py --run runs/<id> --remote mac-host   NEON over ssh (Apple Silicon)
    legs.py --run runs/<id> --remote local      same remote script, run here

Each leg builds cedar_bench twice from the run branch — the run's base commit
and the branch head, same flags — then benchmarks them interleaved and
compares their output dumps. Results are appended to runs/<id>/legs.jsonl.
Toolchain gaps on a remote host are reported, never installed (OQ3).
"""
import argparse
import json
import math
import os
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import bench
from verify import REPO, TARGETS, sh  # re-execs under the experiments venv if needed

import numpy as np

# cedar + cedar_bench only: no akkado, tests, tools, OpenSSL or fetched deps.
MIN_CMAKE = ["-DCMAKE_BUILD_TYPE=Release", "-DCEDAR_SIMD=ON", "-DCEDAR_BUILD_BENCH=ON",
             "-DNKIDO_BUILD_AKKADO=OFF", "-DNKIDO_BUILD_TESTS=OFF", "-DNKIDO_BUILD_TOOLS=OFF",
             "-DNKIDO_BUILD_CATALOG=OFF", "-DCEDAR_ENABLE_FILE_IO=OFF"]


def compare_dumps(a, b):
    if np.array_equal(a.view(np.uint32), b.view(np.uint32)):
        return {"equality": "exact"}
    err = np.abs(a.astype(np.float64) - b.astype(np.float64))
    peak = float(np.nan_to_num(err, nan=np.inf).max())
    return {"equality": "differs", "null_test_dbfs": round(20 * math.log10(peak), 1)
            if 0 < peak < math.inf else None}


def leg_results(opcodes, base_cmd, head_cmd, dump_fn):
    rows = {}
    for n in opcodes:
        r = bench.measure(n, base_cmd, head_cmd, rounds=3, reps=300, check_pinned=False)
        r.update(compare_dumps(dump_fn(base_cmd, n), dump_fn(head_cmd, n)))
        rows[n] = {k: r.get(k) for k in ("baseline_ns", "candidate_ns", "speedup", "noise_band",
                                          "isa_baseline", "isa_candidate", "equality",
                                          "null_test_dbfs", "cpu")}
    return rows


# -------------------------------------------------------------------- wasm

def wasm_leg(meta, workdir):
    emsdk = os.environ.get("EMSDK")
    if not emsdk:
        return {"gap": "EMSDK not set"}
    toolchain = f"{emsdk}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
    bins = {}
    for label, rev in (("base", meta["base_rev"]), ("head", meta["branch"])):
        src = workdir / label
        sh(["git", "worktree", "add", "--detach", "-q", str(src), rev], cwd=REPO)
        bd = src / "build" / "wasm-simd"
        r = sh(["cmake", "-S", str(src), "-B", str(bd), f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                "-DCMAKE_CXX_FLAGS=-msimd128", *MIN_CMAKE])
        if not r.returncode:
            r = sh(["cmake", "--build", str(bd), "--target", "cedar_bench", "-j", str(os.cpu_count())])
        if r.returncode:
            return {"gap": f"{label} wasm build failed: {(r.stdout + r.stderr)[-1500:]}"}
        bins[label] = ["node", str(bd / "bin" / "cedar_bench.js")]

    def dump(cmd, n):
        f = workdir / f"{abs(hash(tuple(cmd)))}-{n}.f32"
        subprocess.run([*cmd, "--opcode", n, "--dump", str(f)], check=True, capture_output=True)
        return np.fromfile(f, dtype=np.float32)

    node = sh(["node", "--version"]).stdout.strip()
    return {"runtime": f"node {node}",
            "opcodes": leg_results(TARGETS[meta["target"]]["bench"], bins["base"], bins["head"], dump)}


# ------------------------------------------------------------------ remote

REMOTE_SCRIPT = r"""
set -e
dir="$1"; bundle="$2"; base="$3"; head="$4"; shift 4
for t in git cmake c++; do command -v "$t" >/dev/null || { echo "GAP: $t not found on $(hostname)"; exit 3; }; done
rm -rf "$dir/src" && git clone -q "$bundle" "$dir/src" && cd "$dir/src"
for label in base head; do
  rev=$base; [ "$label" = head ] && rev=$head
  git checkout -q --detach "$rev"
  cmake -S . -B "$dir/build-$label" "$@" >/dev/null
  cmake --build "$dir/build-$label" --target cedar_bench -j 8 >/dev/null
done
echo READY
"""


def remote_leg(meta, workdir, host):
    local = host == "local"
    rdir = str(workdir / "remote") if local else f"nkido-autoresearch/{meta['run_id']}"
    ssh = [] if local else ["ssh", "-o", "BatchMode=yes", host]
    bundle = workdir / "run.bundle"
    r = sh(["git", "bundle", "create", str(bundle), meta["branch"], meta["base_rev"]], cwd=REPO)
    if r.returncode:
        return {"gap": f"git bundle failed: {r.stderr}"}
    if local:
        Path(rdir).mkdir(parents=True, exist_ok=True)
        rbundle = str(bundle)
    else:
        sh([*ssh, "mkdir", "-p", rdir])
        r = sh(["scp", "-q", "-o", "BatchMode=yes", str(bundle), f"{host}:{rdir}/run.bundle"])
        if r.returncode:
            return {"gap": f"scp to {host} failed: {r.stderr.strip()}"}
        rbundle = f"{rdir}/run.bundle"
    head = sh(["git", "rev-parse", meta["branch"]], cwd=REPO).stdout.strip()
    args = [rdir, rbundle, meta["base_rev"], head, *MIN_CMAKE]  # SHAs: no branch DWIM
    cmd = ["bash", "-c", REMOTE_SCRIPT, "remote", *args]
    r = subprocess.run(cmd if local else [*ssh, " ".join(shlex.quote(c) for c in cmd)],
                       capture_output=True, text=True, timeout=3600)
    if "READY" not in r.stdout:
        return {"gap": (r.stdout + r.stderr).strip()[-1500:]}
    bins = {l: [*ssh, f"{rdir}/build-{l}/bin/cedar_bench"] for l in ("base", "head")}

    def dump(cmd, n):
        f = f"{rdir}/{cmd[-1].split('/')[-3]}-{n}.f32"
        subprocess.run([*cmd, "--opcode", n, "--dump", f], check=True, capture_output=True)
        raw = subprocess.run([*ssh, "cat", f], check=True, capture_output=True).stdout
        return np.frombuffer(raw, dtype=np.float32)

    uname = subprocess.run([*ssh, "uname", "-sm"], capture_output=True, text=True).stdout.strip()
    return {"host": host, "uname": uname,
            "opcodes": leg_results(TARGETS[meta["target"]]["bench"], bins["base"], bins["head"], dump)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True, type=Path)
    ap.add_argument("--wasm", action="store_true")
    ap.add_argument("--remote")
    a = ap.parse_args()
    run_dir = a.run.resolve()
    meta = json.loads((run_dir / "run.json").read_text())
    if not meta.get("branch"):
        raise SystemExit("run has no branch (selftest runs are detached)")
    out = []
    with tempfile.TemporaryDirectory(prefix="legs-", dir=run_dir) as tmp:
        tmp = Path(tmp)
        try:
            if a.wasm:
                out.append({"leg": "wasm-simd128", **wasm_leg(meta, tmp)})
            if a.remote:
                out.append({"leg": f"remote:{a.remote}", **remote_leg(meta, tmp, a.remote)})
        finally:
            for wt in ("base", "head"):
                if (tmp / wt).exists():
                    sh(["git", "worktree", "remove", "--force", str(tmp / wt)], cwd=REPO)
    with (run_dir / "legs.jsonl").open("a") as f:
        for rec in out:
            rec["at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            f.write(json.dumps(rec) + "\n")
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    sys.exit(main())
