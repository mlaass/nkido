#pragma once

#include <cstdint>
#include <string>

namespace akkado {

/// Required sample with bank context, used both as a Pattern-value field
/// (`PatternPayload::sample_refs`) and as a CodeGenResult ledger entry
/// (`required_samples_extended_`). Lives in its own header so that
/// `typed_value.hpp` (which holds PatternPayload) and `codegen.hpp` (which
/// owns the global ledger) can both depend on it without a cycle.
struct RequiredSample {
    std::string bank;      // Bank name (empty = default)
    std::string name;      // Sample name (e.g., "bd", "snare")
    int variant = 0;       // Variant index (0 = first variant)

    /// Get a unique key for deduplication
    [[nodiscard]] std::string key() const {
        if (bank.empty()) {
            return variant > 0 ? name + ":" + std::to_string(variant) : name;
        }
        return bank + "/" + name + ":" + std::to_string(variant);
    }

    /// Get the qualified sample name for Cedar (e.g., "TR808_bd_0")
    [[nodiscard]] std::string qualified_name() const {
        if (bank.empty() || bank == "default") {
            return variant > 0 ? name + ":" + std::to_string(variant) : name;
        }
        return bank + "_" + name + "_" + std::to_string(variant);
    }
};

/// Mapping from a PUSH_CONST instruction (whose output buffer feeds
/// SAMPLE_PLAY's `id` input) to the sample-name reference that must be
/// resolved at runtime. Emitted by codegen for `sample(trig, pitch, "name")`
/// calls. After samples are loaded the host patches the instruction's
/// `state_id` immediate to the bank-assigned sample ID.
struct ScalarSampleMapping {
    std::uint32_t instruction_index = 0;  // Index of PUSH_CONST in instructions_
    std::string bank;                     // Bank name (empty = default)
    std::string name;                     // Sample name (e.g., "bd")
    int variant = 0;                      // Variant index (0 = first)
};

} // namespace akkado
