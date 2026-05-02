#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace akkado {

/// Tuning context for microtonal pitch resolution.
/// Converts (midi_note, micro_offset) → Hz at compile time.
///
/// Default kind is `EDO` with 12 divisions of an octave (standard 12-TET).
/// Other kinds:
///   * `EDO` with arbitrary divisions — micro_offset is one EDO step.
///   * `JI`  — 5-limit symmetric 12-tone just intonation. micro_offset shifts
///             by one entry in the JI ratio array (so `c^4` lands on the next
///             scale degree's JI ratio).
///   * `BP`  — Bohlen-Pierce (13 equal divisions of the tritave 3:1).
///             micro_offset is one BP step (~146.3¢). midi_note is interpreted
///             as a step count along the BP scale anchored at the same MIDI
///             reference (60 = root).
struct TuningContext {
    enum class Kind : std::uint8_t { EDO, JI, BP };

    Kind kind = Kind::EDO;
    int divisions = 12;          // EDO/BP: number of divisions of `interval_cents`
    float interval_cents = 1200.0f;  // EDO=1200 (octave), BP=1901.955 (tritave 3:1)

    constexpr TuningContext() = default;
    constexpr TuningContext(Kind k, int d, float ic)
        : kind(k), divisions(d), interval_cents(ic) {}
    /// EDO convenience: `TuningContext{12}` is 12-EDO.
    constexpr explicit TuningContext(int edo_divisions)
        : kind(Kind::EDO), divisions(edo_divisions), interval_cents(1200.0f) {}

    /// 5-limit symmetric 12-tone JI scale (relative to C, sorted ascending).
    /// Used only when kind == JI.
    static constexpr std::array<float, 12> ji_5_limit_symmetric{
        1.0f,         // C   1/1
        16.0f / 15.0f,// C#  16/15
        9.0f / 8.0f,  // D   9/8
        6.0f / 5.0f,  // D#  6/5
        5.0f / 4.0f,  // E   5/4
        4.0f / 3.0f,  // F   4/3
        45.0f / 32.0f,// F#  45/32
        3.0f / 2.0f,  // G   3/2
        8.0f / 5.0f,  // G#  8/5
        5.0f / 3.0f,  // A   5/3
        9.0f / 5.0f,  // A#  9/5
        15.0f / 8.0f, // B   15/8
    };

    /// Step size in cents (EDO/BP only).
    [[nodiscard]] float step_cents() const {
        return interval_cents / static_cast<float>(divisions);
    }

    /// Resolve (midi_note, micro_offset) → Hz.
    /// midi_note is standard 12-EDO MIDI (60 = C4, 69 = A4 = 440 Hz).
    /// micro_offset is in units of the current tuning's micro step.
    [[nodiscard]] float resolve_hz(std::uint8_t midi_note, std::int8_t micro_offset) const {
        switch (kind) {
            case Kind::EDO: {
                // Standard EDO: nominal cents from 12-EDO MIDI + micro_offset * step_cents.
                float base_cents = (static_cast<float>(midi_note) - 69.0f) * 100.0f;
                float micro_cents = static_cast<float>(micro_offset) * step_cents();
                return 440.0f * std::pow(2.0f, (base_cents + micro_cents) / 1200.0f);
            }
            case Kind::BP: {
                // Bohlen-Pierce: midi_note acts as a step count along the BP
                // scale anchored at MIDI 60 (root). micro_offset adds further
                // BP steps. A4 (midi=69) is not 440Hz in BP — the reference is
                // the MIDI-60 root taken from 12-EDO (C4 ≈ 261.626 Hz).
                float c4_hz = 440.0f * std::pow(2.0f, (60.0f - 69.0f) / 12.0f);
                float steps = static_cast<float>(static_cast<int>(midi_note) - 60)
                              + static_cast<float>(micro_offset);
                return c4_hz * std::pow(2.0f, (steps * step_cents()) / 1200.0f);
            }
            case Kind::JI: {
                // 5-limit symmetric 12-tone JI, anchored at C4 (= 12-EDO C4).
                // Total chromatic step index = (midi - 60) + micro_offset.
                // pc = step mod 12, octave = floor(step / 12).
                int step = static_cast<int>(midi_note) - 60 + static_cast<int>(micro_offset);
                int pc = step % 12;
                int oct = step / 12;
                if (pc < 0) { pc += 12; oct -= 1; }
                float c4_hz = 440.0f * std::pow(2.0f, (60.0f - 69.0f) / 12.0f);
                return c4_hz * ji_5_limit_symmetric[static_cast<std::size_t>(pc)]
                       * std::pow(2.0f, static_cast<float>(oct));
            }
        }
        return 0.0f;  // unreachable
    }
};

/// Parse a tuning name string into a TuningContext.
/// Supported formats:
///   * `Nedo` / `N-edo` / `N-EDO` (e.g., "12edo", "31edo", "53-edo") — EDO
///   * `ji` — 5-limit symmetric 12-tone just intonation
///   * `bp` — Bohlen-Pierce (13edt, 13 equal divisions of the tritave 3:1)
/// Returns nullopt if the name is not recognized.
std::optional<TuningContext> parse_tuning(std::string_view name);

} // namespace akkado
