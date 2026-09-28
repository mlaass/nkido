#!/usr/bin/env python3
"""Roll runs back to before their first access/quota failure so sweep.sh resumes them.

    rollback.py [GLOB]   # run-dir glob under runs/, default: every run

A key/budget 403, a credits 402, a free-tier 429 or a Codex usage limit makes
every remaining iteration fail in seconds. Those records are junk, not model
results: this moves the failed tail to iterations.403.jsonl (kept for the
record), drops its prompt/transcript/rejected files and summary.json, and
re-adds the worktree if the finished run removed it. Then `sweep.sh` (or
`run.py --resume`) redoes those iterations. A run whose baseline was never
completed cannot resume; archive it to runs/_aborted/ instead.
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
import verify

BAD = ("Key limit", "Budget limit exceeded", "usage limit", "HTTP 402",
       "Rate limit exceeded: free-models")


def main():
    pattern = sys.argv[1] if len(sys.argv) > 1 else "*_*"
    for rd in sorted((HERE / "runs").glob(pattern)):
        log = rd / "iterations.jsonl"
        if not log.exists():
            continue
        recs = [json.loads(l) for l in log.read_text().splitlines()]
        bad = [i for i, r in enumerate(recs) if any(s in (r.get("error") or "") for s in BAD)]
        if not bad:
            continue
        k = bad[0]
        if bad != list(range(k, len(recs))):
            print(f"{rd.name}: failures are not a contiguous tail, skipped (fix by hand)")
            continue
        with (rd / "iterations.403.jsonl").open("a") as f:
            f.writelines(json.dumps(r) + "\n" for r in recs[k:])
        log.write_text("".join(json.dumps(r) + "\n" for r in recs[:k]))
        for r in recs[k:]:
            for p in (f"prompt-{r['iteration']}.md", f"transcript-{r['iteration']}.txt"):
                (rd / p).unlink(missing_ok=True)
            (rd / "rejected" / f"iter-{r['iteration']}.patch").unlink(missing_ok=True)
        (rd / "summary.json").unlink(missing_ok=True)
        meta = json.loads((rd / "run.json").read_text())
        if not (rd / "wt").exists():
            r = verify.sh(["git", "worktree", "add", "--quiet", str(rd / "wt"), meta["branch"]],
                          cwd=verify.REPO)
            if r.returncode:
                raise SystemExit(r.stderr)
            verify.configure(rd / "wt")
        print(f"{rd.name}: kept {k}, dropped {len(recs) - k}")


if __name__ == "__main__":
    main()
