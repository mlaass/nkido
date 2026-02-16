> **Status: NOT STARTED** — No microtonal parsing or tuning engine.

# **Product Requirement Document: Akkado Microtonal Extension**

**Version:** 2.0

**Status:** Draft / Planned

**Feature:** Microtonal Notation & Tuning Engine

## **1. Objective**

To extend the Akkado mini-notation syntax to support arbitrary, agnostic microtonal composition. The system must allow users to express precise pitch deviations (using steps, cents, or ratios) independent of the underlying tuning system (12-EDO, 31-EDO, JI, etc.), preserving the fluidity of live coding.

## **2. Core Philosophy**

* **Agnostic Notation:** The syntax defines *relative relationships* (e.g., "up one step"), not absolute frequencies.
* **Compile-Time Resolution:** `tune()` is a compile-time context modifier that resolves microtonal notation to Hz during pattern compilation. The Cedar runtime sees only Hz — no structural changes to `Sequence::Event`, `OutputEvent`, `PolyAllocState`, or any opcode.
* **Backward Compatibility:** Standard Western notation (`c#4`, `Bb5`) functions exactly as before when no custom tuning is applied (default 12-EDO context).
* **Individual Notes Only:** Microtonal operators apply to single pitch atoms. Chord symbols (`C4'`, `Am7'`) remain in 12-TET. Microtonal chord voicings are deferred.

## **3. Syntax Specification**

### **3.1. Extended Pitch Atom**

The Pitch Atom definition is expanded to accept a **stream of modifiers** between the Note Nominal and the Octave.

**Format:** [Note Nominal] + [Modifier Stream] + [Octave]

### **3.2. Microtonal Operators**

Two new primary operators control micro-steps.

| Operator | Name | Function |
| :---- | :---- | :---- |
| ^ | Step Up | Increments the micro\_offset counter by 1. |
| v | Step Down | Decrements the micro\_offset counter by 1. |

### **3.3. Numeric Arguments**

To support large intervals or specific harmonic indices, operators accept numeric arguments.

* ^n (e.g., ^4) is semantically equivalent to repeating the operator n times (^^^^).
* vn (e.g., v2) is semantically equivalent to repeating v n times.

## **4. Parser Implementation Requirements**

### **4.1. The "Stacking" Strategy**

The parser must not rely on rigid token matching (e.g., looking for "bb" or "x"). Instead, it must parse the modifier section as a continuous stream of characters, accumulating values into two distinct registers.

**Registers:**

1. **accidental\_std (Standard):** Tracks chromatic semitones (12-TET basis).
2. **micro\_offset (Micro):** Tracks system-specific micro-steps.

**Accumulation Logic Table:**

| Character | Register Affected | Delta | Description |
| :---- | :---- | :---- | :---- |
| \# | accidental\_std | +1 | Standard Sharp |
| b | accidental\_std | -1 | Standard Flat |
| ^ | micro\_offset | +1 | Micro Step Up |
| v | micro\_offset | -1 | Micro Step Down |

### **4.2. Alias Support**

To support community standards (Stein-Zimmermann, HEJI), the parser must recognize specific ASCII aliases. These aliases map directly to the registers above.

| Alias | Register | Delta | Standard Meaning |
| :---- | :---- | :---- | :---- |
| d | micro\_offset | -1 | Inverted Flat / Quarter-tone Flat |
| \\ | micro\_offset | -1 | Alternative Down |
| + | micro\_offset | +1 | Half Sharp / Quarter-tone Sharp |
| x | accidental\_std | +2 | Double Sharp |

**Parsing Edge Case:**

The character `d` is ambiguous (Note D vs. Alias d). The current lexer structure naturally handles this:

* **Rule:** If d appears immediately following a Note Nominal or another Modifier (e.g., `cd4` or `cbd4`), it is parsed as an Alias — it's consumed as part of the preceding note's modifier stream.
* **Rule:** If d starts a new token (e.g., `c d4` separated by whitespace), it is parsed as a new Pitch Atom (note D).

### **4.3. Parsing Examples & Resulting State**

| Input String | accidental\_std | micro\_offset | Rationale |
| :---- | :---- | :---- | :---- |
| c\#4 | +1 | 0 | Standard sharp |
| c^4 | 0 | +1 | Pure micro step |
| c\#^4 | +1 | +1 | Sharp + Micro step (Mixed) |
| cbb4 | -2 | 0 | Stacked flats |
| cbd4 | -1 | -1 | Flat + Inverted Flat (Sesquiflat) |
| c+4 | 0 | +1 | Alias usage |

### **4.4. Implementation Mapping to Codebase**

1. **`MiniLexer::is_accidental()`** — Currently recognizes `#` and `b` only. Extend to include `^`, `v`, `+`, `d`, `\`, `x`.

2. **`MiniLexer::lex_pitch_or_sample()`** — Currently parses `[a-g][#b]?[0-9]?`. Refactor to parse the full modifier stream between note letter and octave digit, accumulating `accidental_std` and `micro_offset` separately.

3. **`MiniPitchData`** — Currently `{ uint8_t midi; bool has_octave; float velocity; }`. Add `int8_t micro_offset` field.

4. **`MiniAtomData`** — Currently stores `uint8_t midi_note`. Add `int8_t micro_offset` field alongside it. The `midi_note` continues to include standard accidentals as today.

5. **`codegen_patterns.cpp` (line ~313)** — The Hz conversion `440 * 2^((midi-69)/12)` needs to consult the active tuning context to incorporate `micro_offset`.

## **5. Architecture & Data Flow**

### **5.1. Compile-Time Resolution**

The tuning system resolves all pitch information to Hz during pattern compilation. No microtonal data flows into Cedar runtime structures.

```
Mini-notation: "c#^4"
    → MiniLexer: accidental_std=+1, micro_offset=+1, octave=4
    → AST: MiniAtomData { midi_note: 61, micro_offset: +1 }
    → Codegen + TuningContext: resolve(midi=61, micro=+1, "31edo") → Hz
    → Sequence::Event { values[0] = freq_hz }   ← Hz only, as today
    → Runtime: unchanged
```

### **5.2. The Tuning Context (compile-time)**

`tune()` is a compile-time scope modifier that sets the active tuning context for all patterns within its scope.

```akkado
tune("31edo") {
    pat("c^4 e4 gv4") |> poly(8, lead) |> out(%, %)
}
```

**Resolution Logic (Pseudocode):**

* **EDO (Equal Division of Octave):**
  * StepSize = 1200 / EDO\_Count
  * Nominal\_Cents = MidiToCents(event.midi\_note)   // includes accidental\_std
  * Micro\_Shift = event.micro\_offset * StepSize
  * Total\_Cents = Nominal\_Cents + Micro\_Shift
  * freq = 440 * 2^((Total\_Cents - 6900) / 1200)

* **Just Intonation:**
  * The micro\_offset acts as an index traverser in a ratio array, relative to the nominal anchor.

### **5.3. Instrument Compatibility**

The compile-time Hz resolution means all instrument types work without modification:

| Instrument | Pitch Input | Microtonal Status |
| :---- | :---- | :---- |
| **Oscillators** (via POLY) | Hz per-sample buffer | Works automatically — Hz-native |
| **Sampler** | Pitch multiplier (continuous) | Works automatically — no quantization |
| **SoundFont** | Hz → MIDI conversion | **Needs fix** (see Section 5.4) |
| **Future** (KS, granular, wavetable) | Hz buffer (convention) | Works automatically |

The POLY system is tuning-agnostic: it receives Hz from pattern events, fills `voice_freq_buf` per voice, and the instrument body reads Hz. Microtonal patterns through POLY work identically to 12-TET patterns.

### **5.4. SoundFont Microtonal Fix**

The SoundFont player (`soundfont.hpp`) converts Hz back to MIDI at runtime for zone lookup:

```cpp
float midi_note = 69.0f + 12.0f * std::log2(freq / 440.0f);
uint8_t note = static_cast<uint8_t>(std::roundf(midi_note));  // ← QUANTIZES to semitone
```

This `roundf()` destroys microtonal information. The pitch calculation then uses the quantized value:

```cpp
float pitch_cents = (note - zone.root_key + zone.transpose) * 100.0f + zone.tune;
```

**Fix:** Preserve the exact fractional MIDI note for pitch calculation. Use rounded MIDI only for zone selection:

```cpp
float midi_exact = 69.0f + 12.0f * std::log2(freq / 440.0f);
uint8_t note = static_cast<uint8_t>(std::roundf(midi_exact));  // zone lookup only
// ... zone selection uses rounded 'note' ...
float pitch_cents = (midi_exact - static_cast<float>(zone.root_key)
                     + static_cast<float>(zone.transpose)) * 100.0f
                    + static_cast<float>(zone.tune);
float pitch_ratio = std::pow(2.0f, pitch_cents / 1200.0f);
voice->speed = pitch_ratio * (zone.sample_rate / ctx.sample_rate);
```

## **6. Standard Library & Configuration**

### **6.1. Built-in Library**

The system ships with definitions for common xenharmonic systems.

* **12edo** (Default)
* **17edo, 19edo, 22edo** (Superparticular)
* **24edo** (Quarter-tone)
* **31edo** (Meantone/Huygens)
* **41edo, 53edo** (High-count approximations)
* **ji** (5-limit symmetric)
* **bp** (Bohlen-Pierce non-octave)

### **6.2. External Loading API (Future)**

Users will be able to load .scl (Scala) and .tun files. Deferred to post-MVP.

```akkado
load_tuning("slendro", "assets/slendro.scl")
tune("slendro") {
    pat("n0 n1 n2") |> poly(4, inst) |> out(%, %)
}
```

## **7. Development Roadmap**

### Phase 1: Parser Update
- Refactor `MiniLexer::lex_pitch_or_sample()` for modifier stream parsing (`^`, `v`, `+`, `d`, `\`, `x`, numeric args)
- Add `micro_offset` to `MiniPitchData` and `MiniAtomData`
- Standard accidentals (`#`, `b`, `x`) continue affecting `midi_note` as today
- New operators (`^`, `v`, `+`, `d`, `\`) accumulate into `micro_offset`
- **Files:** `akkado/src/mini_lexer.cpp`, `akkado/include/akkado/mini_lexer.hpp`, `akkado/include/akkado/ast.hpp`, `akkado/include/akkado/mini_token.hpp`
- Unit tests for all stacking examples from Section 4.3

### Phase 2: Tuning Context & Codegen
- Implement `TuningContext` struct (EDO count, step-to-cents mapping, or ratio table)
- `tune()` as a compile-time scope modifier that sets the active tuning
- Modify `codegen_patterns.cpp` Hz conversion to use `(base_midi, micro_offset, tuning_context)` → Hz
- Default context: 12-EDO where each `micro_offset` step = 100 cents (one semitone)
- **Files:** `akkado/src/codegen_patterns.cpp`, `akkado/include/akkado/codegen/` (new tuning context)
- Built-in presets: 12, 17, 19, 22, 24, 31, 41, 53 EDO + JI + BP

### Phase 3: SoundFont Microtonal Fix
- Preserve exact fractional MIDI in `soundfont.hpp` pitch calculation
- Use rounded MIDI only for zone selection, exact for playback speed
- **Files:** `cedar/include/cedar/opcodes/soundfont.hpp`
- Test with quarter-tone pitches through SF player

### Future
- Scala/TUN file loading
- JI ratio tables with configurable prime limits
- Microtonal chord voicings (reinterpret chord intervals through tuning context)
- Runtime `detune()` parameter for real-time micro-adjustments on top of compiled tuning
