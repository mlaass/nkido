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

    def propose(self, prompt, wt, check_cmd, budget_usd):
        cmd = ["claude", "-p", "--model", self.model, "--output-format", "json",
               "--safe-mode", "--strict-mcp-config", "--no-session-persistence",
               "--tools", "Read,Edit,Write,Glob,Grep,Bash",
               "--allowedTools", "Read", "Edit", "Write", "Glob", "Grep", f"Bash({check_cmd})",
               "--permission-mode", "dontAsk",
               "--max-budget-usd", f"{max(budget_usd, 0.01):.2f}"]
        if self.effort:
            cmd += ["--effort", self.effort]
        t0 = time.time()
        r = subprocess.run(cmd, input=prompt, cwd=wt, capture_output=True, text=True,
                           timeout=3600)
        wall = time.time() - t0
        try:
            out = json.loads(r.stdout)
        except json.JSONDecodeError:
            return {"tokens": {"input": 0, "output": 0, "cached": 0}, "cost_usd": 0.0,
                    "wall_clock_s": round(wall, 1), "gpu_s": None,
                    "error": f"claude exited {r.returncode}: {(r.stderr or r.stdout)[-800:]}",
                    "transcript": r.stdout + r.stderr}
        u = out.get("usage", {})
        return {
            "tokens": {"input": u.get("input_tokens", 0) + u.get("cache_creation_input_tokens", 0)
                                + u.get("cache_read_input_tokens", 0),
                       "output": u.get("output_tokens", 0),
                       "cached": u.get("cache_read_input_tokens", 0)},
            "cost_usd": out.get("total_cost_usd", 0.0),
            "wall_clock_s": round(wall, 1), "gpu_s": None,
            "turns": out.get("num_turns"),
            "model_resolved": list(out.get("modelUsage", {}) or {}) or None,
            "error": out.get("result") if out.get("is_error") else None,
            "transcript": out.get("result", ""),
        }
