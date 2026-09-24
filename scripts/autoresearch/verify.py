#!/usr/bin/env python3
"""Gates for the SIMD autoresearch loop (docs/prd-simd-autoresearch.md §4.3, §5.5).

A run directory holds a candidate git worktree (`wt/`) plus a baseline cache
(`baseline/`) built from the pristine tree before the first proposal:

    verify.py --run RUN_DIR               full gate, verdict JSON on stdout
    verify.py --run RUN_DIR --quick       model self-check: allowlist, build,
                                          equality, one short bench (text)
    verify.py --selftest                  apply fixtures/*.patch to a scratch
                                          run and check each expected verdict

Gate order (first failure wins, its output is the rejection reason fed back
to the model): allowlist → build → equality → unit → zero_alloc →
experiment → speed.
"""
import argparse
import json
import math
import os
import random
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

try:
    import numpy as np
except ModuleNotFoundError:  # re-exec under the experiments venv (numpy, matplotlib)
    _venv = Path(__file__).resolve().parents[2] / "experiments" / ".venv" / "bin" / "python3"
    if _venv.exists() and Path(sys.executable).resolve() != _venv.resolve():
        os.execv(str(_venv), [str(_venv), *sys.argv])
    raise

import bench

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
VENV_PY = REPO / "experiments" / ".venv" / "bin" / "python3"
RUNS = HERE / "runs"
FIXTURES = HERE / "fixtures"

# Writable by the model (§5.5). Everything else is the measurement.
ALLOWLIST = [
    re.compile(r"^cedar/include/cedar/opcodes/.+"),
    re.compile(r"^cedar/include/cedar/dsp/simd[^/]*\.hpp$"),
    re.compile(r"^cedar/src/dsp/simd[^/]*\.cpp$"),
]
APPROX_MARK = "simd: approx"
NULL_TEST_DBFS = -100.0
DUMP_BLOCKS = 1024
BUILD_TARGETS = ["cedar_bench", "cedar_tests", "akkado_tests", "cedar_core"]
MAX_CHECKS = 10  # quick-check calls per attempt, same for every backend (research P4)

# Candidate set (§3.1). `approx`: opcodes whose scalar body calls a
# transcendental (tanh / tan) and may take a declared approximation.
TARGETS = {
    "arith": {"bench": ["op_add", "op_sub", "op_mul"], "approx": [],
              "file": "cedar/include/cedar/opcodes/arithmetic.hpp", "experiments": []},
    "distort": {"bench": ["op_distort_tanh", "op_distort_soft"], "approx": ["op_distort_tanh"],
                "file": "cedar/include/cedar/opcodes/distortion.hpp",
                "experiments": ["test_op_saturate.py", "test_op_softclip.py"]},
    "formant": {"bench": ["op_filter_formant"], "approx": ["op_filter_formant"],
                "file": "cedar/include/cedar/opcodes/filters.hpp",
                "experiments": ["test_op_formant.py"]},
    "svf": {"bench": ["op_filter_svf_lp", "op_filter_svf_hp", "op_filter_svf_bp"],
            "approx": ["op_filter_svf_lp", "op_filter_svf_hp", "op_filter_svf_bp"],
            "file": "cedar/include/cedar/opcodes/filters.hpp",
            "experiments": ["test_op_lp.py", "test_op_hp.py", "test_op_bp.py"]},
    # C5, nominated by the Phase 0 ranking (heaviest stable opcode).
    "freeverb": {"bench": ["op_reverb_freeverb"], "approx": [],
                 "file": "cedar/include/cedar/opcodes/reverbs.hpp",
                 "experiments": ["test_op_freeverb.py"]},
}


class Reject(Exception):
    def __init__(self, reason, detail=""):
        super().__init__(reason)
        self.reason, self.detail = reason, detail


def sh(cmd, cwd=None, env=None, timeout=None):
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True,
                          errors="replace", timeout=timeout)


def tail(text, n=40):
    return "\n".join(text.strip().splitlines()[-n:])


# ---------------------------------------------------------------- run layout

def load_run(run_dir):
    run_dir = Path(run_dir).resolve()
    meta = json.loads((run_dir / "run.json").read_text())
    return run_dir, meta


def build_dir(wt):
    return Path(wt) / "build" / "simd"


def configure(wt):
    """Configure the worktree's `simd` build, reusing the main checkout's
    fetched deps and ccache so a fresh worktree does not hit the network."""
    deps = REPO / "build" / "simd" / "_deps"
    args = ["cmake", "--preset", "simd", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
            f"-DPYTHON_EXECUTABLE={VENV_PY}"]
    for name in ("CATCH2", "RTMIDI"):
        src = deps / f"{name.lower()}-src"
        if src.exists():
            args.append(f"-DFETCHCONTENT_SOURCE_DIR_{name}={src}")
    r = sh(args, cwd=wt, env=ccache_env())
    if r.returncode:
        raise RuntimeError("configure failed:\n" + tail(r.stdout + r.stderr))


def ccache_env():
    return {**os.environ, "CCACHE_BASEDIR": str(RUNS), "CCACHE_NOHASHDIR": "1"}


def build(wt, targets=BUILD_TARGETS):
    r = sh(["cmake", "--build", str(build_dir(wt)), "-j", str(os.cpu_count() or 8),
            "--target", *targets], env=ccache_env())
    if r.returncode:
        errs = [l for l in (r.stdout + r.stderr).splitlines() if "error" in l.lower()]
        raise Reject("build", tail("\n".join(errs) or r.stdout + r.stderr, 40))


def bench_bin(wt):
    return build_dir(wt) / "bin" / "cedar_bench"


def dump(binary, opcode, path, scalar=False, seed=0):
    cmd = [str(binary), "--opcode", opcode, "--dump", str(path), "--blocks", str(DUMP_BLOCKS)]
    if scalar:
        cmd.append("--scalar")
    if seed:
        cmd += ["--seed", str(seed)]
    r = sh(cmd)
    if r.returncode:
        raise Reject("equality", f"{opcode}: dump failed: {tail(r.stderr, 10)}")
    return np.fromfile(path, dtype=np.float32)


def run_experiment(wt, script):
    t0 = time.time()
    r = sh([str(VENV_PY), script], cwd=Path(wt) / "experiments",
           env={**os.environ, "MPLBACKEND": "Agg"}, timeout=1800)
    out = r.stdout + r.stderr
    return {"rc": r.returncode, "fail": out.count("✗"), "s": round(time.time() - t0, 1),
            "tail": tail(out, 15)}


# ---------------------------------------------------------- baseline cache

def prepare_baseline(run_dir, meta):
    """Build the pristine worktree once and cache everything the gates
    compare against: bench binary, output dumps, experiment ✗ counts."""
    wt, base = run_dir / "wt", run_dir / "baseline"
    base.mkdir(exist_ok=True)
    configure(wt)
    build(wt)
    shutil.copy2(bench_bin(wt), base / "cedar_bench")
    names = sh([str(base / "cedar_bench"), "--list"]).stdout.split()
    for n in names:
        dump(base / "cedar_bench", n, base / f"{n}.f32")
    exps = {s: run_experiment(wt, s) for s in TARGETS[meta["target"]]["experiments"]}
    (base / "experiments.json").write_text(json.dumps(exps, indent=2))
    (base / "cases.json").write_text(json.dumps(names))


def refresh_speed_baseline(run_dir):
    """After an accept, later proposals must beat the accepted state, not the
    original. Equality always stays against the original scalar dumps."""
    shutil.copy2(bench_bin(run_dir / "wt"), run_dir / "baseline" / "cedar_bench.head")


# ------------------------------------------------------------------- gates

def changed_files(wt, base_rev):
    tracked = sh(["git", "diff", "--name-only", base_rev], cwd=wt).stdout.split()
    untracked = sh(["git", "ls-files", "--others", "--exclude-standard"], cwd=wt).stdout.split()
    return sorted(set(tracked) | set(untracked))


def added_lines(wt, base_rev):
    diff = sh(["git", "diff", base_rev], cwd=wt).stdout
    extra = ""
    for p in sh(["git", "ls-files", "--others", "--exclude-standard"], cwd=wt).stdout.split():
        extra += (Path(wt) / p).read_text(errors="replace")
    return "\n".join(l[1:] for l in diff.splitlines() if l.startswith("+")) + extra


def gate_allowlist(wt, base_rev):
    files = changed_files(wt, base_rev)
    bad = [f for f in files if not any(p.match(f) for p in ALLOWLIST)]
    if bad:
        raise Reject("out-of-allowlist edit", ", ".join(bad))
    if not files:
        raise Reject("empty patch", "no files changed")
    return files


def compare(n, ref, cand, target, declared, label):
    """None if bit-identical, else the null-test dB of a permitted approximation;
    raises Reject otherwise."""
    if np.array_equal(cand.view(np.uint32), ref.view(np.uint32)):
        return None
    err = np.abs(cand.astype(np.float64) - ref.astype(np.float64))
    err = np.where(np.isnan(err), np.inf, err)
    peak = float(err.max())
    db = 20 * math.log10(peak) if peak > 0 else -math.inf
    first = int(np.argmax(err > 0))
    where = f"{label}: first diff at sample {first} (block {first // 256}): " \
            f"ref {ref[first]!r} vs candidate {cand[first]!r}"
    if n not in target["approx"]:
        raise Reject("equality", f"{n}: not bit-identical (peak error {db:.1f} dBFS); "
                                 f"{where}. Only {target['approx'] or 'no'} opcodes "
                                 "may take a declared approximation.")
    if not declared:
        raise Reject("undeclared approximation",
                     f"{n}: not bit-identical ({db:.1f} dBFS, {label}) and no "
                     f"`// {APPROX_MARK}` comment declares the approximation")
    if db > NULL_TEST_DBFS:
        raise Reject("equality", f"{n}: null test {db:.1f} dBFS > {NULL_TEST_DBFS} dBFS; {where}")
    if not np.isfinite(cand).all() and np.isfinite(ref).all():
        raise Reject("equality", f"{n}: candidate produces NaN/Inf ({label})")
    return db


def gate_equality(run_dir, meta, hidden=True):
    """Bit-identity (or a declared approximation) on the fixed stimuli, then —
    full verify only — on a fresh random hidden-stimulus seed the model never
    sees (research P5). Both references are the original scalar build."""
    wt, base = run_dir / "wt", run_dir / "baseline"
    target = TARGETS[meta["target"]]
    declared = APPROX_MARK in added_lines(wt, meta["base_rev"])
    dbs = []
    tmp = run_dir / "dumps"
    tmp.mkdir(exist_ok=True)
    seed = random.randint(1, 2**31 - 1) if hidden else 0
    for n in json.loads((base / "cases.json").read_text()):
        ref = np.fromfile(base / f"{n}.f32", dtype=np.float32)
        # Fallback first: the scalar body is the oracle and must survive
        # bit-for-bit on every opcode, target or not.
        sc = dump(bench_bin(wt), n, tmp / f"{n}.scalar.f32", scalar=True)
        if not np.array_equal(sc.view(np.uint32), ref.view(np.uint32)):
            raise Reject("equality", f"{n}: scalar fallback path no longer bit-identical "
                                     "to baseline (the scalar body must stay intact)")
        cand = dump(bench_bin(wt), n, tmp / f"{n}.f32")
        dbs.append(compare(n, ref, cand, target, declared, "fixed stimuli"))
        if seed:
            href = dump(base / "cedar_bench", n, tmp / f"{n}.hidden.ref.f32", seed=seed)
            hcand = dump(bench_bin(wt), n, tmp / f"{n}.hidden.f32", seed=seed)
            dbs.append(compare(n, href, hcand, target, declared,
                               f"hidden randomised stimuli (seed {seed})"))
    dbs = [d for d in dbs if d is not None]
    return {"equality": "approx" if dbs else "exact", "hidden_seed": seed or None,
            "null_test_dbfs": round(max(dbs), 1) if dbs else None}


def gate_unit(wt):
    bd = build_dir(wt)
    for name, exe in (("cedar_tests", bd / "cedar" / "tests" / "cedar_tests"),
                      ("akkado_tests", bd / "akkado" / "tests" / "akkado_tests")):
        r = sh([str(exe)], cwd=wt, timeout=1800)  # some tests read sources repo-relative
        if r.returncode:
            fails = [l for l in r.stdout.splitlines() if "FAILED" in l or "failed" in l]
            raise Reject("unit", f"{name}: " + tail("\n".join(fails) or r.stdout, 30))


def gate_zero_alloc(wt):
    r = sh([str(build_dir(wt) / "cedar" / "tests" / "cedar_tests"), "[zero_alloc]"], cwd=wt)
    if r.returncode:
        raise Reject("zero_alloc", tail(r.stdout, 30))


def gate_experiment(run_dir, meta):
    base = json.loads((run_dir / "baseline" / "experiments.json").read_text())
    for script, ref in base.items():
        r = run_experiment(run_dir / "wt", script)
        if r["rc"] != 0 or r["fail"] > ref["fail"]:
            raise Reject("experiment", f"{script}: rc={r['rc']}, ✗ {r['fail']} "
                                       f"(baseline {ref['fail']})\n{r['tail']}")


def gate_speed(run_dir, meta, rounds=5, reps=500):
    base = run_dir / "baseline"
    head = base / "cedar_bench.head"
    ref_bin = head if head.exists() else base / "cedar_bench"
    rows = {n: bench.measure(n, ref_bin, bench_bin(run_dir / "wt"), rounds, reps)
            for n in TARGETS[meta["target"]]["bench"]}
    if head.exists():  # also report against the original scalar build
        for n, r in rows.items():
            orig = bench.measure(n, base / "cedar_bench", bench_bin(run_dir / "wt"), 3, reps)
            r["speedup_vs_origin"] = orig["speedup"]
    unstable = [f"{n}: {r['unstable']}" for n, r in rows.items() if r["unstable"]]
    slower = [n for n, r in rows.items() if r["speedup"] < 1 - max(r["noise_band"], 0.02)]
    summary = {n: {k: r.get(k) for k in ("baseline_ns", "candidate_ns", "speedup",
                                          "speedup_vs_origin", "noise_band")}
               for n, r in rows.items()}
    if slower:
        raise Reject("speed", "slower than baseline: " + ", ".join(
            f"{n} {rows[n]['speedup']:.3f}x" for n in slower))
    if not any(r["faster"] for r in rows.values()):
        raise Reject("speed", "no opcode improved by >= 5 % outside the noise band: " + ", ".join(
            f"{n} {r['speedup']:.3f}x (band {r['noise_band']:.1%})" for n, r in rows.items()))
    return summary, unstable


def verify(run_dir, meta):
    """Full gate. Returns the verdict dict; never raises for a bad patch."""
    wt = run_dir / "wt"
    gates = {}
    res = {"verdict": "reject", "reject_reason": None, "detail": "", "gates": gates,
           "bench": None, "unstable": False, "files": []}
    try:
        res["files"] = gate_allowlist(wt, meta["base_rev"])
        gates["allowlist"] = "pass"
        build(wt)
        gates["build"] = "pass"
        gates.update(gate_equality(run_dir, meta))
        gate_unit(wt)
        gates["unit"] = "pass"
        gate_zero_alloc(wt)
        gates["zero_alloc"] = "pass"
        gate_experiment(run_dir, meta)
        gates["experiment"] = "pass"
        gates["speed"] = "fail"
        res["bench"], unstable = gate_speed(run_dir, meta)
        gates["speed"] = "pass"
        res["unstable"] = unstable or False
        res["verdict"] = "accept"
    except Reject as e:
        res["reject_reason"], res["detail"] = e.reason, e.detail
    return res


def vectoriser_remarks(wt, header, limit=40):
    """GCC -fopt-info-vec for the opcode header, from recompiling vm.cpp (where
    the opcodes are inlined) — research P10: precise vectoriser feedback."""
    db = json.loads((build_dir(wt) / "compile_commands.json").read_text())
    entry = next((e for e in db if e["file"].endswith("cedar/src/vm/vm.cpp")), None)
    if not entry:
        return "(no compile command for vm.cpp)"
    args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    if "-o" in args:
        i = args.index("-o")
        args[i + 1] = os.devnull
    args += ["-fopt-info-vec-optimized", "-fopt-info-vec-missed"]
    r = sh(args, cwd=entry["directory"])
    name = Path(header).name
    seen, out = set(), []
    for l in (r.stdout + r.stderr).splitlines():
        if name in l and ("optimized:" in l or "missed:" in l):
            key = re.sub(r"^.*?" + re.escape(name), name, l)
            if key not in seen:
                seen.add(key)
                out.append(key)
    if not out:
        return f"(no vectoriser remarks for {name})"
    more = f"\n  … {len(out) - limit} more" if len(out) > limit else ""
    return "\n".join("  " + l for l in out[:limit]) + more


def quick(run_dir, meta):
    """Fast self-check for the model: fixed stimuli only, vectoriser remarks,
    one short bench against the current head. Capped at MAX_CHECKS calls."""
    wt = run_dir / "wt"
    counter = run_dir / "check_calls"
    n_calls = int(counter.read_text() or 0) + 1 if counter.exists() else 1
    counter.write_text(str(n_calls))
    if n_calls > MAX_CHECKS:
        return (f"CHECK BUDGET EXHAUSTED: {MAX_CHECKS} checks per attempt. Stop now and write "
                "your final message (ending with the IDEA line).")
    header = f"(check {n_calls}/{MAX_CHECKS})"
    try:
        files = gate_allowlist(wt, meta["base_rev"])
        build(wt, ["cedar_bench"])
        eq = gate_equality(run_dir, meta, hidden=False)
    except Reject as e:
        return f"{header} FAIL [{e.reason}]\n{e.detail}"
    lines = [f"{header} OK: allowlist ({', '.join(files)}), build, equality on the fixed stimuli "
             f"({eq['equality']}"
             + (f", null test {eq['null_test_dbfs']} dBFS" if eq["null_test_dbfs"] else "") + ")",
             f"Vectoriser remarks for {TARGETS[meta['target']]['file']}:",
             vectoriser_remarks(wt, TARGETS[meta["target"]]["file"]),
             "Short benchmark vs the current version:"]
    head = run_dir / "baseline" / "cedar_bench.head"
    ref = head if head.exists() else run_dir / "baseline" / "cedar_bench"
    for n in TARGETS[meta["target"]]["bench"]:
        r = bench.measure(n, ref, bench_bin(wt), 1, 200)
        lines.append(f"  {n}: {r['baseline_ns']} -> {r['candidate_ns']} ns/block "
                     f"({r['speedup']:.2f}x, isa {r['isa_candidate']})")
    lines.append("(hidden stimuli, unit suites, experiments and the full 5-round bench run at "
                 "the final verify)")
    return "\n".join(lines)


# ---------------------------------------------------------------- selftest

SELFTEST = [  # (fixture, target, expected verdict, expected reason)
    ("good_mul_avx2.patch", "arith", "accept", None),
    ("bad_lane_swap.patch", "arith", "reject", "equality"),
    ("slower_mul.patch", "arith", "reject", "speed"),
    ("cheat_bench.patch", "arith", "reject", "out-of-allowlist edit"),
    ("tanh_undeclared.patch", "distort", "reject", "undeclared approximation"),
    ("hidden_ftz_mul.patch", "arith", "reject", "equality"),  # exact on fixed stimuli only
]


def new_run(run_id, target, base_rev="HEAD", branch=None):
    """Create runs/<id>/ with a detached (or branched) worktree + run.json."""
    run_dir = RUNS / run_id
    run_dir.mkdir(parents=True)
    base_rev = sh(["git", "rev-parse", base_rev], cwd=REPO).stdout.strip()
    cmd = ["git", "worktree", "add", "--quiet"]
    cmd += ["-b", branch, str(run_dir / "wt"), base_rev] if branch else \
           ["--detach", str(run_dir / "wt"), base_rev]
    r = sh(cmd, cwd=REPO)
    if r.returncode:
        raise RuntimeError(r.stderr)
    meta = {"run_id": run_id, "target": target, "base_rev": base_rev, "branch": branch}
    (run_dir / "run.json").write_text(json.dumps(meta, indent=2))
    return run_dir, meta


def reset_wt(run_dir):
    wt = run_dir / "wt"
    sh(["git", "reset", "-q", "--hard", "HEAD"], cwd=wt)
    sh(["git", "clean", "-fdq"], cwd=wt)  # ignored files (build/) survive


def selftest(only=None):
    stamp = time.strftime("%Y%m%dT%H%M%S")
    results, ok = [], True
    runs = {}
    for fixture, target, want, want_reason in SELFTEST:
        if only and only not in fixture:
            continue
        if target not in runs:
            print(f"[selftest] preparing baseline for {target} ...", file=sys.stderr)
            rd, meta = new_run(f"selftest-{stamp}-{target}", target)
            prepare_baseline(rd, meta)
            runs[target] = (rd, meta)
        rd, meta = runs[target]
        reset_wt(rd)
        r = sh(["git", "apply", str(FIXTURES / fixture)], cwd=rd / "wt")
        if r.returncode:
            raise RuntimeError(f"{fixture} does not apply: {r.stderr}")
        t0 = time.time()
        v = verify(rd, meta)
        good = v["verdict"] == want and v["reject_reason"] == want_reason
        ok &= good
        line = (f"{'PASS' if good else 'FAIL'} {fixture}: {v['verdict']}"
                f" ({v['reject_reason'] or 'all gates green'}) in {time.time() - t0:.0f}s")
        if v["bench"]:
            line += " — " + ", ".join(f"{n} {b['speedup']}x" for n, b in v["bench"].items())
        print(line)
        if not good:
            print("   detail:", v["detail"][:600])
        results.append({"fixture": fixture, **v})
    reset_wt(rd)
    for rd, _ in runs.values():
        sh(["git", "worktree", "remove", "--force", str(rd / "wt")], cwd=REPO)
    print("selftest", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--selftest", nargs="?", const="", default=None,
                    help="run all fixtures, or only those whose name contains the argument")
    a = ap.parse_args()
    if a.selftest is not None:
        return selftest(a.selftest or None)
    if not a.run:
        ap.error("--run or --selftest required")
    run_dir, meta = load_run(a.run)
    if a.quick:
        print(quick(run_dir, meta))
    else:
        print(json.dumps(verify(run_dir, meta), indent=2))


if __name__ == "__main__":
    sys.exit(main())
