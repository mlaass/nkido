# Sequencing Functions Audit & Deprecation

## Summary

The Akkado pattern/sequencing API has accumulated redundant and non-functional functions. Only `pat()` is a true parser keyword that produces working patterns. Several other functions exist in builtins but are non-functional or legacy. Additionally, 11 Cedar opcodes are completely unreachable from the compiler. This document records the audit findings and deprecation decisions.

## Function Status

| Function | Status | Reason |
|----------|--------|--------|
| `pat()` | **Keep** | Only real parser keyword. Compiles to SEQPAT_QUERY/STEP/GATE/TYPE. Fully functional. |
| `seq()` | **Remove** | Not a keyword. In builtins as PUSH_CONST placeholder. Only recognized by `is_pattern_expr()` for pattern transform validation. Non-functional standalone. |
| `note()` | **Remove** | Same as `seq()` — non-functional standalone. Identical to `pat()` in intent. |
| `seq_step()` | **Remove** | Legacy opcode. State (events) only populatable via C++ API, never from Akkado. Superseded by SEQPAT system. |
| `timeline()` | **Keep** | Opcode works, Akkado syntax planned for a future PRD. Keep opcode and builtin entry. |
| `chord()` | **Keep** | Has its own codegen handler. Expands chord symbols to MIDI note arrays. Unique functionality. |
| `euclid()` | **Keep** | Euclidean rhythm generator. Unique, self-contained, state computed at runtime from inputs. |
| `trigger()` | **Keep** | Beat-division impulse. Unique, no redundancy. |
| `clock()` | **Keep** | Clock position query. Unique. |
| `lfo()` | **Keep** | Beat-synced LFO. Unique. |
| Pattern transforms (`slow`, `fast`, `rev`, `transpose`, `velocity`, `bank`, `n`) | **Keep** | Compile-time pattern transformations. Work with `pat()`. |

## Unreachable Opcodes (11)

These opcodes exist in the Cedar VM but are never emitted by the Akkado compiler.

### Array operations (6)

`ARRAY_CONCAT`, `ARRAY_FILL`, `ARRAY_LEN`, `ARRAY_REVERSE`, `ARRAY_SLICE`, `ARRAY_SUM`

Array ops were implemented in Cedar but the Akkado compiler never gained codegen for them. No builtin entries map to these opcodes.

### DISTORT_FOLD (1)

Shadowed by the array `fold()` builtin. The builtins map has two entries for `"fold"` — the array fold (later in the map) overwrites the distortion fold, making `DISTORT_FOLD` unreachable.

### 2x oscillator variants (4)

`OSC_SIN_2X`, `OSC_SAW_2X`, `OSC_SQR_2X`, `OSC_TRI_2X`

The FM detection logic in codegen jumps directly from 1x to 4x variants. The 2x variants are never selected.

## Legacy State Structs

- `SeqStepState` — only used by `SEQ_STEP`, remove with opcode
- `TimelineState` — used by `TIMELINE`, **keep** (timeline is being retained)

## Changes Required

### Part 1: Remove deprecated builtins

- `akkado/include/akkado/builtins.hpp` — remove `seq`, `note`, `seq_step` entries (keep `timeline`)
- `akkado/src/codegen_patterns.cpp` — remove `"seq"`, `"note"` from `is_pattern_expr()` and `compile_pattern_for_transform()` (keep `"timeline"`)

### Part 2: Remove unreachable opcodes

- `cedar/include/cedar/vm/instruction.hpp` — remove 11 opcodes from enum
- `cedar/include/cedar/opcodes/arrays.hpp` — remove array op implementations
- `cedar/include/cedar/opcodes/distortion.hpp` — remove DISTORT_FOLD case
- `cedar/include/cedar/opcodes/oscillators.hpp` — remove 2x variant cases
- `cedar/include/cedar/opcodes/sequencing.hpp` — remove SEQ_STEP implementation (keep TIMELINE)
- `cedar/include/cedar/opcodes/dsp_state.hpp` — remove SeqStepState, TimelineState
- `akkado/include/akkado/builtins.hpp` — remove duplicate `fold` entry mapping to DISTORT_FOLD

### Part 3: Regenerate & rebuild

- `cd web && bun run build:opcodes` — regenerate opcode metadata
- `cd web && bun run build:docs` — regenerate docs index

### Part 4: Update documentation

- `web/static/docs/reference/builtins/sequencing.md` — remove seq_step section (keep timeline)
- `web/static/docs/reference/mini-notation/basics.md` — remove seq(), note() references
- `docs/mini-notation-reference.md` — remove seq(), note() references (keep timeline())

### Part 5: Update tests

- Remove or update tests referencing removed functions/opcodes
- Mark experiment files as deprecated or remove
