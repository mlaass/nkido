#include "akkado/tuning.hpp"

#include <charconv>
#include <cmath>

namespace akkado {

namespace {

// Tritave (3:1) in cents: 1200 * log2(3) ≈ 1901.955
constexpr float TRITAVE_CENTS = 1901.9550008653875f;

}  // namespace

std::optional<TuningContext> parse_tuning(std::string_view name) {
    if (name.empty()) return std::nullopt;

    // Named presets first (case-sensitive lowercase): "ji", "bp".
    if (name == "ji") {
        return TuningContext{TuningContext::Kind::JI, /*divisions=*/12, /*interval_cents=*/1200.0f};
    }
    if (name == "bp") {
        return TuningContext{TuningContext::Kind::BP, /*divisions=*/13, /*interval_cents=*/TRITAVE_CENTS};
    }

    // Try to parse "Nedo" format (e.g., "12edo", "31edo", "24edo").
    if (name.size() >= 4 && name.substr(name.size() - 3) == "edo") {
        std::string_view num_part = name.substr(0, name.size() - 3);
        int edo = 0;
        auto result = std::from_chars(num_part.data(), num_part.data() + num_part.size(), edo);
        if (result.ec == std::errc{} && result.ptr == num_part.data() + num_part.size() && edo > 0) {
            return TuningContext{TuningContext::Kind::EDO, edo, 1200.0f};
        }
    }

    // Try to parse "N-edo" or "N-EDO" format.
    auto dash_pos = name.find('-');
    if (dash_pos != std::string_view::npos) {
        std::string_view num_part = name.substr(0, dash_pos);
        std::string_view suffix = name.substr(dash_pos + 1);
        if (suffix == "edo" || suffix == "EDO") {
            int edo = 0;
            auto result = std::from_chars(num_part.data(), num_part.data() + num_part.size(), edo);
            if (result.ec == std::errc{} && result.ptr == num_part.data() + num_part.size() && edo > 0) {
                return TuningContext{TuningContext::Kind::EDO, edo, 1200.0f};
            }
        }
    }

    return std::nullopt;
}

} // namespace akkado
