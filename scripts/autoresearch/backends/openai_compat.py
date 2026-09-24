"""OpenAI-compatible tool loop shared by the OpenRouter and local backends.

The tool surface mirrors what Claude Code gets (Read / Write / Edit / Glob /
Grep / the one check command), so the harness is the same for every model.
Anything the endpoint cannot do — no tool calling, truncated context — is
recorded in `capability_gaps`, never worked around with a different harness
(docs/prd-simd-autoresearch.md §4.1).

stdlib only (urllib): no new dependency.
"""
import fnmatch
import json
import re
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path

MAX_TURNS = 60
TOOL_OUTPUT_LIMIT = 20000

SYSTEM = ("You are an expert C++ performance engineer working in a git checkout. Use the tools "
          "to read and edit files and to run the check command. Make your edits with the tools; "
          "code written only in your reply is not applied. Stop when you are done.")

TOOLS = [
    {"type": "function", "function": {
        "name": "read_file", "description": "Read a file (relative to the repo root) with line numbers.",
        "parameters": {"type": "object", "properties": {
            "path": {"type": "string"},
            "offset": {"type": "integer", "description": "1-based first line"},
            "limit": {"type": "integer", "description": "max lines (default 2000)"}},
            "required": ["path"]}}},
    {"type": "function", "function": {
        "name": "write_file", "description": "Create or overwrite a file with the given content.",
        "parameters": {"type": "object", "properties": {
            "path": {"type": "string"}, "content": {"type": "string"}},
            "required": ["path", "content"]}}},
    {"type": "function", "function": {
        "name": "edit_file",
        "description": "Replace one exact, unique occurrence of old_string with new_string.",
        "parameters": {"type": "object", "properties": {
            "path": {"type": "string"}, "old_string": {"type": "string"},
            "new_string": {"type": "string"}},
            "required": ["path", "old_string", "new_string"]}}},
    {"type": "function", "function": {
        "name": "list_files", "description": "List files matching a glob, e.g. cedar/include/**/*.hpp",
        "parameters": {"type": "object", "properties": {"pattern": {"type": "string"}},
                       "required": ["pattern"]}}},
    {"type": "function", "function": {
        "name": "grep", "description": "Search file contents with a regex; returns path:line: text.",
        "parameters": {"type": "object", "properties": {
            "pattern": {"type": "string"},
            "glob": {"type": "string", "description": "optional file glob filter"}},
            "required": ["pattern"]}}},
    {"type": "function", "function": {
        "name": "run_check",
        "description": "Build, check allowlist + bit-identity for every opcode, short benchmark.",
        "parameters": {"type": "object", "properties": {}}}},
]


class Workspace:
    def __init__(self, root, check_cmd):
        self.root = Path(root).resolve()
        self.check_cmd = check_cmd

    def _path(self, p):
        full = (self.root / p).resolve()
        if self.root != full and self.root not in full.parents:
            raise ValueError(f"{p}: outside the repository")
        return full

    def _files(self):
        out = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard"],
                             cwd=self.root, capture_output=True, text=True).stdout
        return out.split()

    def call(self, name, args):
        try:
            if name == "read_file":
                lines = self._path(args["path"]).read_text(errors="replace").splitlines()
                start = max(int(args.get("offset") or 1), 1)
                lim = int(args.get("limit") or 2000)
                return "\n".join(f"{i:6d}\t{l}" for i, l in
                                 enumerate(lines[start - 1:start - 1 + lim], start)) or "(empty)"
            if name == "write_file":
                f = self._path(args["path"])
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(args["content"])
                return f"wrote {args['path']}"
            if name == "edit_file":
                f = self._path(args["path"])
                s = f.read_text()
                n = s.count(args["old_string"])
                if n != 1:
                    return f"error: old_string found {n} times (must be exactly once)"
                f.write_text(s.replace(args["old_string"], args["new_string"]))
                return f"edited {args['path']}"
            if name == "list_files":
                pat = args["pattern"]
                hits = [f for f in self._files() if fnmatch.fnmatch(f, pat)]
                return "\n".join(hits[:500]) or "(no matches)"
            if name == "grep":
                rx = re.compile(args["pattern"])
                g = args.get("glob")
                out = []
                for f in self._files():
                    if g and not fnmatch.fnmatch(f, g):
                        continue
                    try:
                        for i, l in enumerate((self.root / f).read_text().splitlines(), 1):
                            if rx.search(l):
                                out.append(f"{f}:{i}: {l}")
                    except (UnicodeDecodeError, OSError):
                        continue
                    if len(out) > 300:
                        break
                return "\n".join(out) or "(no matches)"
            if name == "run_check":
                r = subprocess.run([self.check_cmd], capture_output=True, text=True, timeout=1800)
                return (r.stdout + r.stderr).strip()
            return f"error: unknown tool {name}"
        except Exception as e:  # tool errors go back to the model, like Claude Code's
            return f"error: {e}"


class OpenAICompat:
    provider = "openai-compat"
    base_url = ""
    local = False  # local endpoints: USD 0, request time reported as gpu_s

    def __init__(self, model):
        self.model = model

    def headers(self):
        return {"Content-Type": "application/json"}

    def extra_body(self):
        return {}

    def post(self, body):
        req = urllib.request.Request(self.base_url + "/chat/completions",
                                     data=json.dumps(body).encode(), headers=self.headers())
        with urllib.request.urlopen(req, timeout=1800) as r:
            return json.loads(r.read())

    def propose(self, prompt, wt, check_cmd, budget_usd):
        ws = Workspace(wt, check_cmd)
        msgs = [{"role": "system", "content": SYSTEM}, {"role": "user", "content": prompt}]
        tok = {"input": 0, "output": 0, "cached": 0}
        cost, gaps, err, t_req, turns, tool_calls = 0.0, [], None, 0.0, 0, 0
        t0 = time.time()
        for turns in range(1, MAX_TURNS + 1):
            body = {"model": self.model, "messages": msgs, "tools": TOOLS,
                    "tool_choice": "auto", **self.extra_body()}
            ts = time.time()
            try:
                resp = self.post(body)
            except urllib.error.HTTPError as e:
                text = e.read().decode(errors="replace")[:800]
                if "tool" in text.lower() and turns == 1:
                    gaps.append(f"endpoint rejected tool calling: {text[:200]}")
                err = f"HTTP {e.code}: {text}"
                break
            except (urllib.error.URLError, TimeoutError) as e:
                err = f"request failed: {e}"
                break
            finally:
                t_req += time.time() - ts
            u = resp.get("usage") or {}
            tok["input"] += u.get("prompt_tokens", 0) or 0
            tok["output"] += u.get("completion_tokens", 0) or 0
            tok["cached"] += ((u.get("prompt_tokens_details") or {}).get("cached_tokens", 0) or 0)
            if u.get("cost") is not None:
                cost += float(u["cost"])
            if turns == 1 and u.get("prompt_tokens") and u["prompt_tokens"] < len(prompt) / 8:
                # ~4 chars/token; less than half of that means the server cut the prompt.
                gaps.append(f"context truncated: prompt reported as {u['prompt_tokens']} tokens "
                            f"for {len(prompt)} chars")
            msg = resp["choices"][0]["message"]
            calls = msg.get("tool_calls") or []
            msgs.append({k: v for k, v in msg.items() if k in ("role", "content", "tool_calls")})
            if not calls:
                if tool_calls == 0:
                    gaps.append("answered without calling any tool (no edits applied)")
                break
            for c in calls:
                tool_calls += 1
                try:
                    args = json.loads(c["function"].get("arguments") or "{}")
                except json.JSONDecodeError as e:
                    out = f"error: arguments are not valid JSON: {e}"
                else:
                    out = ws.call(c["function"]["name"], args)
                msgs.append({"role": "tool", "tool_call_id": c.get("id", ""),
                             "content": out[:TOOL_OUTPUT_LIMIT]})
            if cost >= budget_usd:
                err = "budget exhausted mid-iteration"
                break
        else:
            gaps.append(f"hit the {MAX_TURNS}-turn limit")
        return {"tokens": tok, "cost_usd": round(cost, 6), "wall_clock_s": round(time.time() - t0, 1),
                "gpu_s": round(t_req, 1) if self.local else None, "turns": turns,
                "capability_gaps": gaps or None, "error": err,
                "transcript": json.dumps(msgs, indent=1)}
