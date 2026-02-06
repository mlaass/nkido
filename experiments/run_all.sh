#!/usr/bin/env bash
# Run all test_op_*.py experiments.
# Usage: ./run_all.sh [--stop-on-error]

set -euo pipefail
cd "$(dirname "$0")"

stop_on_error=false
[[ "${1:-}" == "--stop-on-error" ]] && stop_on_error=true

passed=0
failed=0
failures=()

for f in test_op_*.py; do
    echo "=== $f ==="
    if uv run python "$f"; then
        ((passed++)) || true
    else
        ((failed++)) || true
        failures+=("$f")
        if $stop_on_error; then
            echo "STOPPED: $f failed"
            exit 1
        fi
    fi
    echo
done

echo "==============================="
echo "Results: $passed passed, $failed failed ($(( passed + failed )) total)"
if (( failed > 0 )); then
    echo "Failures:"
    for f in "${failures[@]}"; do
        echo "  - $f"
    done
    exit 1
fi
