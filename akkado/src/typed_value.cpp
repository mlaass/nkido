#include "akkado/typed_value.hpp"

namespace akkado {

std::vector<std::uint16_t> buffers_of(const TypedValue& tv) {
    switch (tv.type) {
        case ValueType::Array:
            if (tv.array) {
                std::vector<std::uint16_t> result;
                result.reserve(tv.array->elements.size());
                for (const auto& elem : tv.array->elements) {
                    result.push_back(elem.buffer);
                }
                return result;
            }
            if (tv.buffer != 0xFFFF) return {tv.buffer};
            return {};

        case ValueType::Signal:
        case ValueType::Number:
        case ValueType::Pattern:
            if (tv.buffer != 0xFFFF) return {tv.buffer};
            return {};

        default:
            return {};
    }
}

int pattern_field_index(const std::string& name) {
    // freq / pitch / f → 0
    if (name == "freq" || name == "pitch" || name == "f") return PatternPayload::FREQ;
    // vel / velocity / v → 1
    if (name == "vel" || name == "velocity" || name == "v") return PatternPayload::VEL;
    // trig / trigger / t → 2
    if (name == "trig" || name == "trigger" || name == "t") return PatternPayload::TRIG;
    // gate / g → 3
    if (name == "gate" || name == "g") return PatternPayload::GATE;
    // type → 4
    if (name == "type") return PatternPayload::TYPE;
    return -1;
}

TypedValue pattern_field(const TypedValue& tv, const std::string& name) {
    if (tv.type != ValueType::Pattern || !tv.pattern) {
        return TypedValue::error_val();
    }

    int idx = pattern_field_index(name);
    if (idx < 0) return TypedValue::error_val();

    std::uint16_t buf = tv.pattern->fields[static_cast<std::size_t>(idx)];
    if (buf == 0xFFFF) return TypedValue::error_val();

    return TypedValue::signal(buf);
}

} // namespace akkado
