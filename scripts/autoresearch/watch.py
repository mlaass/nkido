#!/usr/bin/env python3
"""Poll run logs for a monitor: one line per new iteration (access/usage errors
flagged), one per finished run, and a final line when the lanes exit.

    watch.py GLOB LANE   # e.g. watch.py "2026-09-2[89]T*" "sweep.sh codex"
"""
import glob, json, subprocess, sys, time
from pathlib import Path

RUNS = Path(__file__).resolve().parent / "runs"
GLOB, LANE = sys.argv[1], sys.argv[2]
BAD = ("turn failed", "usage limit", "rate limit", "http 4", "exited", "quota")
seen = set()
first = True
while True:
    for f in sorted(glob.glob(f"{RUNS}/{GLOB}/iterations.jsonl")):
        for n, line in enumerate(open(f)):
            if (f, n) in seen:
                continue
            seen.add((f, n))
            if first:
                continue  # don't replay history at startup
            r = json.loads(line)
            e = r.get("error") or ""
            flag = f"  ERROR {e[:200]}" if any(b in e.lower() for b in BAD) else ""
            print(f"{r['run_id']} it{r['iteration']}: {r['verdict']} {r['reject_reason'] or ''}{flag}",
                  flush=True)
    for s in sorted(glob.glob(f"{RUNS}/{GLOB}/summary.json")):
        if s not in seen:
            seen.add(s)
            if not first:
                d = json.load(open(s))
                print(f"RUN DONE {d['run_id']}: {d['accepts']}/{d['iterations']} best {d['best_speedup']}",
                      flush=True)
    first = False
    lanes = subprocess.run(["ps", "-eo", "args"], capture_output=True, text=True).stdout
    if not any(l.startswith("bash ./" + LANE) for l in lanes.splitlines()):
        print("ALL LANES EXITED", flush=True)
        break
    time.sleep(60)
