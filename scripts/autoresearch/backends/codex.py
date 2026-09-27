"""Codex CLI headless backend: `codex exec --json` on the ChatGPT subscription.

--ignore-user-config and --ephemeral keep the user's Codex config, rules and
session history out of the experiment (auth still works). Unlike claude -p,
Codex cannot restrict its shell to the one check command, so it runs in the
workspace-write sandbox (worktree + runs/ for check.sh and the machine lock
+ the ccache dir for the build) and every command other than check.sh is
logged as a capability gap, so the report shows any extra tool use.
Subscription usage has no per-call price: cost_usd is None.
"""
import json
import subprocess
import time
from pathlib import Path

RUNS = Path(__file__).resolve().parent.parent / "runs"
CCACHE_DIR = Path.home() / ".cache" / "ccache"
MAX_LOGGED_CMDS = 20


class Codex:
    provider = "codex"

    def __init__(self, model, effort="medium"):
        self.model = model
        self.effort = effort or "medium"

    def propose(self, prompt, wt, check_cmd, budget_usd, timeout_s):
        cmd = ["codex", "exec", "--json", "--ephemeral", "--ignore-user-config",
               "-m", self.model, "-c", f'model_reasoning_effort="{self.effort}"',
               "-c", 'approval_policy="never"', "-s", "workspace-write",
               "-C", str(wt), "--add-dir", str(RUNS), "--add-dir", str(CCACHE_DIR)]
        t0 = time.time()
        timed_out = False
        try:
            r = subprocess.run(cmd, input=prompt, cwd=wt, capture_output=True, text=True,
                               errors="replace", timeout=timeout_s)
            stream, stderr, rc = r.stdout, r.stderr, r.returncode
        except subprocess.TimeoutExpired as e:
            timed_out = True
            raw = e.stdout or b""
            stream = raw.decode(errors="replace") if isinstance(raw, bytes) else raw
            stderr, rc = "", None
        wall = round(time.time() - t0, 1)
        tok = {"input": 0, "output": 0, "cached": 0}
        final, tool_calls, extra, failure = "", 0, [], None
        for line in stream.splitlines():
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            t, item = e.get("type"), e.get("item") or {}
            if t == "turn.completed":
                u = e.get("usage") or {}
                tok["input"] += u.get("input_tokens", 0)
                tok["output"] += u.get("output_tokens", 0)
                tok["cached"] += u.get("cached_input_tokens", 0)
            elif t in ("turn.failed", "error"):
                failure = json.dumps(e.get("error") or e.get("message") or e)[:800]
            elif t == "item.completed" and item.get("type") == "agent_message":
                final = item.get("text") or final
            elif t == "item.completed" and item.get("type") in ("command_execution", "file_change"):
                tool_calls += 1
                c = item.get("command") or ""
                if item["type"] == "command_execution" and check_cmd not in c:
                    extra.append(c)
        gaps = [f"shell command outside check.sh: {c[:200]}" for c in extra[:MAX_LOGGED_CMDS]]
        if len(extra) > MAX_LOGGED_CMDS:
            gaps.append(f"... {len(extra) - MAX_LOGGED_CMDS} more commands outside check.sh")
        if timed_out:
            err = f"wall-clock cap of {timeout_s} s hit"
        elif failure:
            err = f"codex turn failed: {failure}"
        elif rc:
            err = f"codex exited {rc}: {(stderr or stream)[-800:]}"
        else:
            err = None
        return {"tokens": tok, "cost_usd": None, "wall_clock_s": wall, "gpu_s": None,
                "turns": tool_calls, "model_resolved": [self.model],
                "capability_gaps": gaps or None, "error": err,
                "final_text": final, "transcript": stream}
