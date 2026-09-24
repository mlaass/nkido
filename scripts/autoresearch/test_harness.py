#!/usr/bin/env python3
"""Self-check for the harness logic that is not exercised by verify.py --selftest."""
import tempfile
from pathlib import Path

import report
import run
from backends.openai_compat import Workspace

SRC = """
[[gnu::always_inline]]
inline void op_a(int x) { if (x) { return; } }
inline void op_b() {}
"""
assert run.extract_function(SRC, "op_a").endswith("{ if (x) { return; } }")
assert run.extract_function(SRC, "op_b") == "inline void op_b() {}"
assert "not found" in run.extract_function(SRC, "op_c")

with tempfile.TemporaryDirectory() as d:
    ws = Workspace(d, "true")
    assert ws.call("write_file", {"path": "a/b.txt", "content": "x\nx\ny\n"}).startswith("wrote")
    assert "outside" in ws.call("write_file", {"path": "../escape.txt", "content": ""})
    assert "outside" in ws.call("read_file", {"path": "/etc/passwd"})
    assert "2 times" in ws.call("edit_file", {"path": "a/b.txt", "old_string": "x", "new_string": "z"})
    assert ws.call("edit_file", {"path": "a/b.txt", "old_string": "y", "new_string": "w"}).startswith("edited")
    assert (Path(d) / "a/b.txt").read_text() == "x\nx\nw\n"

recs = [
    {"model": "m", "provider": "p", "target": "arith", "iteration": 1, "verdict": "reject",
     "reject_reason": "equality", "tokens": {"input": 10, "output": 2, "cached": 5}, "cost_usd": 0.5},
    {"model": "m", "provider": "p", "target": "arith", "iteration": 2, "verdict": "accept",
     "reject_reason": None, "tokens": {"input": 20, "output": 4, "cached": 0}, "cost_usd": 1.0,
     "bench": {"op_add": {"speedup": 2.0}, "op_mul": {"speedup": 1.5, "speedup_vs_origin": 8.0}}},
]
s = report.summarise("r", recs)
assert s["accepts"] == 1 and s["first_accept_iteration"] == 2
assert s["tokens"] == {"input": 30, "output": 6, "cached": 5} and s["cost_usd"] == 1.5
assert s["best_speedup"] == 4.0  # geomean of 2.0 and 8.0 (vs origin wins)
assert s["reject_reasons"] == {"equality": 1}
print("test_harness: ok")
