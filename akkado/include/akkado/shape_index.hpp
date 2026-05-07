#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace akkado {

/// Sentinel passed for `cursor_offset` to disable patternHole resolution.
constexpr std::uint32_t SHAPE_INDEX_NO_CURSOR = 0xFFFFFFFFu;

/// Tolerant parse + analyze of `source`, then serialize a JSON shape
/// index for editor autocomplete (Phase 2 of records-system-unification PRD).
///
/// Walks the global symbol table after analysis and emits per-binding shape
/// records for every top-level `Record`, `Pattern`, or `Array`-of-`Record`
/// binding. Pattern shapes include the 11 fixed PatternPayload slots, every
/// alias from `pattern_field_aliases()`, and source-derived `custom_fields`
/// from `.set()` chains (with collision dedupe — fixed wins per PRD §10.5).
///
/// `cursor_offset` is a UTF-8 byte offset in `source`. When the cursor sits
/// inside a `Pipe` whose LHS resolves to a Pattern-typed top-level binding,
/// the result includes a top-level `patternHole` entry with that pattern's
/// shape. Pass `SHAPE_INDEX_NO_CURSOR` to skip the resolution entirely
/// (e.g. when the editor knows the cursor is inside a string literal).
///
/// Survives parse/analyze errors — bindings that successfully bound before
/// the first error still surface. Never throws.
std::string shape_index_json(std::string_view source,
                             std::uint32_t cursor_offset = SHAPE_INDEX_NO_CURSOR);

}  // namespace akkado
