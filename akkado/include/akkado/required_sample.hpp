#pragma once

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

} // namespace akkado
