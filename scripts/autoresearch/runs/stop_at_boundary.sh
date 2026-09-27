#!/usr/bin/env bash
# one-off: stop each in-flight run right after its current iteration is logged
# (the log line is written after the commit/revert, so the tree is clean).
cd "$(dirname "$0")"
for d in 2026-09-25T19*/; do
  d=${d%/}; [ -f "$d/summary.json" ] && continue
  (
    n=$(wc -l < "$d/iterations.jsonl" 2>/dev/null || echo 0)
    slug_target=${d#*_}
    pid=$(ps -eo pid,cmd | awk -v t="--target ${slug_target##*_}" 'index($0, "run.py") && index($0, t)' \
          | while read -r p rest; do
              m=$(echo "$rest" | sed -E 's/.*--model ([^ ]+).*/\1/' | tr 'A-Z' 'a-z' | sed -E 's/[^a-z0-9]+/-/g; s/^-|-$//g')
              [ "${m}_${slug_target##*_}" = "$slug_target" ] && echo "$p"
            done)
    [ -z "$pid" ] && { echo "$d: no process"; exit; }
    pgid=$(ps -o pgid= -p "$pid" | tr -d ' ')
    until [ "$(wc -l < "$d/iterations.jsonl" 2>/dev/null || echo 0)" -gt "$n" ]; do sleep 1; done
    if [ "$(wc -l < "$d/iterations.jsonl")" -ge 8 ]; then echo "$d: last iteration, left to finish"; exit; fi
    kill -TERM -- "-$pgid"
    echo "$(date -u +%T) $d: stopped after iteration $(wc -l < "$d/iterations.jsonl") (pid $pid, pgid $pgid)"
  ) &
done
wait
echo "all stopped"
