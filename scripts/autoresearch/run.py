#!/usr/bin/env python3
"""SIMD autoresearch driver (docs/prd-simd-autoresearch.md §4.2).

    run.py --backend claude-code --model opus --target arith [--iterations 8]
    run.py --backend openrouter  --model z-ai/glm-5.3 --target svf
    run.py --backend local       --model qwen3:14b --target distort

One (model, target) run: a worktree on branch autoresearch/<model>-<target>-<ts>,
N proposals, each verified by verify.py. Accepts are committed to the run
branch (master is never touched); rejects are saved as patches and reverted.
Logs runs/<run-id>/iterations.jsonl + summary.json.

Every attempt is a fresh session (so backends' own context handling can't
skew the comparison); memory between attempts is the prompt: current vs
original ns, the accepted diff, and an attempts table built from each
agent's `IDEA:` line (docs/research/llm-autoresearch-loops.md P1-P8).
"""
import argparse
import difflib
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
# Same per-attempt budget for every backend (research P4): wall clock here,
# check-command calls in verify.MAX_CHECKS. `claude -p` has no turn cap, so
# turns are logged, not limited.
ITERATION_S = 30 * 60
PLATEAU_AFTER = 3
DUP_RATIO = 0.95
ACCEPTED_DIFF_LIMIT = 8000


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


def parse_idea(text):
    """The agent's `IDEA: …` ledger line (research P1); last one wins."""
    hits = re.findall(r"^[\s>*`]*IDEA:\s*(.+?)[`*\s]*$", text or "", re.M)
    return hits[-1].strip()[:400] if hits else None


def normalise_diff(diff):
    """+/- lines only, whitespace collapsed: the idea, not the formatting."""
    return "\n".join(l[0] + " ".join(l[1:].split()) for l in diff.splitlines()
                     if l[:1] in ("+", "-") and not l.startswith(("+++", "---")))


def find_duplicate(diff, attempts):
    """Earlier rejected attempt whose patch is ≥ DUP_RATIO similar (research P8,
    ShinkaEvolve's novelty filter); None if the proposal is new."""
    norm = normalise_diff(diff)
    for a in attempts:
        if a["verdict"] == "accept" or not a.get("norm_diff") or not norm:
            continue
        m = difflib.SequenceMatcher(None, norm, a["norm_diff"], autojunk=False)
        if m.quick_ratio() >= DUP_RATIO and m.ratio() >= DUP_RATIO:
            return a["iteration"]
    return None


def history_table(attempts):
    """results.tsv-style attempts table (research P1/P3); full reject detail
    only for the latest reject, a one-line reason for older ones."""
    if not attempts:
        return "None yet — this is the first attempt."
    rows = ["| # | Verdict | Idea (the agent's own IDEA line) | Speedup vs previous |",
            "|---|---|---|---|"]
    for a in attempts:
        if a["verdict"] == "accept":
            verdict = "ACCEPTED"
        else:
            first = (a["detail"] or "").strip().splitlines()[:1]
            why = first[0][:120] if first else ""
            verdict = f"rejected: `{a['reason']}`" + (f" — {why}" if why else "")
        sp = ", ".join(f"{n.removeprefix('op_')} {b['speedup']}x"
                       for n, b in (a["bench"] or {}).items()) or "—"
        idea = (a["idea"] or "(no IDEA line)").replace("|", "/")[:200]
        rows.append(f"| {a['iteration']} | {verdict.replace('|', '/')} | {idea} | {sp} |")
    out = "\n".join(rows)
    last = next((a for a in reversed(attempts) if a["verdict"] != "accept"), None)
    if last and last["detail"]:
        detail = last["detail"].strip()
        detail = detail[:1500] + (" …" if len(detail) > 1500 else "")
        out += (f"\n\nFull verifier output for the latest rejection (attempt "
                f"{last['iteration']}, gate `{last['reason']}`):\n```\n{detail}\n```")
    return out


def plateau_note(attempts):
    """Fixed nudge after PLATEAU_AFTER rejects in a row (research P7)."""
    streak = 0
    for a in reversed(attempts):
        if a["verdict"] == "accept":
            break
        streak += 1
    if streak < PLATEAU_AFTER:
        return ""
    return (f"\n**The last {streak} attempts were all rejected.** Try a structurally different "
            "approach from everything in the table above (different data layout, different "
            "loop structure, a different opcode in the list) rather than a variation of them.\n")


def render_prompt(meta, run_dir, attempts, current):
    t = TARGETS[meta["target"]]
    wt = run_dir / "wt"
    src = (wt / t["file"]).read_text()
    pinned = json.loads((HERE / "baselines.json").read_text())["opcodes"]
    approx = (f"Exception: {', '.join(t['approx'])} call `tanh`/`tan`; for these only, an "
              "approximation is allowed if (a) you mark it with a `// simd: approx` comment naming "
              "the replaced function, and (b) its peak error stays at or below -100 dBFS "
              "(about 1e-5 absolute) on every stimulus." if t["approx"] else
              "No approximations are allowed for these opcodes.")
    table = "\n".join(
        f"| `{n}` | {pinned[n]} | {current[n]} | {pinned[n] / current[n]:.2f}x |"
        for n in t["bench"])
    accepted = sh(["git", "diff", meta["base_rev"], "HEAD"], cwd=wt).stdout
    if accepted:
        if len(accepted) > ACCEPTED_DIFF_LIMIT:
            accepted = accepted[:ACCEPTED_DIFF_LIMIT] + "\n… (truncated; read the file for the rest)"
        accepted = f"```diff\n{accepted}```"
    else:
        accepted = "Nothing yet — the tree is the original scalar code."
    return string.Template((HERE / "prompts" / "optimize_opcode.md").read_text()).substitute(
        opcodes="\n".join(f"- `{n}`" for n in t["bench"]),
        file=t["file"],
        sources="\n\n".join(extract_function(src, n) for n in t["bench"]),
        baseline=table,
        accepted=accepted,
        allowlist="`cedar/include/cedar/opcodes/**`, `cedar/include/cedar/dsp/simd*.hpp`, "
                  "`cedar/src/dsp/simd*.cpp`",
        approx_policy=approx,
        check_cmd=meta["check_cmd"],
        max_checks=verify.MAX_CHECKS,
        minutes=ITERATION_S // 60,
        history=history_table(attempts),
        plateau=plateau_note(attempts),
    )


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
    pinned = json.loads((HERE / "baselines.json").read_text())["opcodes"]
    current = {n: pinned[n] for n in TARGETS[a.target]["bench"]}
    attempts, spent, status = [], 0.0, "complete"
    wt = run_dir / "wt"
    for it in range(1, a.iterations + 1):
        prompt = render_prompt(meta, run_dir, attempts, current)
        (run_dir / f"prompt-{it}.md").write_text(prompt)
        (run_dir / "check_calls").write_text("0")
        print(f"[{run_id}] iteration {it}/{a.iterations}: proposing ...", file=sys.stderr)
        u = backend.propose(prompt, wt, str(check), a.budget_usd - spent, ITERATION_S)
        (run_dir / f"transcript-{it}.txt").write_text(u.pop("transcript", "") or "")
        idea = parse_idea(u.pop("final_text", ""))
        spent += u["cost_usd"] or 0.0
        sh(["git", "add", "-A", "--", "."], cwd=wt)  # stage new files too, for the diff
        diff = sh(["git", "diff", "--cached", "HEAD"], cwd=wt).stdout
        dup = find_duplicate(diff, attempts)
        if dup:
            v = {"verdict": "reject", "reject_reason": "duplicate",
                 "detail": f"patch is >= {DUP_RATIO:.0%} identical to rejected attempt {dup}; "
                           "not re-verified", "gates": {}, "bench": None, "unstable": False}
        else:
            print(f"[{run_id}] iteration {it}: verifying ...", file=sys.stderr)
            v = verify.verify(run_dir, meta)
        if u.get("error") and v["reject_reason"] == "empty patch":
            v["reject_reason"], v["detail"] = "backend error", u["error"]
        sha = None
        if v["verdict"] == "accept":
            (run_dir / "accepted" / f"iter-{it}.patch").write_text(diff)
            speed = ", ".join(f"{n} {b['speedup']}x" for n, b in v["bench"].items())
            sh(["git", "commit", "-q", "-m",
                f"autoresearch({a.target}): iteration {it} by {a.model}\n\n"
                f"{idea or '(no IDEA line)'}\n\n{speed}"], cwd=wt)
            sha = sh(["git", "rev-parse", "HEAD"], cwd=wt).stdout.strip()
            verify.refresh_speed_baseline(run_dir)
            current.update({n: b["candidate_ns"] for n, b in v["bench"].items()})
        else:
            if diff:
                (run_dir / "rejected" / f"iter-{it}.patch").write_text(diff)
            verify.reset_wt(run_dir)
        checks = int((run_dir / "check_calls").read_text() or 0)
        rec = {
            "run_id": run_id, "iteration": it, "model": a.model, "provider": backend.provider,
            "target": a.target, "opcodes": TARGETS[a.target]["bench"], "isa_target": "avx2",
            **{k: u.get(k) for k in ("tokens", "cost_usd", "wall_clock_s", "gpu_s", "turns",
                                     "model_resolved", "capability_gaps", "error")},
            "check_calls": checks, "idea": idea,
            "verdict": v["verdict"], "reject_reason": v["reject_reason"],
            "reject_detail": (v["detail"] or "")[:4000], "gates": v["gates"],
            "bench": v["bench"], "unstable": v["unstable"],
            "patch_sha": sha, "branch": branch,
        }
        with log.open("a") as f:
            f.write(json.dumps(rec) + "\n")
        attempts.append({"iteration": it, "verdict": v["verdict"], "reason": v["reject_reason"],
                         "detail": v["detail"], "bench": v["bench"], "idea": idea,
                         "norm_diff": normalise_diff(diff)})
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
