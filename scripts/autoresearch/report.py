#!/usr/bin/env python3
"""JSONL → talk table + chart (docs/prd-simd-autoresearch.md §5.4, G4).

    report.py [runs/...]            markdown table on stdout, chart PNG next to it
    report.py --out results.md      write the table to a file instead

With no arguments every run under runs/ (except selftest-*) is included.
Everything is recomputed from iterations.jsonl, so the table and chart
regenerate with one command.
"""
import argparse
import json
import math
import os
import sys
from pathlib import Path

try:
    import matplotlib  # noqa: F401
except ModuleNotFoundError:  # re-exec under the experiments venv
    _venv = Path(__file__).resolve().parents[2] / "experiments" / ".venv" / "bin" / "python3"
    if __name__ == "__main__" and _venv.exists() and Path(sys.executable).resolve() != _venv.resolve():
        os.execv(str(_venv), [str(_venv), *sys.argv])

HERE = Path(__file__).resolve().parent
RUNS = HERE / "runs"


def best_speedup(rec):
    """Geometric mean of the run's opcodes' speedups vs the original scalar
    build (an accept after an accept reports speedup_vs_origin)."""
    b = rec.get("bench") or {}
    sps = [v.get("speedup_vs_origin") or v["speedup"] for v in b.values()]
    return math.exp(sum(math.log(s) for s in sps) / len(sps)) if sps else None


def summarise(run_id, recs):
    tok = {"input": 0, "output": 0, "cached": 0}
    for r in recs:
        for k in tok:
            tok[k] += (r.get("tokens") or {}).get(k, 0) or 0
    accepts = [r for r in recs if r["verdict"] == "accept"]
    speeds = [best_speedup(r) for r in accepts]
    reasons = {}
    for r in recs:
        if r["verdict"] != "accept":
            reasons[r["reject_reason"]] = reasons.get(r["reject_reason"], 0) + 1
    first = recs[0] if recs else {}
    return {
        "run_id": run_id, "provider": first.get("provider"),
        "model": first.get("model") + (f" ({', '.join(first['model_resolved'])})"
                                       if first.get("model_resolved") else "") if first else None,
        "target": first.get("target"), "iterations": len(recs),
        "accepts": len(accepts), "rejects": len(recs) - len(accepts), "reject_reasons": reasons,
        "first_accept_iteration": accepts[0]["iteration"] if accepts else None,
        "best_speedup": round(max(speeds), 3) if speeds else None,
        "tokens": tok,
        "cost_usd": round(sum(r.get("cost_usd") or 0 for r in recs), 4),
        "wall_clock_s": round(sum(r.get("wall_clock_s") or 0 for r in recs), 1),
        "gpu_s": round(sum(r.get("gpu_s") or 0 for r in recs), 1) or None,
        # parallel lanes: wall clock minus time queued on verify.machine_lock
        "active_s": round(sum((r.get("wall_clock_s") or 0) - (r.get("lock_wait_propose_s") or 0)
                              for r in recs), 1)
                    if recs and all("lock_wait_propose_s" in r for r in recs) else None,
        "unstable_iterations": sum(1 for r in recs if r.get("unstable")),
    }


def load(dirs):
    out = []
    for d in dirs:
        f = d / "iterations.jsonl"
        if f.exists() and f.stat().st_size:
            recs = [json.loads(l) for l in f.read_text().splitlines() if l.strip()]
            s = summarise(d.name, recs)
            sj = d / "summary.json"
            if sj.exists():
                s["status"] = json.loads(sj.read_text()).get("status")
            out.append(s)
    return out


def table(rows):
    h = ("| Model | Target | Accepted | 1st accept | Best speedup | Tokens in / out (cached) "
         "| USD | $ per 1% gained | Wall clock | Rejections |\n"
         "|---|---|---|---|---|---|---|---|---|---|\n")
    lines = []
    for s in sorted(rows, key=lambda s: (s["target"], s["model"])):
        sp = s["best_speedup"]
        gain = (sp - 1) * 100 if sp else 0
        per = f"${s['cost_usd'] / gain:.3f}" if gain > 0 and s["cost_usd"] else "—"
        t = s["tokens"]
        rej = ", ".join(f"{k} ×{v}" for k, v in sorted(s["reject_reasons"].items())) or "—"
        status = f" ({s['status']})" if s.get("status") not in (None, "complete") else ""
        lines.append(
            f"| {s['model']}{status} | {s['target']} | {s['accepts']}/{s['iterations']} "
            f"| {s['first_accept_iteration'] or '—'} | {f'{sp:.2f}×' if sp else '—'} "
            f"| {t['input']:,} / {t['output']:,} ({t['cached']:,}) | ${s['cost_usd']:.2f} "
            f"| {per} | {s['wall_clock_s'] / 60:.0f} min"
            + (f" ({s['active_s'] / 60:.0f} active)" if s.get("active_s") is not None else "")
            + (f" (GPU {s['gpu_s']:.0f} s)" if s.get("gpu_s") else "") + f" | {rej} |")
    return h + "\n".join(lines) + "\n"


def chart(rows, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8, 5))
    models = sorted({s["model"] for s in rows})
    marks = "osD^vP*X"
    for i, m in enumerate(models):
        pts = [s for s in rows if s["model"] == m]
        xs = [s["tokens"]["input"] + s["tokens"]["output"] for s in pts]
        ys = [s["best_speedup"] or 1.0 for s in pts]
        ax.scatter(xs, ys, marker=marks[i % len(marks)], s=70, label=m)
        for s, x, y in zip(pts, xs, ys):
            ax.annotate(s["target"], (x, y), textcoords="offset points", xytext=(5, 4), fontsize=8)
    ax.set_xscale("log")
    ax.axhline(1.0, color="grey", lw=0.8, ls="--")
    ax.set_xlabel("tokens spent per run (input + output, log)")
    ax.set_ylabel("best accepted speedup (× vs scalar)")
    ax.set_title("SIMD autoresearch: best speedup vs tokens per run")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=150)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="*", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--chart", type=Path, default=HERE / "runs" / "results.png")
    a = ap.parse_args()
    dirs = a.runs or sorted(d for d in RUNS.glob("*") if d.is_dir()
                            and not d.name.startswith("selftest-"))
    rows = load(dirs)
    if not rows:
        print("no runs with iterations.jsonl found", file=sys.stderr)
        return 1
    md = table(rows)
    if a.out:
        a.out.write_text(md)
    else:
        print(md)
    chart(rows, a.chart)
    print(f"chart: {a.chart}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
