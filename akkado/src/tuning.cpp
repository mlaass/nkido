#include "akkado/tuning.hpp"

#include <charconv>

namespace akkado {

std::optional<TuningContext> parse_tuning(std::string_view name) {
    // Try to parse "Nedo" format (e.g., "12edo", "31edo", "24edo")
    if (name.size() >= 4 && name.substr(name.size() - 3) == "edo") {
        std::string_view num_part = name.substr(0, name.size() - 3);
        int edo = 0;
        auto result = std::from_chars(num_part.data(), num_part.data() + num_part.size(), edo);
        if (result.ec == std::errc{} && result.ptr == num_part.data() + num_part.size() && edo > 0) {
            return TuningContext{edo};
        }
    }

    // Try to parse "N-edo" or "N-EDO" format
    auto dash_pos = name.find('-');
    if (dash_pos != std::string_view::npos) {
        std::string_view num_part = name.substr(0, dash_pos);
        std::string_view suffix = name.substr(dash_pos + 1);
        if (suffix == "edo" || suffix == "EDO") {
            int edo = 0;
            auto result = std::from_chars(num_part.data(), num_part.data() + num_part.size(), edo);
            if (result.ec == std::errc{} && result.ptr == num_part.data() + num_part.size() && edo > 0) {
                return TuningContext{edo};
            }
        }
    }

    return std::nullopt;
}

} // namespace akkado
