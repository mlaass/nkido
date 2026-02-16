#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace akkado {

/// Tuning context for microtonal pitch resolution.
/// Converts (midi_note, micro_offset) → Hz at compile time.
/// Default is 12-EDO (standard Western tuning), where micro_offset
/// maps to semitones (100 cents each). Other EDO systems divide
/// the octave into different step counts.
struct TuningContext {
    int edo_count = 12;  // divisions of the octave

    /// Resolve (midi_note, micro_offset) → Hz
    /// midi_note is standard 12-EDO MIDI (60 = C4, 69 = A4 = 440Hz)
    /// micro_offset is in units of the current EDO step size
    [[nodiscard]] float resolve_hz(std::uint8_t midi_note, std::int8_t micro_offset) const {
        float base_cents = (static_cast<float>(midi_note) - 69.0f) * 100.0f;
        float micro_cents = static_cast<float>(micro_offset) * (1200.0f / static_cast<float>(edo_count));
        return 440.0f * std::pow(2.0f, (base_cents + micro_cents) / 1200.0f);
    }
};

/// Parse a tuning name string into a TuningContext.
/// Supported formats: "12edo", "24edo", "31edo", etc.
/// Returns nullopt if the name is not recognized.
std::optional<TuningContext> parse_tuning(std::string_view name);

} // namespace akkado
