#!/usr/bin/env python3
"""SIMD autoresearch driver (docs/prd-simd-autoresearch.md §4.2).

    run.py --backend claude-code --model opus --target arith [--iterations 8]
    run.py --backend openrouter  --model deepseek/deepseek-chat --target svf
    run.py --backend local       --model qwen2.5-coder-7b --target distort

One (model, target) run: a worktree on branch autoresearch/<model>-<target>-<ts>,
N proposals, each verified by verify.py. Accepts are committed to the run
branch (master is never touched); rejects are saved as patches and reverted.
Logs runs/<run-id>/iterations.jsonl + summary.json.
"""
import argparse
import json
import re
import string
import subprocess
import sys
import time
from pathlib import Path

import verify
from verify import HERE, REPO, RUNS, TARGETS, VENV_PY, sh

BUDGET_USD = 20.0  # hard stop per run (§6 edge case 11)


def backend_for(name, model, effort=None):
    if name == "claude-code":
        from backends.claude_code import ClaudeCode
        return ClaudeCode(model, effort)
    if name == "openrouter":
        from backends.openrouter import OpenRouter
        return OpenRouter(model)
    if name == "local":
        from backends.local_openai import LocalOpenAI
        return LocalOpenAI(model)
    raise SystemExit(f"unknown backend {name}")


def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")


def extract_function(src, name):
    """Source of `inline void <name>(...) { ... }` by brace matching."""
    m = re.search(r"(\[\[gnu::always_inline\]\]\s*)?inline void " + re.escape(name) + r"\(", src)
    if not m:
        return f"// {name}: not found"
    i = src.index("{", m.end())
    depth = 0
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[m.start():j + 1]
    return src[m.start():]


def render_prompt(meta, run_dir, history):
    t = TARGETS[meta["target"]]
    src = (run_dir / "wt" / t["file"]).read_text()
    pinned = json.loads((HERE / "baselines.json").read_text())["opcodes"]
    approx = (f"Exception: {', '.join(t['approx'])} call `tanh`/`tan`; for these only, an "
              "approximation is allowed if (a) you mark it with a `// simd: approx` comment naming "
              "the replaced function, and (b) its peak error stays at or below -100 dBFS "
              "(about 1e-5 absolute) on every stimulus." if t["approx"] else
              "No approximations are allowed for these opcodes.")
    hist = "\n".join(history) if history else "None yet — this is the first attempt."
    return string.Template((HERE / "prompts" / "optimize_opcode.md").read_text()).substitute(
        opcodes="\n".join(f"- `{n}`" for n in t["bench"]),
        file=t["file"],
        sources="\n\n".join(extract_function(src, n) for n in t["bench"]),
        baseline="\n".join(f"- `{n}`: {pinned.get(n, '?')} ns" for n in t["bench"]),
        allowlist="`cedar/include/cedar/opcodes/**`, `cedar/include/cedar/dsp/simd*.hpp`, "
                  "`cedar/src/dsp/simd*.cpp`",
        approx_policy=approx,
        check_cmd=meta["check_cmd"],
        history=hist,
    )


def history_line(it, v):
    if v["verdict"] == "accept":
        sp = ", ".join(f"{n} {b['speedup']}x" for n, b in (v["bench"] or {}).items())
        return f"- Attempt {it}: ACCEPTED and committed ({sp}). Build on it."
    detail = (v["detail"] or "").strip()
    if len(detail) > 1500:
        detail = detail[:1500] + " …"
    return f"- Attempt {it}: REJECTED at gate `{v['reject_reason']}`. Reverted.\n  ```\n  " + \
        detail.replace("\n", "\n  ") + "\n  ```"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--backend", required=True, choices=["claude-code", "openrouter", "local"])
    ap.add_argument("--model", required=True)
    ap.add_argument("--target", required=True, choices=sorted(TARGETS))
    ap.add_argument("--iterations", type=int, default=8)
    ap.add_argument("--budget-usd", type=float, default=BUDGET_USD)
    ap.add_argument("--effort", help="claude-code only")
    ap.add_argument("--keep-worktree", action="store_true")
    a = ap.parse_args()

    backend = backend_for(a.backend, a.model, a.effort)
    ts = time.strftime("%Y-%m-%dT%H%MZ", time.gmtime())
    run_id = f"{ts}_{slug(a.model)}_{a.target}"
    branch = f"autoresearch/{slug(a.model)}-{a.target}-{time.strftime('%Y%m%d%H%M', time.gmtime())}"
    run_dir, meta = verify.new_run(run_id, a.target, "HEAD", branch)
    check = run_dir / "check.sh"
    check.write_text(f"#!/bin/sh\nexec {VENV_PY} {HERE / 'verify.py'} --quick --run {run_dir}\n")
    check.chmod(0o755)
    meta.update(check_cmd=str(check), model=a.model, provider=backend.provider,
                iterations=a.iterations, budget_usd=a.budget_usd)
    (run_dir / "run.json").write_text(json.dumps(meta, indent=2))
    print(f"[{run_id}] preparing baseline ...", file=sys.stderr)
    verify.prepare_baseline(run_dir, meta)

    log = run_dir / "iterations.jsonl"
    (run_dir / "rejected").mkdir()
    (run_dir / "accepted").mkdir()
    history, spent, status = [], 0.0, "complete"
    wt = run_dir / "wt"
    for it in range(1, a.iterations + 1):
        prompt = render_prompt(meta, run_dir, history)
        (run_dir / f"prompt-{it}.md").write_text(prompt)
        print(f"[{run_id}] iteration {it}/{a.iterations}: proposing ...", file=sys.stderr)
        u = backend.propose(prompt, wt, str(check), a.budget_usd - spent)
        (run_dir / f"transcript-{it}.txt").write_text(u.pop("transcript", "") or "")
        spent += u["cost_usd"] or 0.0
        print(f"[{run_id}] iteration {it}: verifying ...", file=sys.stderr)
        v = verify.verify(run_dir, meta)
        if u.get("error") and v["reject_reason"] == "empty patch":
            v["reject_reason"], v["detail"] = "backend error", u["error"]
        sh(["git", "add", "-A", "--", "."], cwd=wt)  # stage new files too, for the diff
        diff = sh(["git", "diff", "--cached", "HEAD"], cwd=wt).stdout
        sha = None
        if v["verdict"] == "accept":
            (run_dir / "accepted" / f"iter-{it}.patch").write_text(diff)
            speed = ", ".join(f"{n} {b['speedup']}x" for n, b in v["bench"].items())
            sh(["git", "commit", "-q", "-m",
                f"autoresearch({a.target}): iteration {it} by {a.model}\n\n{speed}"], cwd=wt)
            sha = sh(["git", "rev-parse", "HEAD"], cwd=wt).stdout.strip()
            verify.refresh_speed_baseline(run_dir)
        else:
            if diff:
                (run_dir / "rejected" / f"iter-{it}.patch").write_text(diff)
            verify.reset_wt(run_dir)
        rec = {
            "run_id": run_id, "iteration": it, "model": a.model, "provider": backend.provider,
            "target": a.target, "opcodes": TARGETS[a.target]["bench"], "isa_target": "avx2",
            **{k: u.get(k) for k in ("tokens", "cost_usd", "wall_clock_s", "gpu_s", "turns",
                                     "model_resolved", "capability_gaps")},
            "verdict": v["verdict"], "reject_reason": v["reject_reason"],
            "reject_detail": (v["detail"] or "")[:4000], "gates": v["gates"],
            "bench": v["bench"], "unstable": v["unstable"],
            "patch_sha": sha, "branch": branch,
        }
        with log.open("a") as f:
            f.write(json.dumps(rec) + "\n")
        history.append(history_line(it, v))
        print(f"[{run_id}] iteration {it}: {v['verdict']} "
              f"({v['reject_reason'] or 'all gates green'}), ${spent:.2f} spent", file=sys.stderr)
        if spent >= a.budget_usd:
            status = "budget-stopped"
            break

    import report
    summary = report.summarise(run_id, [json.loads(l) for l in log.read_text().splitlines()])
    summary.update(status=status, branch=branch, base_rev=meta["base_rev"])
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    if not a.keep_worktree:
        sh(["git", "worktree", "remove", "--force", str(wt)], cwd=REPO)


if __name__ == "__main__":
    sys.exit(main())
