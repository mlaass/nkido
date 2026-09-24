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
# ideas ledger, duplicate filter, attempts table, plateau switch (research P1/P3/P7/P8)
assert run.parse_idea("blah\nIDEA: serial recurrence -> 2-ch fuse (1.3x)\n") == \
    "serial recurrence -> 2-ch fuse (1.3x)"
assert run.parse_idea("`IDEA: a`\nIDEA: b") == "b" and run.parse_idea("no line") is None
d1 = "--- a/x\n+++ b/x\n@@\n+  int   a = 1;\n-int b;\n context\n"
d2 = "--- a/x\n+++ b/x\n@@ -9 +9 @@\n+int a = 1;\n- int b;\n"
assert run.normalise_diff(d1) == run.normalise_diff(d2) == "+int a = 1;\n-int b;"
rej = {"iteration": 1, "verdict": "reject", "reason": "speed", "detail": "no gain\nmore",
       "bench": {"op_mul": {"speedup": 1.0}}, "idea": "mul | avx2", "norm_diff": run.normalise_diff(d1)}
acc = {**rej, "iteration": 2, "verdict": "accept", "reason": None, "detail": ""}
assert run.find_duplicate(d2, [rej]) == 1 and run.find_duplicate(d2, [acc]) is None
assert run.find_duplicate("+totally different\n", [rej]) is None
t = run.history_table([rej, acc])
assert "| 1 | rejected: `speed` — no gain | mul / avx2 | mul 1.0x |" in t and "ACCEPTED" in t
assert "Full verifier output for the latest rejection (attempt 1" in t
r3 = [{**rej, "iteration": i} for i in (3, 4, 5)]
assert run.plateau_note([acc, *r3]) and not run.plateau_note([*r3[:2], acc]) \
    and not run.plateau_note(r3[:2])

print("test_harness: ok")
