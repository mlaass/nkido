#pragma once

#include "cedar/io/buffer.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

namespace cedar {

class SampleBank;

/// SF2 loop modes (match SF2 spec)
enum class SFLoopMode : std::uint8_t {
    None = 0,        // No looping — play to end
    Continuous = 1,  // Loop between loop_start/loop_end continuously
    Sustain = 3      // Loop during sustain, play through on release
};

/// SF2 envelope parameters (pre-converted from timecents to seconds)
struct SFEnvelope {
    float delay = 0.0f;     // Delay before envelope starts (seconds)
    float attack = 0.001f;  // Attack time (seconds)
    float hold = 0.0f;      // Hold time at peak (seconds)
    float decay = 0.001f;   // Decay time to sustain (seconds)
    float sustain = 1.0f;   // Sustain level (0-1 for amp, raw for mod)
    float release = 0.001f; // Release time (seconds)
    float keynumToHold = 0.0f;   // Key number scaling for hold
    float keynumToDecay = 0.0f;  // Key number scaling for decay
};

/// A zone within a SoundFont preset
/// Maps a key/velocity range to a sample with DSP parameters
struct SoundFontZone {
    // Key/velocity range
    std::uint8_t lo_key = 0;
    std::uint8_t hi_key = 127;
    std::uint8_t lo_vel = 0;
    std::uint8_t hi_vel = 127;

    // Sample reference (ID in SampleBank after extraction)
    std::uint32_t sample_id = 0;

    // Pitch
    std::uint8_t root_key = 60;       // MIDI note of unshifted sample
    std::int16_t tune = 0;            // Fine tuning in cents
    std::int16_t transpose = 0;       // Coarse tuning in semitones
    std::int16_t pitch_keytrack = 100; // Key tracking (100 = normal)

    // Loop
    SFLoopMode loop_mode = SFLoopMode::None;
    std::uint32_t loop_start = 0;  // Loop start in sample frames
    std::uint32_t loop_end = 0;    // Loop end in sample frames
    std::uint32_t sample_end = 0;  // Sample end in frames

    // Levels
    float attenuation = 0.0f;  // Initial attenuation (dB, 0-144)
    float pan = 0.0f;          // Pan (-0.5 left to +0.5 right)

    // Envelopes
    SFEnvelope amp_env;   // Amplitude envelope
    SFEnvelope mod_env;   // Modulation envelope

    // Filter
    float filter_fc = 13500.0f;  // Initial filter cutoff (Hz)
    float filter_q = 0.0f;       // Initial filter Q (dB, 0-96)

    // Modulation targets
    std::int16_t mod_env_to_pitch = 0;      // Mod env → pitch (cents)
    std::int16_t mod_env_to_filter_fc = 0;  // Mod env → filter (cents)

    // Sample rate of original sample
    float sample_rate = 44100.0f;

    // Exclusive class (for voice stealing within a group)
    std::uint32_t exclusive_class = 0;

    /// Check if a note/velocity falls within this zone
    [[nodiscard]] bool matches(std::uint8_t note, std::uint8_t vel) const {
        return note >= lo_key && note <= hi_key &&
               vel >= lo_vel && vel <= hi_vel;
    }
};

/// A preset within a SoundFont (e.g., "Acoustic Grand Piano")
struct SoundFontPreset {
    std::string name;
    std::uint16_t bank = 0;
    std::uint16_t program = 0;
    std::vector<SoundFontZone> zones;
};

/// A loaded SoundFont with presets and zones
struct SoundFontBank {
    std::string name;
    std::vector<SoundFontPreset> presets;

    /// Find preset by program/bank number
    /// Returns nullptr if not found
    [[nodiscard]] const SoundFontPreset* get_preset(std::uint16_t program,
                                                      std::uint16_t bank = 0) const {
        for (const auto& p : presets) {
            if (p.program == program && p.bank == bank) {
                return &p;
            }
        }
        return nullptr;
    }

    /// Find preset by index (0-based)
    [[nodiscard]] const SoundFontPreset* get_preset_by_index(std::size_t index) const {
        if (index >= presets.size()) return nullptr;
        return &presets[index];
    }

    /// Find matching zones for a note/velocity in a preset
    /// Writes up to max_zones results, returns count found
    /// Zero-allocation — writes to caller-provided array
    std::size_t find_zones(const SoundFontPreset& preset,
                           std::uint8_t note, std::uint8_t vel,
                           const SoundFontZone** out, std::size_t max_zones) const {
        std::size_t count = 0;
        for (const auto& zone : preset.zones) {
            if (zone.matches(note, vel)) {
                out[count++] = &zone;
                if (count >= max_zones) break;
            }
        }
        return count;
    }
};

/// Registry of loaded SoundFonts
/// Each SF2 file gets a unique ID (0-based)
class SoundFontRegistry {
public:
    static constexpr std::size_t MAX_SOUNDFONTS = 256;

    /// Load a SoundFont from memory, extracting samples into the SampleBank
    /// @param data Raw SF2 file bytes
    /// @param name Display name for this SoundFont
    /// @param sample_bank Target sample bank for extracted audio
    /// @return SoundFont ID (0-based), or -1 on failure
    int load_from_memory(MemoryView data,
                         const std::string& name,
                         SampleBank& sample_bank);

    /// Get a loaded SoundFont by ID
    [[nodiscard]] const SoundFontBank* get(int sf_id) const {
        auto idx = static_cast<std::size_t>(sf_id);
        if (sf_id < 0 || idx >= banks_.size()) {
            return nullptr;
        }
        return &banks_[idx];
    }

    /// Get number of loaded SoundFonts
    [[nodiscard]] std::size_t size() const { return banks_.size(); }

    /// Serialize preset list as JSON for web UI
    /// Format: [{"name":"...", "bank":0, "program":0, "zoneCount":N}, ...]
    [[nodiscard]] std::string get_presets_json(int sf_id) const;

private:
    std::vector<SoundFontBank> banks_;
};

}  // namespace cedar
