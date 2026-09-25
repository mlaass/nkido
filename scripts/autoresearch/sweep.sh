#!/usr/bin/env bash
# Full model matrix (docs/prd-simd-autoresearch.md §14 steps 4-5), one run at a
# time: every run shares the pinned bench core. Re-running skips (model, target)
# pairs that already have a summary.json. OpenRouter runs get a $2 per-run cap
# and stop once all OpenRouter runs together have spent OR_CAP_USD.
#   OPENROUTER_API_KEY=... scripts/autoresearch/sweep.sh [backend:model ...]
# Pass model specs to run one lane per model in parallel: runs have their own
# worktree + branch, and verify.machine_lock serialises every build and bench.
set -u
cd "$(dirname "$0")"
OR_CAP_USD=${OR_CAP_USD:-30}
TARGETS="arith distort formant svf freeverb"
MODELS=${*:-"claude-code:opus claude-code:sonnet openrouter:z-ai/glm-5.3
openrouter:deepseek/deepseek-v4-pro-0813 openrouter:qwen/qwen3.8-27b local:qwen3:14b-32k"}

or_spent() {  # USD over every OpenRouter iteration logged so far
  cat runs/*/iterations.jsonl 2>/dev/null | python3 -c '
import json, sys
print(sum(r.get("cost_usd") or 0 for r in map(json.loads, sys.stdin) if r["provider"] == "openrouter"))'
}

for spec in $MODELS; do
  backend=${spec%%:*} model=${spec#*:}
  slug=$(echo "$model" | tr 'A-Z' 'a-z' | sed -E 's/[^a-z0-9]+/-/g; s/^-|-$//g')
  for t in $TARGETS; do
    done_run=$(grep -l '"iterations": 8' runs/*_"${slug}_$t"/run.json 2>/dev/null \
      | while read -r f; do [ -f "${f%run.json}summary.json" ] && echo "$f"; done)
    if [ -n "$done_run" ]; then  # the 2026-09-24 spikes ran 2-3 iterations: not done
      echo "== skip $model $t (done)"; continue
    fi
    extra=()
    if [ "$backend" = openrouter ]; then
      spent=$(or_spent)
      if python3 -c "import sys; sys.exit(float('$spent') < $OR_CAP_USD)"; then
        echo "== skip $model $t: OpenRouter total \$$spent >= \$$OR_CAP_USD"; continue
      fi
      extra=(--budget-usd 2)
    fi
    echo "== $(date -u +%FT%TZ) $backend $model $t"
    ./run.py --backend "$backend" --model "$model" --target "$t" "${extra[@]}" \
      || echo "== FAILED $model $t (exit $?)"
  done
done
echo "== sweep done $(date -u +%FT%TZ)"
