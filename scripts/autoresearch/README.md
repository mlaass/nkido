# SIMD autoresearch

Harness for `docs/prd-simd-autoresearch.md`: models propose SIMD kernels for
fixed Cedar opcodes; gates accept or reject; tokens and speedups are logged.
Python scripts re-exec under `experiments/.venv` (numpy, matplotlib).

```bash
# one-time: build the gate config (Release + tests + CEDAR_SIMD)
cmake --preset simd && cmake --build build/simd -j

# Phase 0 — pinned baselines + C5 ranking (i7-12700KF, P-core 2, governor performance)
scripts/autoresearch/bench.py --pin --baseline build/release/bin/cedar_bench

# Phase 1 — prove the gates: good kernel accepted; lane swap, slower kernel,
# benchmark edit, undeclared tanh approximation and a kernel that is only
# exact on the fixed stimuli rejected
scripts/autoresearch/verify.py --selftest

# Phase 2/3 — one (model, target) run, 8 proposals, commits on autoresearch/* only
scripts/autoresearch/run.py --backend claude-code --model opus   --target formant
scripts/autoresearch/run.py --backend openrouter  --model z-ai/glm-5.3 --target svf   # OPENROUTER_API_KEY
scripts/autoresearch/run.py --backend local       --model qwen3:14b --target distort  # Ollama :11434

# reported legs for an accepted run
scripts/autoresearch/legs.py --run scripts/autoresearch/runs/<id> --wasm --remote <mac-host>

# talk table + chart from every runs/*/iterations.jsonl
scripts/autoresearch/report.py --out results.md
```

Prompt framework: `prompts/optimize_opcode.md`, shaped by the survey in
`docs/research/llm-autoresearch-loops.md`. Each attempt is a fresh session.
Memory between attempts lives in the prompt: current vs original ns, the
accepted diff, and an attempts table built from each agent's `IDEA:` line.
Every attempt, on every backend, gets the same budget: 30 min wall clock and
10 `check.sh` calls. The quick check prints GCC vectoriser remarks. The full
verify also checks bit-identity on a fresh random hidden-stimulus seed
(`cedar_bench --seed N`) that the model never sees. Near-duplicate rejected
patches are skipped without verifying, and three rejects in a row add a
fixed "try something structurally different" line.

Targets (`verify.TARGETS`): `arith` (C1), `distort` (C2), `formant` (C3),
`svf` (C4), `freeverb` (C5, top of the Phase 0 ranking).

Files a model may edit: `cedar/include/cedar/opcodes/**`,
`cedar/include/cedar/dsp/simd*.hpp`, `cedar/src/dsp/simd*.cpp`. Anything else
is an automatic reject.

Local models: Ollama's server default context is 4096 tokens, which truncates
the ~5k-token prompt (logged as a `context truncated` capability gap). Start
the server with `OLLAMA_CONTEXT_LENGTH=32768` for a fair run. LocalAI:
`AUTORESEARCH_LOCAL_URL=http://localhost:8069/v1`.

`scripts/autoresearch/test_harness.py` checks the helper logic that the
selftest does not reach.
