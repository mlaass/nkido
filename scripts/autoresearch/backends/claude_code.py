"""Claude Code headless backend: `claude -p`, usage from its JSON result.

--safe-mode keeps the user's CLAUDE.md, hooks, skills, plugins and MCP
servers out of the experiment (auth still works), so Claude sees the same
prompt as every other backend. Tools: file read/write/search plus Bash
restricted to the one check command.
"""
import json
import subprocess
import time


class ClaudeCode:
    provider = "claude-code"

    def __init__(self, model, effort=None):
        self.model = model
        self.effort = effort

    def propose(self, prompt, wt, check_cmd, budget_usd, timeout_s):
        cmd = ["claude", "-p", "--model", self.model,
               "--output-format", "stream-json", "--verbose",  # full tool-call transcript
               "--safe-mode", "--strict-mcp-config", "--no-session-persistence",
               "--tools", "Read,Edit,Write,Glob,Grep,Bash",
               "--allowedTools", "Read", "Edit", "Write", "Glob", "Grep",
               f"Bash({check_cmd})", f"Bash({check_cmd}:*)",
               "--permission-mode", "dontAsk",
               "--max-budget-usd", f"{max(budget_usd, 0.01):.2f}"]
        if self.effort:
            cmd += ["--effort", self.effort]
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
        events = []
        for line in stream.splitlines():
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                pass
        result = next((e for e in reversed(events) if e.get("type") == "result"), None)
        denied = [d for e in events if e.get("type") == "result"
                  for d in e.get("permission_denials") or []]
        gaps = [f"tool call denied: {json.dumps(d.get('tool_input', d))[:200]}" for d in denied]
        if result is None:
            # Timed out or crashed: sum what the streamed assistant turns reported.
            tok = {"input": 0, "output": 0, "cached": 0}
            for e in events:
                u = (e.get("message") or {}).get("usage") if e.get("type") == "assistant" else None
                if u:
                    tok["input"] += (u.get("input_tokens", 0) + u.get("cache_creation_input_tokens", 0)
                                     + u.get("cache_read_input_tokens", 0))
                    tok["output"] += u.get("output_tokens", 0)
                    tok["cached"] += u.get("cache_read_input_tokens", 0)
            err = (f"wall-clock cap of {timeout_s} s hit" if timed_out
                   else f"claude exited {rc}: {(stderr or stream)[-800:]}")
            return {"tokens": tok, "cost_usd": None, "wall_clock_s": wall, "gpu_s": None,
                    "error": err + "; cost unknown (no result event)", "final_text": "",
                    "capability_gaps": gaps or None, "transcript": stream}
        u = result.get("usage", {})
        return {
            "tokens": {"input": u.get("input_tokens", 0) + u.get("cache_creation_input_tokens", 0)
                                + u.get("cache_read_input_tokens", 0),
                       "output": u.get("output_tokens", 0),
                       "cached": u.get("cache_read_input_tokens", 0)},
            "cost_usd": result.get("total_cost_usd", 0.0),
            "wall_clock_s": wall, "gpu_s": None,
            "turns": result.get("num_turns"),
            "model_resolved": list(result.get("modelUsage", {}) or {}) or None,
            "capability_gaps": gaps or None,
            "error": result.get("result") if result.get("is_error") else None,
            "final_text": result.get("result", ""),
            "transcript": stream,
        }
