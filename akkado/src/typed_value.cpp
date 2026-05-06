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

namespace {
// Single source of truth for pattern field name → PatternPayload index.
// First entry of each row is the canonical name (used by available_fields()
// in E136 diagnostics); remaining entries are aliases.
struct FieldRow {
    std::size_t index;
    std::initializer_list<const char*> names;
};

inline const auto& pattern_field_table() {
    static const std::array<FieldRow, 11> kTable = {{
        {PatternPayload::FREQ,      {"freq", "frequency", "pitch", "f", "p"}},
        {PatternPayload::VEL,       {"vel", "velocity", "v"}},
        {PatternPayload::TRIG,      {"trig", "trigger", "t"}},
        {PatternPayload::GATE,      {"gate", "g"}},
        {PatternPayload::TYPE,      {"type"}},
        {PatternPayload::NOTE,      {"note", "midi", "n"}},
        {PatternPayload::DUR,       {"dur", "duration"}},
        {PatternPayload::CHANCE,    {"chance"}},
        {PatternPayload::TIME,      {"time", "t0", "start"}},
        {PatternPayload::PHASE,     {"phase", "cycle", "co"}},
        {PatternPayload::SAMPLE_ID, {"sample_id", "sample", "s"}},
    }};
    return kTable;
}
} // namespace

int pattern_field_index(const std::string& name) {
    for (const auto& row : pattern_field_table()) {
        for (const char* alias : row.names) {
            if (name == alias) return static_cast<int>(row.index);
        }
    }
    return -1;
}

TypedValue pattern_field(const TypedValue& tv, const std::string& name) {
    if (tv.type != ValueType::Pattern || !tv.pattern) {
        return TypedValue::error_val();
    }

    int idx = pattern_field_index(name);
    if (idx >= 0) {
        std::uint16_t buf = tv.pattern->fields[static_cast<std::size_t>(idx)];
        if (buf == 0xFFFF) return TypedValue::error_val();
        return TypedValue::signal(buf);
    }

    // Phase 2.1 PRD §11.1: custom-property pipe-binding accessor.
    auto it = tv.pattern->custom_fields.find(name);
    if (it != tv.pattern->custom_fields.end() && it->second != 0xFFFF) {
        return TypedValue::signal(it->second);
    }
    return TypedValue::error_val();
}

std::string available_fields(const PatternPayload& payload) {
    std::string s;
    bool first = true;
    for (const auto& row : pattern_field_table()) {
        // Skip rows with no buffer wired up (e.g. reserved slots that the
        // codegen hasn't allocated). Today every row except potential future
        // additions populates a buffer, but this keeps the message truthful.
        if (row.index >= payload.fields.size() ||
            payload.fields[row.index] == 0xFFFF) {
            continue;
        }
        if (!first) s += ", ";
        s += *row.names.begin();  // canonical name
        first = false;
    }
    for (const auto& [key, buf] : payload.custom_fields) {
        if (buf == 0xFFFF) continue;
        if (!first) s += ", ";
        s += key;
        first = false;
    }
    return s;
}

} // namespace akkado
