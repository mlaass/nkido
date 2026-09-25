# PRD: SIMD Autoresearch Loop

> **Status: PARTIAL** — harness for phases 0–3 built and self-tested; the
> model matrix (Phase 4) has not been run (2026-09-24). See §13 for what
> shipped, measured numbers and divergences. Talk plays 16 Oct; final video
> due 2 Oct.

---

## 1. Executive Summary

An automated research loop that optimises hand-picked Cedar DSP opcodes with
SIMD (SSE/AVX2, NEON, WASM SIMD128), runs the same loop across several models —
Claude, cheap hosted models, and local models — and logs both the **speedup
achieved** and the **tokens spent** getting there.

Two outcomes, weighted equally:

1. **Engine work.** Real, merge-quality SIMD kernels for opcodes that are hot
   today, behind runtime dispatch, with the scalar path kept as fallback.
2. **Talk artifact.** The closing section of *Compiling Music* at ADCx Gather:
   a results table, a cost-per-speedup comparison across models, a screen
   capture of the loop working, and at least one change the gates rejected.

The premise the talk rests on: the four-legged memory harness plus a
bit-identity/null-test gate is what makes unattended optimisation safe. A model
proposing SIMD kernels is only interesting because a wrong kernel cannot survive
the gates.

**Decisions locked during design (2026-09-24):**

- Candidate opcodes are **hand-picked and fixed** (§3.1), so results are
  comparable across models. The loop may nominate extra "looks heavy" targets,
  but only the fixed set counts for the headline table.
- Accepted changes are committed to a **scratch branch per run**; master is
  never touched by the loop.
- Gates: bit-identity where achievable, else a **null test within tolerance**;
  plus existing test suites. The memory harness runs as a **final pass on the
  winners only**, not per iteration.
- Transcendental approximations (`tanh`, `tan`) **are allowed** with a
  documented per-opcode exception, tolerance, and a human listening check.
- **Runtime dispatch; no change to default build baselines.** WASM gets
  `-msimd128` in its own preset.
- Each (model, opcode) run gets a **fixed iteration count**, default 8.
- One Python driver, many backends (§4.2).

---

## 2. Problem Statement / Current State

| Today | With this PRD |
|---|---|
| No CPU benchmark anywhere in the repo. `git grep -i benchmark` hits only prose in PRDs. | `cedar_bench` target: median ns/block per opcode, JSON output, stable enough to gate on. |
| No SIMD. No `immintrin.h`, no `-march`, no `-msimd128`; every opcode is a scalar `for (i < BLOCK_SIZE)` loop. | SIMD kernels for the candidate set, selected at runtime, scalar fallback retained. |
| Optimisation opportunities are noted in three PRDs and never measured (`prd-squelch-engine.md` §formant, `prd-stereo-native-opcodes.md` §9.13 + OQ4). | Those exact targets are measured, attempted, and reported — including where SIMD does *not* win. |
| No automation calls an LLM. No agent tooling in `.claude/` beyond one command and one skill. | `scripts/autoresearch/` drives models through an identical propose→build→verify→bench→accept loop. |
| Correctness is guarded by unit tests, the memory harness, and human listening. | Same gates, plus an A/B null test of optimised vs baseline output through the `cedar_core` pybind harness. |

Groundwork that already helps:

- `cedar/include/cedar/vm/buffer_pool.hpp:39` — slabs are `alignas(32)`, rows are
  contiguous `BLOCK_SIZE` floats. AVX-ready today.
- `BLOCK_SIZE = 128` is a clean multiple of every vector width in play.
- `cedar/bindings/bindings.cpp` → `cedar_core` pybind module: the fastest way to
  drive one opcode with fixed input and capture output for A/B comparison.
- `cedar/tests/test_zero_alloc.cpp`, `scripts/memory/run_all.sh`: the safety net
  the talk's argument depends on.
- `scripts/cedar-size-report.sh` is the structural template for "measure across
  configs, emit markdown, compare to baseline".

---

## 3. Goals and Non-Goals

### 3.1 Goals

- **G1.** A per-opcode microbenchmark whose run-to-run noise is small enough that
  a claimed ≥10 % speedup is real (§6.1 defines the stability requirement).
- **G2.** An autoresearch loop that, unattended, produces accepted SIMD kernels
  for these candidates:

  | # | Opcode | File | Shape | Prior estimate |
  |---|---|---|---|---|
  | C1 | `op_mul` / `op_add` / `op_sub` | `cedar/include/cedar/opcodes/arithmetic.hpp` | Pure elementwise, stateless, aligned | Control case; near-linear in lane count |
  | C2 | `op_distort_tanh`, `op_distort_soft` | `cedar/include/cedar/opcodes/distortion.hpp` | Stateless waveshaper, loop-invariant branches | `soft()` exact; `tanh` needs approximation |
  | C3 | `op_filter_formant` | `cedar/include/cedar/opcodes/filters.hpp:439` | 3 serial BPFs → 4 lanes across *filters* | `prd-squelch-engine.md`: "roughly halve" |
  | C4 | `op_filter_svf_lp/_hp/_bp` | `cedar/include/cedar/opcodes/filters.hpp:48` | Feedback-serial in time; only the 2-channel inner loop fuses | `prd-stereo-native-opcodes.md` OQ4; may be a loss |
  | C5 | Opportunistic | any | Whatever the profiling step ranks as heavy | Reported separately from C1–C4 |

- **G3.** The same loop run across the model matrix (§4.1) with identical
  prompts, identical gates and identical iteration budget.
- **G4.** Per-iteration cost logging (tokens in/out, cached, USD, wall clock) and
  a per-run summary, in JSONL, regenerable into the talk's table and chart.
- **G5.** Measurements on Linux x86-64 (gate), Apple Silicon over ssh (NEON),
  and WASM SIMD128 (headless).
- **G6.** Honest reporting of negative results. An opcode where SIMD loses, or a
  model that never produces an accepted change, is a finding, not a failure to
  hide. Budgets and tolerances are never loosened to make a run look better.

### 3.2 Non-Goals

- **Windows.** Dropped from the matrix for this round (was considered; the ssh
  setup cost does not fit before 2 Oct). Future PRD.
- **Vectorising the VM dispatch loop itself**, block-size changes, or
  multithreaded graph execution. Opcode bodies only.
- **Raising the default x86 baseline** (`-march=x86-64-v2/v3`). Runtime dispatch
  instead; revisit in a future PRD if the dispatch overhead proves material.
- **Auto-merging to master.** Scratch branches only; a human merges.
- **Replacing the existing Python experiments** as the DSP quality oracle. They
  stay; the loop adds an A/B null test on top.
- **A general-purpose agent framework.** The driver does one job.

---

## 4. Experiment Design

### 4.1 Model matrix

| Tier | Model | Backend | Cost source |
|---|---|---|---|
| Reference | Claude Opus 5 | Claude Code headless (`claude -p`) | CLI usage JSON |
| Hosted cheap | Claude Sonnet 5 | Claude Code headless | CLI usage JSON |
| Hosted cheap (3rd party) | One GLM, one DeepSeek, one Qwen — coder tiers **[exact ids confirmed against the OpenRouter catalogue in Phase 3]** | OpenRouter API | `usage` field in response |
| Local | One quantized coder model sized to the local GPU **[model chosen in Phase 3 from what is installed]** | LocalAI (Docker, `localhost:8069`) | tokens + GPU seconds, USD = 0 |

Every backend receives the **same prompt template**, the same repo state, the
same tool surface (read files, write files under the allowlist, run the verify
script) and the same iteration budget. Anything a backend cannot do (e.g. no
tool calling) is recorded as a capability gap rather than papered over with a
different harness.

### 4.2 The loop

```
                    ┌──────────────── run: (model, opcode), N=8 iterations ────────────────┐
                    │                                                                      │
  baseline bench ──►│  1. context: opcode source + baseline ns/block + prior attempts       │
  (pinned, §6.1)    │  2. model proposes a patch (opcodes/ + simd/ files only)              │
                    │  3. verify.sh:                                                        │
                    │       a. build (scalar baseline build is cached)                       │
                    │       b. A/B null test through cedar_core: bit-exact? else ≤ tol       │
                    │       c. cedar_tests + akkado_tests + [zero_alloc]                     │
                    │       d. that opcode's experiments/test_op_*.py                         │
                    │  4. bench: median ns/block, N reps, same machine, same governor        │
                    │  5. verdict: accept (all gates green AND faster) | reject (+reason)    │
                    │  6. log iteration JSONL (tokens, cost, wall clock, verdict, speedup)   │
                    │  7. accepted → commit to run branch; rejected → revert, feed reason    │
                    └──────────────────────────────────────────────────────────────────────┘
                                                │
                            winners of all runs ▼
                    final pass: memory harness (sanitizer + drift fuzz) + human listening
```

**Iteration budget:** fixed `N = 8` proposals per (model, opcode), regardless of
cost. Token spend is *measured*, not capped — that is the comparison.

### 4.3 Gates

| Gate | Tool | Pass condition |
|---|---|---|
| Build | `cmake --build build/simd` | Compiles clean, warnings-as-errors as configured |
| Output equality | A/B through `cedar_core`, fixed stimuli (seeded noise, sine sweep, impulse, ramp) | `np.array_equal` (bit-identical) **or**, where an approximation is declared, peak null-test error ≤ **−100 dBFS** and no new NaN/Inf/click per `artifact_metrics.scan_sanity` |
| Unit suites | `cedar_tests`, `akkado_tests` | All green |
| Zero-alloc | `cedar_tests "[zero_alloc]"` | No allocation on the audio path |
| Opcode experiment | `experiments/test_op_<name>.py` | Passes its own assertions |
| Speed | `cedar_bench` | Median ns/block improves by ≥ **5 %** over baseline, outside the noise band (§6.1) |
| **Winners only** | `scripts/memory/run_all.sh`, then human listening to the rendered WAV | Green; no audible difference |

A patch that declares an approximation must say so in a `// simd: approx` comment
naming the function replaced and the measured null-test floor. Approximations
without that declaration are rejected even if they pass the tolerance.

### 4.4 Platforms

| Platform | Role | How |
|---|---|---|
| Linux x86-64 (this machine) | **Gate.** Accept/reject decisions happen here. | Native build, AVX2 + SSE2 paths |
| Apple Silicon (ssh) **[OPEN QUESTION: host + toolchain]** | Reported. NEON result for the portable-wrapper claim. | Remote build + bench of accepted kernels |
| WASM SIMD128 | Reported. Matters most for nkido's actual users. | `wasm` preset + `-msimd128`, benched headless in node |

Remote and WASM legs run **after** acceptance, on the winners, not inside the
per-iteration loop.

---

## 5. Architecture

### 5.1 Runtime dispatch

```cpp
// cedar/include/cedar/dsp/simd.hpp  (new)
namespace cedar::simd {
enum class Isa { Scalar, Sse2, Avx2, Neon, Wasm128 };
Isa active();              // resolved once at static-init, never in the audio path
bool has(Isa) noexcept;
}
```

Each optimised opcode keeps its scalar body and adds one or more kernels:

```cpp
// cedar/include/cedar/opcodes/arithmetic.hpp
[[gnu::always_inline]] inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
    // ... resolve a, b, out ...
#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) { kernels::mul_avx2(a, b, out); return; }
#endif
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = a[i] * b[i];   // fallback stays
}
```

Constraints the loop must respect:

- The scalar body is **never deleted** — it is the oracle and the fallback.
- No allocation, no syscalls, no locks in a kernel (the zero-alloc trap enforces).
- CPU feature detection runs once at static-init; never per block, never per sample.
- Buffers are already 32-byte aligned; kernels may assume it and must assert it
  in debug builds.

### 5.2 Repository layout (new)

```
scripts/autoresearch/
  run.py              # driver: (model, opcode) → N iterations → JSONL
  backends/
    claude_code.py    # claude -p, parses usage JSON
    openrouter.py     # GLM / DeepSeek, reads usage field
    local_openai.py   # LocalAI / Ollama, OpenAI-compatible
  verify.sh           # build + all gates → verdict JSON on stdout
  bench.sh            # wraps cedar_bench, medians, noise band
  report.py           # JSONL → markdown table + chart PNG
  prompts/
    optimize_opcode.md
  runs/               # gitignored; one dir per run
cedar/benchmarks/
  bench_opcodes.cpp   # cedar_bench target
```

### 5.3 Benchmark harness

`cedar_bench` is a plain C++ target — no new dependency, per the repo's
zero-dependency habit:

```
./build/release/bin/cedar_bench --opcode op_mul --reps 2000 --warmup 200 --json
{"opcode":"op_mul","ns_per_block_median":41.2,"ns_p10":40.7,"ns_p90":42.9,"reps":2000,
 "isa":"avx2","cpu":"...","governor":"performance"}
```

- Drives one opcode over a fixed program through the VM, steady state.
- Reports median and p10/p90; the **noise band** is `(p90 − p10) / median` from a
  baseline-vs-baseline run. A speedup inside that band is not a speedup.
- Records CPU model and scaling governor; refuses to gate (warns and marks the
  run `unstable`) if the governor is not `performance`.

### 5.4 Log schema

Per iteration, appended to `runs/<run-id>/iterations.jsonl`:

```json
{"run_id":"2026-09-28T14:03Z_opus5_op_distort_tanh","iteration":3,
 "model":"claude-opus-5","provider":"claude-code",
 "opcode":"op_distort_tanh","isa_target":"avx2",
 "tokens":{"input":18422,"output":2811,"cached":12000},
 "cost_usd":0.41,"wall_clock_s":96.3,"gpu_s":null,
 "verdict":"accept","reject_reason":null,
 "gates":{"build":"pass","equality":"approx","null_test_dbfs":-112.4,
          "unit":"pass","zero_alloc":"pass","experiment":"pass"},
 "bench":{"baseline_ns":118.7,"candidate_ns":41.9,"speedup":2.83,"noise_band":0.031},
 "patch_sha":"...","branch":"autoresearch/opus5-distort-tanh"}
```

Per run, `runs/<run-id>/summary.json`: totals (tokens, USD, wall clock, GPU
seconds), best speedup, iteration index of first accept, accept/reject counts.

### 5.5 Anti-cheat

The loop measures the thing it optimises, so the model must not be able to touch
the measurement. `verify.sh` enforces a **write allowlist**:

- Writable: `cedar/include/cedar/opcodes/**`, `cedar/include/cedar/dsp/simd*.hpp`,
  `cedar/src/dsp/simd*.cpp`.
- Everything else — benchmarks, tests, experiments, budgets, CMake — is restored
  from a pristine checkout before every verify. A patch touching them is an
  automatic reject, logged as `reject_reason: "out-of-allowlist edit"`.

This is also a talk beat: the first thing you defend against is the optimiser
optimising the scoreboard.

---

## 6. Edge Cases

1. **Benchmark noise / thermal drift.** Long runs heat the machine and slow late
   iterations. Mitigation: interleave baseline re-measurement every iteration
   (speedup is always ratio-to-a-fresh-baseline, never to a number from an hour
   ago); mark runs `unstable` when the fresh baseline drifts > 10 % from the
   pinned one.
2. **Model edits the benchmark or the tests.** Rejected by the allowlist (§5.5).
3. **Patch does not compile.** Counts as an iteration; compiler output is fed
   back as the rejection reason.
4. **Bit-identity impossible** (`tanh`, `tan`, `pow`, FP reassociation). Falls to
   the tolerance path; requires the declared `// simd: approx` comment.
5. **Approximation passes the null test but changes musical behaviour** — e.g. a
   `tanh` approximation that alters a filter's self-oscillation. Caught by the
   winners-only listening pass; if it fails there, the change is rejected even
   though every automated gate was green.
6. **SIMD is slower.** Expected for C4 (feedback-serial). Recorded as a result;
   the loop stops nominating that shape after two consecutive losses.
7. **Local model too weak to produce a valid patch in 8 iterations.** Recorded as
   zero accepted changes with its token cost — that is a finding for the talk,
   not a reason to give it extra iterations.
8. **Local model context window too small** for the opcode file. Recorded as a
   capability gap; the prompt may extract just the function body, and the fact
   that this was needed is reported.
9. **Accepted change breaks a different opcode.** Full `cedar_tests` runs every
   iteration, so this fails the gate; if it escapes, the winners-only memory
   harness pass is the backstop.
10. **Two runs collide on the same files.** One git worktree per run; runs are
    serialised on the benchmark machine anyway (concurrent benching is invalid).
11. **Runaway spend.** Fixed iteration count bounds it, but the driver also hard-
    stops a run at **$20**, logging the run as `budget-stopped`.
12. **Denormals.** A kernel that flushes denormals where the scalar path does not
    changes output. FTZ/DAZ state must be identical in both builds; the A/B test
    is run with the engine's normal settings.

---

## 7. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar/include/cedar/opcodes/*` (candidates) | **Modified** | Kernels added; scalar bodies kept intact |
| All other opcodes | **Stays** | Untouched this round |
| `cedar/include/cedar/vm/buffer_pool.hpp` | **Stays** | Already 32-byte aligned |
| VM dispatch (`cedar/src/vm/vm.cpp`) | **Stays** | No instrumentation added to the hot switch |
| `CMakePresets.json`, `cmake/CompilerOptions.cmake` | **Modified** | New `simd` build option + WASM `-msimd128`; default baselines unchanged |
| `cedar/benchmarks/` | **New** | `cedar_bench` target |
| `cedar/include/cedar/dsp/simd.hpp` | **New** | ISA detection + kernel declarations |
| `scripts/autoresearch/` | **New** | Driver, backends, gates, report generator |
| `scripts/memory/*`, `budgets.sh` | **Stays** | Used as-is on winners; budgets not touched |
| `experiments/test_op_*.py` | **Stays** | Used as gates; not modified by the loop |
| master branch | **Stays** | Loop commits only to `autoresearch/*` branches |

---

## 8. File-Level Changes

| File | Change |
|---|---|
| `cedar/benchmarks/bench_opcodes.cpp` | New — `cedar_bench` CLI, JSON output |
| `cedar/benchmarks/CMakeLists.txt` | New — target wiring |
| `cedar/include/cedar/dsp/simd.hpp` | New — `Isa`, `active()`, `has()`, alignment asserts |
| `cedar/src/dsp/simd.cpp` | New — one-time CPU detection |
| `cedar/include/cedar/opcodes/arithmetic.hpp` | Modified — AVX2/SSE2/NEON kernels for mul/add/sub |
| `cedar/include/cedar/opcodes/distortion.hpp` | Modified — kernels for `distort_tanh`, `distort_soft` |
| `cedar/include/cedar/opcodes/filters.hpp` | Modified — formant BPF bank lanes; SVF L/R fusion |
| `cmake/CompilerOptions.cmake` | Modified — `CEDAR_SIMD` option, per-ISA flags for kernel TUs |
| `CMakePresets.json` | Modified — `simd` preset; `-msimd128` on `wasm` |
| `scripts/autoresearch/run.py` | New — driver |
| `scripts/autoresearch/backends/{claude_code,openrouter,local_openai}.py` | New — one per provider, shared interface |
| `scripts/autoresearch/verify.sh` | New — allowlist restore, build, gates, verdict JSON |
| `scripts/autoresearch/bench.sh` | New — medians, fresh-baseline ratio, noise band |
| `scripts/autoresearch/report.py` | New — JSONL → markdown table + chart |
| `scripts/autoresearch/prompts/optimize_opcode.md` | New — the single shared prompt |
| `.gitignore` | Modified — `scripts/autoresearch/runs/` |
| `docs/adcx-gather-2026-talk-outline.md` | Modified — §5 replaced with real results |

---

## 9. Implementation Phases

Dates are driven by the **2 Oct** final-video deadline. Phases 0–4 must be real
before recording.

### Phase 0 — Benchmark (25 Sep)
Build `cedar_bench`; establish the noise band; record pinned baselines for C1–C4
plus a profile ranking to pick C5.
**Verify:** baseline-vs-baseline run shows a noise band < 5 %; numbers reproduce
across three invocations.

### Phase 1 — SIMD scaffolding + gates (26 Sep)
`simd.hpp` + detection, `CEDAR_SIMD` build option, A/B null-test script,
`verify.sh` with the allowlist. One hand-written AVX2 `op_mul` kernel as the
known-good patch.
**Verify:** `verify.sh` accepts the hand-written kernel, rejects a deliberately
wrong one (off-by-one lane) and a deliberately slower one, and rejects a patch
that edits a test file.

### Phase 2 — Loop, Claude backend (27–28 Sep)
`run.py` + `claude_code.py` + prompt; run all four candidates with Opus 5 and
Sonnet 5; JSONL + `report.py`.
**Verify:** a full 8-iteration run completes unattended and produces a summary
with at least one accepted change; the branch builds and passes gates from
scratch.

### Phase 3 — Other backends + platforms (29–30 Sep)
OpenRouter (GLM, DeepSeek) and local backends; Apple Silicon ssh leg and WASM
SIMD128 leg for accepted kernels.
**Verify:** identical prompts and budgets across backends; remote and WASM
numbers recorded for every winner; capability gaps logged rather than patched
around.

### Phase 4 — Full matrix + talk assets (1 Oct)
Full matrix run; winners-only memory harness pass and listening check; generate
table + chart; screen-capture a live run; pick the rejected-change example.
**Verify:** `scripts/memory/run_all.sh` green on the merged winner set; the chart
regenerates from JSONL with one command.

### Phase 5 — After the talk (post 16 Oct)
Merge winners to master via review; widen the opcode set; revisit Windows and the
x86 baseline question.

---

## 10. Testing / Verification Strategy

| What | How | Expected |
|---|---|---|
| Benchmark stability | `cedar_bench --opcode op_mul` ×3, governor `performance` | Medians within the noise band; band < 5 % |
| Gate correctness (positive) | `verify.sh` on the hand-written AVX2 `op_mul` | `accept`, speedup > 1.5× |
| Gate correctness (wrong output) | Patch with a deliberate lane swap | `reject`, reason `equality` |
| Gate correctness (no win) | Patch that is correct but slower | `reject`, reason `speed` |
| Gate correctness (cheating) | Patch that edits `bench_opcodes.cpp` | `reject`, reason `out-of-allowlist edit` |
| Approximation policy | `tanh` kernel without `// simd: approx` | `reject` even if inside tolerance |
| A/B oracle | Baseline build vs baseline build | Bit-identical; null test at `-inf` |
| Winners | `scripts/memory/run_all.sh` + render a patch to WAV and listen | Green; no audible change |
| Cost accounting | Replay a finished run's JSONL through `report.py` | Totals match the provider's own usage figures |

---

## 11. Risks

- **Schedule.** The full matrix before 2 Oct is ambitious: Phase 0 builds the
  repo's first benchmark from nothing, and Phases 3–4 add remote hosts. Fallback
  if time runs short: present Linux + Claude + one cheap model as the headline
  and label the rest as in-flight. Cutting gates to save time is not a fallback.
- **The interesting result may be negative.** C4 in particular may show no win.
  That is reportable and arguably the better talk.
- **Local models may produce nothing usable.** Also a result; the cost-per-
  speedup chart is still the point.
- **Benchmarking a laptop** is inherently noisy. The fresh-baseline ratio and the
  `unstable` flag are the mitigations; a run that cannot stabilise is discarded,
  not quietly reported.

---

## 12. Open Questions

- **[OQ1]** *Resolved (2026-09-24):* GLM + DeepSeek + Qwen coder tiers via
  OpenRouter; exact ids pinned in Phase 3.
- **[OQ2]** *Resolved (2026-09-24):* LocalAI at `localhost:8069`, one quantized
  coder model. Which one is picked in Phase 3 from what is installed.
  **[Confirm the VRAM budget — the local box is an RTX 3060 12 GB; a 16 GB
  model will not fit.]**
- **[OQ3]** Apple Silicon host is available; toolchain (cmake/clang) verified in
  Phase 3 and any gaps reported rather than installed unprompted.
- **[OQ4]** *Resolved (2026-09-24):* $20 per run.
- **[OQ5]** C5: which additional "heavy" opcodes the Phase 0 profile nominates
  — candidates from the survey are the freeverb comb bank (`reverbs.hpp`),
  `oversampling.hpp`, and `dynamics.hpp`.
- **[OQ6]** Whether the WASM leg benches in node only, or also in a real
  AudioWorklet (more representative, much harder to time).

---

## 13. Implementation Notes (2026-09-24)

Commits on master: `86b3ace` (Phase 0), `62d11b0` (test fix found on the way),
`49191bb` + `d11bc16` (Phase 1), `d29a648` (Phase 2), `75bb487` (Phase 3).
No model-generated kernel is on master; those live on `autoresearch/*`.

### Phase 0 — pinned baselines (i7-12700KF, P-core 2, governor `performance`)

`scripts/autoresearch/baselines.json`, 3 interleaved rounds × 500 reps. The
noise band is < 1 % for every candidate (Phase 0 verify: < 5 %).

| Opcode | ns / opcode-block | Band |
|---|---|---|
| `op_distort_tape` (C5 survey) | 8488 | 3.8 % |
| `op_reverb_freeverb` (C5 survey) | 6785 | 0.4 % |
| `op_filter_formant` (C3) | 5316 | 0.4 % |
| `op_dynamics_comp` (C5 survey) | 3647 | 0.3 % |
| `op_distort_tube` (C5 survey) | 3057 | 0.6 % |
| `op_filter_svf_hp` / `_lp` / `_bp` (C4) | 2355 / 2256 / 2240 | ≤ 0.7 % |
| `op_reverb_fdn` (C5 survey) | 1080 | 0.4 % |
| `op_distort_tanh` (C2) | 974 | 0.4 % |
| `op_distort_soft` (C2) | 257 | 0.1 % |
| `op_add` / `op_sub` / `op_mul` (C1) | 14.5 | ≤ 0.6 % |

**[OQ5] → C5 = `op_reverb_freeverb`** (heaviest opcode with a stable band;
`tape` is heavier but its cost varies 4–8 µs with input, so a gate on it is
noisy). Target name `freeverb` in `verify.TARGETS`.

### Phase 1 — gate selftest (`verify.py --selftest`)

| Fixture | Verdict |
|---|---|
| hand-written AVX2 `op_mul` | accept — **1.32×** (PRD §10 expected > 1.5×: the scalar loop is already SSE-autovectorised and the op is L1-bound) |
| lane swap | reject `equality` |
| correct but slower | reject `speed` |
| edits `bench_opcodes.cpp` | reject `out-of-allowlist edit` |
| `tanh` via `exp`, in tolerance, undeclared | reject `undeclared approximation` |

### Phase 2 — Claude Code spike (2 of 8 iterations, `arith`)

`run.py --backend claude-code --model sonnet --target arith --iterations 2`
→ branch `autoresearch/sonnet-arith-202609241443` (not merged).

| Iter | Verdict | Speedup (add / sub / mul) | Tokens in / out (cached) | USD | Wall clock |
|---|---|---|---|---|---|
| 1 | accept, every gate green, bit-exact | 1.32× / 1.31× / 1.32× | 54 499 / 1 857 (30 077) | $0.12 | 33 s |
| 2 | reject `speed`: no gain beyond noise over iter 1 | 1.00× / 1.01× / 1.00× | 32 513 / 1 187 (24 773) | $0.05 | 15 s |

The driver ran unattended through baseline, propose, verify, commit and
revert; `report.py` regenerated the table and chart from the JSONL. The full
8-iteration runs across the matrix (Phase 2 verify and Phase 4) have **not**
been run.

### Phase 3 — platform legs on the spike's accepted kernel

`legs.py --wasm --remote local` (the remote script run on this machine; no Mac
host yet):

| Leg | add / sub / mul | Output |
|---|---|---|
| WASM SIMD128 (node 24, `-msimd128` both sides) | 1.00× / 1.00× / 1.00× | bit-exact |
| remote script, local x86 (unpinned) | 1.56× / 1.29× / 1.69× | bit-exact |

Finding for the talk: an AVX2-only kernel does nothing for nkido's web
users. At `-msimd128` the compiler already auto-vectorises the scalar loop
to the same speed. The prompt allows WASM/NEON paths, but the gate only
rewards x86.

### Divergences from the design above

1. **Python, not shell.** `verify.sh` / `bench.sh` are `verify.py` /
   `bench.py`: the equality gate needs numpy, and one language keeps the
   driver importing the gates directly.
2. **A/B oracle is `cedar_bench --dump`, not `cedar_core`.** The dump uses the
   exact stimuli the timing path uses (one source of truth), and a baseline and
   a candidate binary can run side by side without two `cedar_core` modules
   colliding on one import name. `cedar_core` is still built per worktree and
   drives the experiment gate.
3. **Extra gate: scalar fallback.** Every opcode is also dumped with
   `--scalar` (forces `simd::Isa::Scalar`) and must be bit-identical to the
   original. This enforces "the scalar body is never deleted" mechanically.
4. **Noise band = run-to-run spread of baseline medians** over 5 interleaved
   rounds. The within-run p10/p90 spread is still reported, but it measures
   interrupt tails (30 % on `op_mul`) rather than median noise (0.5 %). The bench
   also subtracts an interleaved empty-program VM (~450 ns of fixed
   `process_block` overhead that hid most of `op_mul`'s cost) and times each
   rep over 32 blocks spread across all four stimulus segments (`tanh`'s cost
   depends on the input).
5. **No per-ISA kernel TUs.** Kernels carry `[[gnu::target("avx2")]]`
   (`CEDAR_TARGET_AVX2`) in the opcode headers, so no CMake flags change and the
   allowlist stays header-only. `CEDAR_SIMD` defaults **OFF**; the `simd`
   preset (Release + tests) is the gate build, and `wasm-simd` adds
   `-msimd128`.
6. **Experiment gate = no new `✗` lines vs baseline, and exit code 0.** Most
   `test_op_*.py` scripts have no assertions and always exit 0, so the
   stronger form in §4.3 is not available without editing them.
7. **Speed baseline moves, equality baseline does not.** After an accept, later
   proposals must beat the accepted head; equality is always against the
   original scalar dumps, so approximations cannot compound. Speedup vs origin
   is logged too.
8. **Edge case 6 not implemented.** C5 is fixed from the Phase 0 ranking, not
   nominated by the loop, so "stop nominating after two losses" has nothing to
   act on.
9. **Model matrix ids.** OpenRouter no longer lists "coder" tiers. The pinned
   defaults are `z-ai/glm-5.3`, `deepseek/deepseek-v4-pro-0813` and
   `qwen/qwen3.8-27b` (dated ids, tool-capable) **[confirm]**. The local
   backend defaults to Ollama (`qwen3:14b` Q4_K_M, 9.3 GB) on the actual GPU,
   an **RTX 5060 Ti 16 GB** (not the 3060 12 GB assumed in OQ2). LocalAI
   stays reachable via `AUTORESEARCH_LOCAL_URL`. Ollama's server default
   context of 4096 tokens truncates the prompt; the backend detects this and
   logs it as a capability gap. A fair local run needs the server started
   with `OLLAMA_CONTEXT_LENGTH=32768`.
10. **Claude isolation.** `claude -p --safe-mode` drops CLAUDE.md, hooks,
    skills, plugins and MCP servers, so Claude gets the same prompt as the
    other backends. `--bare` would need an API key.
11. **Found on the way:** four `InstructionBuilder` tests bound a reference
    to a temporary builder's field. The first Release test build (`simd`
    preset) failed on them; fixed in `62d11b0`.

### Prompt framework revision (2026-09-24)

Informed by `docs/research/llm-autoresearch-loops.md`, which covers
Karpathy's autoresearch, AutoKernel, OpenEvolve / AlphaEvolve /
ShinkaEvolve, CUDA-L1, Kevin-32B and METR. The owner adopted P1–P8 and P10.
Summary:

- **P1/P3:** ideas ledger. The agent ends with `IDEA: …`, and the history
  becomes a compact attempts table. Only the latest rejection keeps its full
  verifier output.
- **P2:** the stale-baseline bug is fixed. The prompt shows current vs
  original ns and the accepted diff.
- **P4:** equal per-attempt budget for every backend: 30 min wall clock and
  10 check calls. `claude -p` has no turn cap, so turns are logged, not
  limited.
- **P5:** hidden stimuli. `cedar_bench --seed N` generates randomised
  stimuli. Every seed includes a near-denormal segment and a hot segment.
  The full verify uses a fresh seed on every call, and the prompt tells the
  model this. A new selftest fixture, `hidden_ftz_mul`, is exact on the
  fixed stimuli and must be rejected.
- **P6:** state the bottleneck hypothesis before editing, then make one
  focused change.
- **P7:** after 3 rejects in a row, the driver adds a fixed "structurally
  different" line.
- **P8:** a patch ≥ 95 % identical to an earlier rejected patch is rejected
  as `duplicate` without running the verifier.
- **P10:** the quick check prints GCC `-fopt-info-vec` remarks for the
  opcode's header.
- **Not adopted:** P11 (frozen lessons list). P9 (keep fresh sessions) is
  the status quo.

### Second spike on the revised framework (3 iterations, `distort`)

Command: `run.py --backend claude-code --model sonnet --target distort
--iterations 3`. Branch: `autoresearch/sonnet-distort-202609242050` (not
merged). Total $0.45, 2.4 min of proposing.

| Iter | Verdict | Result |
|---|---|---|
| 1 | accept | `tanh` **18.8×** (973 → 52 ns): a rational approximation declared with `// simd: approx`, −128.9 dBFS on both the fixed and the hidden stimuli. Soft clip **5.2×**, bit-exact |
| 2–3 | reject `speed` | No change made: Claude reported the check as "denied" |

The rejects were harness bugs, fixed in `df57f8b`. The allow rule matched
only the exact check command, so piped calls were refused. Unchanged trees
after an accept were also verified in full instead of being rejected as
`empty patch`. The `tanh` kernel still needs the winners-only listening
check (§4.3) before anyone merges it.

### Blockers for Phase 3/4 (the user's to resolve)

- `OPENROUTER_API_KEY` is not set, so the OpenRouter backend is written but
  has not been exercised live.
- The Apple Silicon host for `legs.py --remote` is still OQ3. The remote
  script has only been exercised with `--remote local`.
- The Ollama server must be restarted with `OLLAMA_CONTEXT_LENGTH=32768`.
  At the default 4096 the prompt is truncated.
- The OpenRouter model ids (§13, divergence 9) need confirming.

---

## 14. Resume Here (next session)

**Sweep paused on 2026-09-25 (22:40 CEST), resume from here:**

The full matrix is 6 models × 5 targets = 30 runs of 8 iterations.
`sweep.sh` runs it as one lane per model: each run has its own worktree
and branch, and `verify.machine_lock` serialises every build and bench.
The six in-flight runs were stopped at an **iteration boundary**: each
was killed right after its current iteration was logged, which happens
after the commit or revert. Iterations done: opus svf 3, and on arith
sonnet 7, deepseek-v4-pro 3, glm-5.3 2, qwen3.8-27b 2 and local
qwen3:14b-32k 2 (`runs/stop_at_boundary.log`).
`run.py --resume RUN_DIR` (`0a19cc1`) continues a stopped run in the
same worktree and branch. The state is rebuilt from `iterations.jsonl`,
and the rebuilt next-iteration prompt was checked byte-identical against
the one the live process had written. `sweep.sh` resumes open runs
before it starts new ones. From now on, `touch runs/STOP` pauses every
lane at its next iteration boundary; `rm runs/STOP` before resuming.

Done: opus arith (1.41×), opus distort (tanh 30.7×, soft 7.9×) and opus
formant (8.1×). DeepSeek's first arith run was lost to a one-token
upstream reply. It is archived in `runs/_aborted/`, and `f2f822f` now
retries such replies. Runs started before `2822230` have no
`lock_wait_propose_s`, so the report shows their raw wall clock.

To resume, first check that nothing is still running:

```bash
pgrep -af "run.py --backend" || scripts/autoresearch/runs/launch_lanes.sh
```

`launch_lanes.sh` is gitignored; it is a copy of the loop below:

```bash
cd scripts/autoresearch
export OPENROUTER_API_KEY=…   # from ../video_app/.env
for spec in claude-code:opus claude-code:sonnet openrouter:z-ai/glm-5.3 \
    openrouter:deepseek/deepseek-v4-pro-0813 openrouter:qwen/qwen3.8-27b local:qwen3:14b-32k; do
  setsid nohup ./sweep.sh "$spec" >> "runs/lane_$(echo "$spec" | tr ':/' '__').log" 2>&1 < /dev/null &
  sleep 60
done
```

Finished `(model, target)` pairs are skipped. OpenRouter is capped at
$30 in total and $2 per run. At about 3 min of lock time per iteration,
the hosted lanes need ≈ 12 h. The local lane needs ≈ 20 h, because it
runs close to the 30 min cap on every iteration. After the sweep:

1. `legs.py --wasm --remote mini` for every run with an accept. `mini`
   is an M4 Mac mini reachable over ssh; the remote leg was fixed and
   verified in `8778ac4`.
2. The `tanh` listening check (step 2 below).
3. `report.py`.
4. Commit the logs to the `autoresearch/results` branch (owner decision
   2026-09-25).

**State on 2026-09-24:**

- Phases 0–3 of the harness are on master (`86b3ace`..`9ea074e`, unpushed).
- The gate selftest is green: 6 fixtures.
- Two test-run branches exist. Neither is merged, and neither is on master:
  - `autoresearch/sonnet-arith-202609241443`: add/sub/mul at 1.31×.
  - `autoresearch/sonnet-distort-202609242050`: `tanh` 18.8× (declared
    approximation), soft clip 5.2×.
- Run logs live under the gitignored `scripts/autoresearch/runs/`: one
  `iterations.jsonl` per run, with prompts, transcripts and patches.
- The Claude transcripts from the second test run predate the switch to
  stream-json, so they hold only the final message.

**Sanity check before anything else** (≈ 6 min):

```bash
cmake --build build/simd -j                       # gate build (preset `simd`)
scripts/autoresearch/test_harness.py              # driver helpers
scripts/autoresearch/verify.py --selftest         # 6 fixtures -> "selftest PASSED"
```

**Next steps, in order:**

1. **Re-run the `distort` test run** (3 iterations). This confirms the fixes
   in `df57f8b`: piped check calls are allowed and a no-op is rejected as
   `empty patch`. Look at the `stream-json` transcript and the `check_calls`
   and `idea` fields in `iterations.jsonl`.
2. **Listening check on the `tanh` winner** (§4.3, winners only):
   `scripts/memory/run_all.sh` on the branch, then render a patch that uses
   `saturate`/`tanh` to WAV, compare it with master, and listen.
3. **Owner inputs** (see "Blockers" above): OpenRouter key, the three
   OpenRouter model ids, the Mac host, and Ollama's context length.
4. **Phase 2 verify:** one full 8-iteration run per candidate with Opus 5
   and Sonnet 5:
   `run.py --backend claude-code --model opus --target <arith|distort|formant|svf|freeverb>`.
   Runs share the benchmark machine, so they must be **serialised**. Budget
   $20 per run.
5. **Phase 3:** OpenRouter runs (`--backend openrouter --model …`) and local
   runs (`--backend local --model qwen3:14b`) on the same targets. Then
   `legs.py --run runs/<id> --wasm --remote <mac>` for every run with an
   accept.
6. **Phase 4:** `report.py --out results.md` builds the table and the chart
   (`runs/results.png`). Then:
   - pick the rejected-change example for the talk (the gates' rejections
     are in `runs/*/rejected/`);
   - screen-capture a live run;
   - replace §5 of `docs/adcx-gather-2026-talk-outline.md` with the real
     results.
7. **Phase 5 (after 16 Oct):** review and merge winners to master. Default
   builds keep `CEDAR_SIMD=OFF` until the owner decides otherwise.

**Findings so far to carry into the talk:**

- The gates caught what they should. The selftest proves one case per
  gate. In the test runs they rejected no-gain changes, and the hidden
  stimuli exposed a kernel that was exact only on the fixed inputs.
- The AVX2 `op_mul` reached 1.32×, not the 1.5× predicted. The scalar loop
  was already SSE-autovectorised.
- On WASM an AVX2-only kernel gives 1.00×, because `-msimd128`
  autovectorisation matches it. Only a WASM128 kernel would help web users.
- `tanh` gained 18.8× from a declared approximation. This is the case for
  the "declared approximation + null test + listening" policy.
- Cost so far: $0.17 (arith) + $0.45 (distort) for 5 proposals in total.

