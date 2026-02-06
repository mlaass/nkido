# Refactor Experiments: One File Per Opcode

## Context

The `experiments/` directory has 25 test files, many testing multiple unrelated opcodes in a single file (e.g., `test_effects.py` covers 10+ opcodes). Output goes to a flat `output/` folder with 150+ files. This makes it hard to evaluate and iterate on individual DSP algorithms. We're refactoring to one file per opcode codename with isolated output directories.

## Shared Utility Changes

### 1. Add `output_dir()` helper to `cedar_testing.py`

```python
def output_dir(name: str) -> str:
    """Get/create output directory for a test: output/<name>/"""
    path = os.path.join(os.path.dirname(__file__), "output", name)
    os.makedirs(path, exist_ok=True)
    return path
```

Every test file calls `OUT = output_dir("op_<codename>")` at the top and uses `OUT` for all file writes.

### 2. Consolidate duplicated helpers into `utils.py`

- `NumpyEncoder` (duplicated in 4 files) -> `utils.py`
- `save_wav()` (duplicated in 5 files) -> `utils.py`
- Signal generators (`gen_white_noise`, `gen_impulse`, `gen_linear_ramp`) -> `utils.py`

### 3. Delete dead files

- `test_harness.py` (markdown design doc, not code)
- `main.py` (placeholder, does nothing)

## File Mapping

### Files to split

**`test_effects.py`** -> 10 files:
| New File | Output Dir | Functions to extract |
|---|---|---|
| `test_op_saturate.py` | `output/op_saturate/` | `test_distortion_curves()` (tanh portion) |
| `test_op_softclip.py` | `output/op_softclip/` | `test_distortion_curves()` (softclip portion) |
| `test_op_tube.py` | `output/op_tube/` | `test_distortion_curves()` (tube portion) |
| `test_op_fold.py` | `output/op_fold/` | `test_distort_fold_*()` (4 functions) |
| `test_op_bitcrush.py` | `output/op_bitcrush/` | `test_bitcrush_levels()` |
| `test_op_phaser.py` | `output/op_phaser/` | `test_phaser_spectrogram()` |
| `test_op_dattorro.py` | `output/op_dattorro/` | `test_reverb_decay()` |
| `test_op_delay.py` | `output/op_delay/` | `test_delay_timing()` |
| `test_op_chorus.py` | `output/op_chorus/` | `test_chorus_spectrum()` |
| `test_op_flanger.py` | `output/op_flanger/` | `test_flanger_sweep()` |

**`test_filters.py`** -> 7 files:
| New File | Output Dir | Functions |
|---|---|---|
| `test_op_lp.py` | `output/op_lp/` | SVF LP tests from `test_svf_comparison()` |
| `test_op_hp.py` | `output/op_hp/` | SVF HP tests |
| `test_op_bp.py` | `output/op_bp/` | SVF BP tests |
| `test_op_moog.py` | `output/op_moog/` | `test_moog_resonance()` |
| `test_op_diode.py` | `output/op_diode/` | `test_diode_*()` (3 functions) |
| `test_op_formant.py` | `output/op_formant/` | `test_formant_*()` (2 functions) |
| `test_op_sallenkey.py` | `output/op_sallenkey/` | `test_sallenkey_*()` (2 functions) |

Shared filter helpers (`analyze_filter()`, `get_bode_data()`) -> new `filter_helpers.py` module imported by all filter test files.

**`test_envelopes.py`** -> 3 files:
| New File | Output Dir |
|---|---|
| `test_op_adsr.py` | `output/op_adsr/` |
| `test_op_ar.py` | `output/op_ar/` |
| `test_op_env_follower.py` | `output/op_env_follower/` |

`EnvelopeTestHost` stays in `test_op_adsr.py` and is imported by `test_op_ar.py`.

**`test_dynamics.py`** -> 3 files:
| New File | Output Dir |
|---|---|
| `test_op_comp.py` | `output/op_comp/` |
| `test_op_limiter.py` | `output/op_limiter/` |
| `test_op_gate.py` | `output/op_gate/` |

**`test_sequencers.py`** -> 4 files:
| New File | Output Dir |
|---|---|
| `test_op_clock.py` | `output/op_clock/` |
| `test_op_lfo.py` | `output/op_lfo/` |
| `test_op_trigger.py` | `output/op_trigger/` |
| `test_op_euclid.py` | `output/op_euclid/` |

Cross-opcode alignment test goes in `test_op_euclid.py` (it tests euclid against clock+trigger).

**`test_utility.py`** -> 4 files:
| New File | Output Dir |
|---|---|
| `test_op_noise.py` | `output/op_noise/` |
| `test_op_mtof.py` | `output/op_mtof/` |
| `test_op_slew.py` | `output/op_slew/` |
| `test_op_sah.py` | `output/op_sah/` |

**`test_stereo.py`** -> 5 files:
| New File | Output Dir |
|---|---|
| `test_op_pan.py` | `output/op_pan/` |
| `test_op_width.py` | `output/op_width/` |
| `test_op_ms_encode.py` | `output/op_ms_encode/` |
| `test_op_ms_decode.py` | `output/op_ms_decode/` |
| `test_op_pingpong.py` | `output/op_pingpong/` |

### Files to rename (single-opcode, just rename + update output paths)

| Old File | New File | Output Dir |
|---|---|---|
| `test_oscillators.py` | `test_op_osc.py` | `output/op_osc/` |
| `test_osc_basic.py` | `test_op_osc_oversampling.py` | `output/op_osc_oversampling/` |
| `test_fm_aliasing.py` | `test_op_osc_fm.py` | `output/op_osc_fm/` |
| `test_pwm.py` | `test_op_sqr_pwm.py` | `output/op_sqr_pwm/` |
| `test_pwm_phase.py` | `test_op_sqr_pwm_phase.py` | `output/op_sqr_pwm_phase/` |
| `test_pwm_fm.py` | `test_op_sqr_pwm_fm.py` | `output/op_sqr_pwm_fm/` |
| `test_minblep_sqr.py` | `test_op_sqr_minblep.py` | `output/op_sqr_minblep/` |
| `test_minblep_table.py` | `test_op_sqr_minblep_table.py` | `output/op_sqr_minblep_table/` |
| `test_sqr_debug.py` | `test_op_sqr_debug.py` | `output/op_sqr_debug/` |
| `test_sqr_corrections.py` | `test_op_sqr_polyblep.py` | `output/op_sqr_polyblep/` |
| `test_polyblep_symmetry.py` | `test_op_sqr_polyblep_symmetry.py` | `output/op_sqr_polyblep_symmetry/` |
| `test_sampler.py` | `test_op_sample.py` | `output/op_sample/` |

## Execution Order

1. **Shared utilities first**: Add `output_dir()` to `cedar_testing.py`, add `NumpyEncoder`/`save_wav`/signal generators to `utils.py`
2. **Delete dead files**: `test_harness.py`, `main.py`
3. **Create `filter_helpers.py`**: Extract shared filter analysis functions
4. **Split large files** (one at a time, largest first): `test_effects.py` -> `test_filters.py` -> `test_sequencers.py` -> `test_envelopes.py` -> `test_dynamics.py` -> `test_utility.py` -> `test_stereo.py`
5. **Rename single-opcode files** and update their output paths
6. **Delete old files** after all splits/renames are done
7. **Clean up `output/`**: Delete flat output contents (gitignored, regeneratable)

## Progress Tracking

- [x] Shared utilities (`output_dir`, `NumpyEncoder`, `save_wav`)
- [x] Delete `test_harness.py`, `main.py`
- [x] Create `filter_helpers.py`
- [x] Split `test_effects.py` (10 new files)
- [x] Split `test_filters.py` (7 new files)
- [x] Split `test_sequencers.py` (4 new files)
- [x] Split `test_envelopes.py` (3 new files)
- [x] Split `test_dynamics.py` (3 new files)
- [x] Split `test_utility.py` (4 new files)
- [x] Split `test_stereo.py` (5 new files)
- [x] Rename 12 single-opcode files
- [x] Delete old files
- [x] Clean up flat `output/` contents

## Key Files

- `experiments/cedar_testing.py` - Add `output_dir()` helper
- `experiments/utils.py` - Add `NumpyEncoder`, `save_wav`, signal generators
- `experiments/filter_helpers.py` - New: shared filter analysis functions
- `experiments/test_effects.py` - Largest split (10 opcodes -> 10 files)
- `experiments/test_filters.py` - Second largest split (7 files)

## Verification

After refactoring, run each new file independently:
```bash
cd experiments
uv run python test_op_<name>.py
```
Verify each creates its output in `output/op_<name>/` and doesn't write anywhere else.
