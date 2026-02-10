/**
 * SoundFont (SF2) parsing and sample extraction
 *
 * Uses TinySoundFont (tsf.h) to parse SF2 files, then extracts samples
 * and zone metadata into Cedar's own data structures. TSF is only used
 * for parsing — playback is handled by Cedar's own opcode.
 */

// Suppress warnings from third-party header
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wcomma"
#endif
#endif

// Include stb_vorbis declarations (header-only) for SF3 support in TSF
// Implementation is compiled in audio_decoder.cpp
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define TSF_NO_STDIO
#define TSF_IMPLEMENTATION
#include "tsf.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cedar/audio/soundfont.hpp>
#include <cedar/vm/sample_bank.hpp>
#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace cedar {

int SoundFontRegistry::load_from_memory(const void* data, int size,
                                         const std::string& name,
                                         SampleBank& sample_bank) {
    if (!data || size <= 0) return -1;
    if (banks_.size() >= MAX_SOUNDFONTS) return -1;

    // Parse SF2 with TinySoundFont
    tsf* sf = tsf_load_memory(data, size);
    if (!sf) {
        std::printf("[SoundFont] Failed to parse SF2: %s\n", name.c_str());
        return -1;
    }

    int sf_id = static_cast<int>(banks_.size());
    SoundFontBank bank;
    bank.name = name;

    int preset_count = tsf_get_presetcount(sf);
    std::printf("[SoundFont] Loading '%s': %d presets\n", name.c_str(), preset_count);

    // Track which raw sample offsets have already been extracted
    // Maps (sample_offset) → (sample_id in SampleBank)
    std::unordered_map<std::uint32_t, std::uint32_t> sample_cache;

    for (int pi = 0; pi < preset_count; ++pi) {
        const tsf_preset& tsf_p = sf->presets[pi];

        SoundFontPreset preset;
        preset.name = tsf_p.presetName;
        preset.bank = tsf_p.bank;
        preset.program = tsf_p.preset;

        for (int ri = 0; ri < tsf_p.regionNum; ++ri) {
            const tsf_region& r = tsf_p.regions[ri];

            SoundFontZone zone;

            // Key/velocity range
            zone.lo_key = r.lokey;
            zone.hi_key = r.hikey;
            zone.lo_vel = r.lovel;
            zone.hi_vel = r.hivel;

            // Pitch
            zone.root_key = static_cast<std::uint8_t>(r.pitch_keycenter);
            zone.tune = static_cast<std::int16_t>(r.tune);
            zone.transpose = static_cast<std::int16_t>(r.transpose);
            zone.pitch_keytrack = static_cast<std::int16_t>(r.pitch_keytrack);

            // Loop
            zone.loop_mode = static_cast<SFLoopMode>(r.loop_mode);
            zone.loop_start = r.loop_start;
            zone.loop_end = r.loop_end;
            zone.sample_end = r.end;

            // Adjust loop points relative to sample start offset
            if (zone.loop_start >= r.offset) {
                zone.loop_start -= r.offset;
            }
            if (zone.loop_end >= r.offset) {
                zone.loop_end -= r.offset;
            }
            if (zone.sample_end >= r.offset) {
                zone.sample_end -= r.offset;
            }

            // Levels
            zone.attenuation = r.attenuation;
            zone.pan = r.pan;

            // Amplitude envelope (already converted from timecents by tsf)
            zone.amp_env.delay = r.ampenv.delay;
            zone.amp_env.attack = r.ampenv.attack;
            zone.amp_env.hold = r.ampenv.hold;
            zone.amp_env.decay = r.ampenv.decay;
            zone.amp_env.sustain = r.ampenv.sustain;
            zone.amp_env.release = r.ampenv.release;
            zone.amp_env.keynumToHold = r.ampenv.keynumToHold;
            zone.amp_env.keynumToDecay = r.ampenv.keynumToDecay;

            // Modulation envelope
            zone.mod_env.delay = r.modenv.delay;
            zone.mod_env.attack = r.modenv.attack;
            zone.mod_env.hold = r.modenv.hold;
            zone.mod_env.decay = r.modenv.decay;
            zone.mod_env.sustain = r.modenv.sustain;
            zone.mod_env.release = r.modenv.release;
            zone.mod_env.keynumToHold = r.modenv.keynumToHold;
            zone.mod_env.keynumToDecay = r.modenv.keynumToDecay;

            // Filter
            // TSF stores initialFilterFc as absolute cents (0-13500)
            // Convert to Hz: Hz = 8.176 * 2^(cents/1200)
            if (r.initialFilterFc > 0 && r.initialFilterFc < 13500) {
                zone.filter_fc = 8.176f * std::pow(2.0f, static_cast<float>(r.initialFilterFc) / 1200.0f);
            } else {
                zone.filter_fc = 20000.0f;  // Wide open
            }
            // Q is stored as centibels (0-960), convert to dB
            zone.filter_q = static_cast<float>(r.initialFilterQ) / 10.0f;

            // Modulation targets
            zone.mod_env_to_pitch = static_cast<std::int16_t>(r.modEnvToPitch);
            zone.mod_env_to_filter_fc = static_cast<std::int16_t>(r.modEnvToFilterFc);

            zone.sample_rate = static_cast<float>(r.sample_rate);
            zone.exclusive_class = r.group;

            // Extract sample data into SampleBank (deduplicate by offset)
            std::uint32_t sample_offset = r.offset;
            std::uint32_t sample_length = r.end - r.offset;

            if (sample_length > 0 && sf->fontSamples) {
                auto it = sample_cache.find(sample_offset);
                if (it != sample_cache.end()) {
                    // Already extracted this sample
                    zone.sample_id = it->second;
                } else {
                    // Extract new sample
                    // Name format: _sf<id>_s<offset> (internal, not user-facing)
                    std::string sample_name = "_sf" + std::to_string(sf_id) +
                                              "_s" + std::to_string(sample_offset);

                    std::uint32_t sid = sample_bank.load_sample(
                        sample_name,
                        sf->fontSamples + sample_offset,
                        sample_length,
                        1,  // SF2 samples are mono
                        static_cast<float>(r.sample_rate)
                    );

                    if (sid > 0) {
                        sample_cache[sample_offset] = sid;
                        zone.sample_id = sid;
                    }
                }
            }

            preset.zones.push_back(std::move(zone));
        }

        bank.presets.push_back(std::move(preset));
    }

    std::printf("[SoundFont] Loaded '%s': %zu presets, %zu unique samples extracted\n",
                name.c_str(), bank.presets.size(), sample_cache.size());

    // Clean up TSF — we've extracted everything we need
    tsf_close(sf);

    banks_.push_back(std::move(bank));
    return sf_id;
}

std::string SoundFontRegistry::get_presets_json(int sf_id) const {
    const auto* bank = get(sf_id);
    if (!bank) return "[]";

    std::ostringstream json;
    json << "[";

    for (std::size_t i = 0; i < bank->presets.size(); ++i) {
        const auto& p = bank->presets[i];
        if (i > 0) json << ",";

        json << "{\"name\":\"";
        // Escape preset name for JSON
        for (char c : p.name) {
            switch (c) {
                case '"': json << "\\\""; break;
                case '\\': json << "\\\\"; break;
                case '\n': json << "\\n"; break;
                default: json << c; break;
            }
        }
        json << "\",\"bank\":" << p.bank
             << ",\"program\":" << p.program
             << ",\"zoneCount\":" << p.zones.size()
             << "}";
    }

    json << "]";
    return json.str();
}

}  // namespace cedar
