You are optimising Cedar DSP opcodes with SIMD. Cedar is the real-time audio
engine of nkido: a bytecode VM that processes blocks of 128 float samples at
48 kHz, zero allocations on the audio path. The repository is your current
working directory.

## Task

Make these opcodes faster on **x86-64 AVX2** (the gate machine):

- `op_filter_formant`

They live in `cedar/include/cedar/opcodes/filters.hpp`. Current source of the opcode bodies:

```cpp
[[gnu::always_inline]]
inline void op_filter_formant(ExecutionContext& ctx, const Instruction& inst) {
    float* out_l = ctx.buffers->get(inst.out_buffer);
    float* out_r = ctx.buffers->get(static_cast<std::uint16_t>(inst.out_buffer + 1));
    const float* input = ctx.buffers->get(inst.inputs[0]);
    const bool stereo_in = (inst.flags & InstructionFlag::STEREO_INPUT) != 0;
    const float* input_r = stereo_in
        ? ctx.buffers->get(static_cast<std::uint16_t>(inst.inputs[0] + 1))
        : nullptr;
    const float* vowel_a = ctx.buffers->get(inst.inputs[1]);
    const float* vowel_b = ctx.buffers->get(inst.inputs[2]);
    const float* morph = ctx.buffers->get(inst.inputs[3]);
    const float* q_in = ctx.buffers->get(inst.inputs[4]);
    auto& state = ctx.states->get_or_create<FormantState>(inst.state_id);

    // ExtendedParams<2>: ext[0] = dry (default 0.0), ext[1] = wet (default 1.0)
    const auto* ext = ctx.states->get_if<ExtendedParams<2>>(ext_params_state_id(inst.state_id));
    const float* dry_buf = nullptr; float dry_const = 0.0f;
    const float* wet_buf = nullptr; float wet_const = 1.0f;
    if (ext) {
        const auto& d = ext->params[0];
        if (d.is_constant()) dry_const = d.constant; else dry_buf = ctx.buffers->get(d.buffer_idx);
        const auto& w = ext->params[1];
        if (w.is_constant()) wet_const = w.constant; else wet_buf = ctx.buffers->get(w.buffer_idx);
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float va = std::clamp(vowel_a[i], 0.0f, 4.0f);
        float vb = std::clamp(vowel_b[i], 0.0f, 4.0f);
        float m = std::clamp(morph[i], 0.0f, 1.0f);
        float q = std::clamp(q_in[i], 1.0f, 20.0f);
        float dry = drywet::coeff(dry_buf, i, dry_const);
        float wet = drywet::coeff(wet_buf, i, wet_const);

        // Check if we need to recalculate formant targets
        if (va != state.last_vowel_a || vb != state.last_vowel_b ||
            m != state.last_morph || q != state.last_q) {

            state.last_vowel_a = va;
            state.last_vowel_b = vb;
            state.last_morph = m;
            state.last_q = q;

            // Interpolate between vowel indices (with fractional support)
            int idx_a = static_cast<int>(va);
            int idx_b = static_cast<int>(vb);
            float frac_a = va - static_cast<float>(idx_a);
            float frac_b = vb - static_cast<float>(idx_b);

            idx_a = std::min(idx_a, 4);
            idx_b = std::min(idx_b, 4);
            int idx_a2 = std::min(idx_a + 1, 4);
            int idx_b2 = std::min(idx_b + 1, 4);

            // Get formants for vowel A (with fractional interpolation)
            float f1_a = VOWEL_TABLE[idx_a].f1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f1 * frac_a;
            float f2_a = VOWEL_TABLE[idx_a].f2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f2 * frac_a;
            float f3_a = VOWEL_TABLE[idx_a].f3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].f3 * frac_a;
            float g1_a = VOWEL_TABLE[idx_a].g1 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g1 * frac_a;
            float g2_a = VOWEL_TABLE[idx_a].g2 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g2 * frac_a;
            float g3_a = VOWEL_TABLE[idx_a].g3 * (1.0f - frac_a) + VOWEL_TABLE[idx_a2].g3 * frac_a;

            // Get formants for vowel B
            float f1_b = VOWEL_TABLE[idx_b].f1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f1 * frac_b;
            float f2_b = VOWEL_TABLE[idx_b].f2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f2 * frac_b;
            float f3_b = VOWEL_TABLE[idx_b].f3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].f3 * frac_b;
            float g1_b = VOWEL_TABLE[idx_b].g1 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g1 * frac_b;
            float g2_b = VOWEL_TABLE[idx_b].g2 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g2 * frac_b;
            float g3_b = VOWEL_TABLE[idx_b].g3 * (1.0f - frac_b) + VOWEL_TABLE[idx_b2].g3 * frac_b;

            // Morph between A and B
            state.f1 = f1_a * (1.0f - m) + f1_b * m;
            state.f2 = f2_a * (1.0f - m) + f2_b * m;
            state.f3 = f3_a * (1.0f - m) + f3_b * m;
            state.g1 = g1_a * (1.0f - m) + g1_b * m;
            state.g2 = g2_a * (1.0f - m) + g2_b * m;
            state.g3 = g3_a * (1.0f - m) + g3_b * m;
        }

        // Chamberlin SVF coefficients with bilinear pre-warping (shared across channels)
        auto prewarp_coef = [&](float freq) -> float {
            float w = PI * std::min(freq, ctx.sample_rate * 0.48f) / ctx.sample_rate;
            return std::min(1.9f, 2.0f * std::tan(w));
        };
        float f1_coef = prewarp_coef(state.f1);
        float f2_coef = prewarp_coef(state.f2);
        float f3_coef = prewarp_coef(state.f3);
        float q_coef = 1.0f / q;

        for (std::size_t ch = 0; ch < 2; ++ch) {
            float x = (ch == 0) ? input[i] : (stereo_in ? input_r[i] : input[i]);

            // Bandpass 1 (F1 - first formant)
            float hp1 = x - state.bp1_z1[ch] * q_coef - state.bp1_z2[ch];
            float bp1 = state.bp1_z1[ch] + f1_coef * hp1;
            float lp1 = state.bp1_z2[ch] + f1_coef * state.bp1_z1[ch];
            state.bp1_z1[ch] = clamp_audio(bp1);
            state.bp1_z2[ch] = clamp_audio(lp1);

            // Bandpass 2 (F2 - second formant)
            float hp2 = x - state.bp2_z1[ch] * q_coef - state.bp2_z2[ch];
            float bp2 = state.bp2_z1[ch] + f2_coef * hp2;
            float lp2 = state.bp2_z2[ch] + f2_coef * state.bp2_z1[ch];
            state.bp2_z1[ch] = clamp_audio(bp2);
            state.bp2_z2[ch] = clamp_audio(lp2);

            // Bandpass 3 (F3 - third formant)
            float hp3 = x - state.bp3_z1[ch] * q_coef - state.bp3_z2[ch];
            float bp3 = state.bp3_z1[ch] + f3_coef * hp3;
            float lp3 = state.bp3_z2[ch] + f3_coef * state.bp3_z1[ch];
            state.bp3_z1[ch] = clamp_audio(bp3);
            state.bp3_z2[ch] = clamp_audio(lp3);

            // Sum bandpasses with formant gains
            float processed = bp1 * state.g1 + bp2 * state.g2 + bp3 * state.g3;
            (ch == 0 ? out_l : out_r)[i] = drywet::mix(x, processed, dry, wet);
        }
    }
}
```

Cost on the gate machine (median ns per opcode-block, lower is better):

| Opcode | Original scalar | Current (accepted so far) | Speedup so far |
|---|---|---|---|
| `op_filter_formant` | 5316.11 | 5316.11 | 1.00x |

## Accepted so far (already in the tree)

Nothing yet — the tree is the original scalar code.

## How to add a kernel

The dispatch header `cedar/include/cedar/dsp/simd.hpp` is already there. Keep
the scalar body and add a kernel in front of it:

```cpp
#include "../dsp/simd.hpp"

namespace kernels {
#if CEDAR_SIMD_X86
CEDAR_TARGET_AVX2 inline void mul_avx2(const float* a, const float* b, float* out) noexcept {
    for (std::size_t i = 0; i < BLOCK_SIZE; i += 8)
        _mm256_store_ps(out + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
}
#endif
}

inline void op_mul(ExecutionContext& ctx, const Instruction& inst) {
    // ... resolve a, b, out ...
#if CEDAR_SIMD_X86
    if (simd::has(simd::Isa::Avx2)) { kernels::mul_avx2(a, b, out); return; }
#endif
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = a[i] * b[i];  // fallback stays
}
```

- `CEDAR_TARGET_AVX2` is `[[gnu::target("avx2")]]`; no compiler flags change.
  FMA is **not** enabled (it would change rounding).
- Buffers from `ctx.buffers->get()` are 32-byte aligned; `BLOCK_SIZE` is 128.
- You may also add NEON (`CEDAR_SIMD_NEON`) or WASM SIMD128
  (`CEDAR_SIMD_WASM`) paths; they are measured later on other machines, not
  gated here.

## Rules (enforced by the verifier — violations are rejected automatically)

1. You may only edit: `cedar/include/cedar/opcodes/**`, `cedar/include/cedar/dsp/simd*.hpp`, `cedar/src/dsp/simd*.cpp`. Any other edit (benchmarks, tests,
   experiments, CMake, scripts) rejects the whole attempt.
2. **Never delete or change the scalar body.** It is the oracle and the
   fallback; the verifier runs it with SIMD disabled and requires
   bit-identical output to the original.
3. Output must be **bit-identical** to the original for every opcode.
   Exception: op_filter_formant call `tanh`/`tan`; for these only, an approximation is allowed if (a) you mark it with a `// simd: approx` comment naming the replaced function, and (b) its peak error stays at or below -100 dBFS (about 1e-5 absolute) on every stimulus.
4. The final verification also runs every opcode on **randomised stimuli you
   cannot see** (different signals, levels down to denormals, parameter
   trajectories). A kernel that only works for the stimuli in the benchmark
   source fails there.
5. No allocation, locks or syscalls in a kernel. CPU detection is already done
   once at start-up; never detect features per block or per sample.
6. The change must make at least one listed opcode **≥ 5 % faster** than the
   *current* version (outside measurement noise) and none slower.

## Budget

At most **10 runs of the check command** and **30 minutes** for
this attempt. Every model in this experiment gets the same budget.

Your tools are: read, write and edit files, search the repository, and run
the check command. **The check command is the only shell command allowed**
(piping its output, e.g. through `tail`, is fine). You cannot run Python,
compilers or other scripts; do any numeric derivation (polynomial
coefficients, error bounds) yourself, and let the check measure it.

## Method

1. Read the code you need.
2. **Before editing, state your hypothesis in one or two sentences:** what
   bounds this opcode's speed (e.g. a serial feedback recurrence, a
   transcendental call, memory bandwidth, branches, VM overhead) and the
   single change you will make to address it.
3. Make **one focused change**. Do not repeat an idea from the attempts table
   below unless you fix the reason it failed.
4. Run `/home/moritz/workspace/nkido/scripts/autoresearch/runs/2026-09-26T2116Z_qwen-qwen3-8-27b-free_formant/check.sh`. It builds, checks the allowlist and bit-identity for
   every opcode, prints compiler vectoriser remarks for `cedar/include/cedar/opcodes/filters.hpp`, and runs a
   short benchmark (~1 minute). The full verification afterwards also runs
   the hidden stimuli, all unit test suites, the zero-allocation trap, the
   opcode's DSP experiments and a longer benchmark.
5. Stop. Leave your changes in the working tree; do not commit.

**Your final message must end with exactly one line of this form:**

```
IDEA: <bottleneck> -> <the change you made> (<what the check showed>)
```

## Previous attempts in this run

| # | Verdict | Idea (the agent's own IDEA line) | Speedup vs previous |
|---|---|---|---|
| 1 | rejected: `backend error` — HTTP 429: {"error":{"message":"Rate limit exceeded: free-models-per-day-high-balance. ","code":429,"metadata":{"headers" | (no IDEA line) | — |
| 2 | rejected: `backend error` — HTTP 429: {"error":{"message":"Rate limit exceeded: free-models-per-day-high-balance. ","code":429,"metadata":{"headers" | (no IDEA line) | — |
| 3 | rejected: `backend error` — HTTP 429: {"error":{"message":"Rate limit exceeded: free-models-per-day-high-balance. ","code":429,"metadata":{"headers" | (no IDEA line) | — |
| 4 | rejected: `backend error` — HTTP 429: {"error":{"message":"Rate limit exceeded: free-models-per-day-high-balance. ","code":429,"metadata":{"headers" | (no IDEA line) | — |
| 5 | rejected: `backend error` — HTTP 429: {"error":{"message":"Rate limit exceeded: free-models-per-day-high-balance. ","code":429,"metadata":{"headers" | (no IDEA line) | — |

Full verifier output for the latest rejection (attempt 5, gate `backend error`):
```
HTTP 429: {"error":{"message":"Rate limit exceeded: free-models-per-day-high-balance. ","code":429,"metadata":{"headers":{"X-RateLimit-Limit":"1000","X-RateLimit-Remaining":"0","X-RateLimit-Reset":"1790467200000"},"limit_source":"openrouter_free_tier_daily","remedy_hint":"Wait for the daily reset (see X-RateLimit-Reset), or purchase credits to raise your free-model daily limit.","provider_name":null}},"user_id":"user_2w2n1SmW2gqA6FhfcrnMhp4Ha6i"}
```

**The last 5 attempts were all rejected.** Try a structurally different approach from everything in the table above (different data layout, different loop structure, a different opcode in the list) rather than a variation of them.

