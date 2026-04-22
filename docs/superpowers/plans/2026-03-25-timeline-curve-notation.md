# Timeline Curve Notation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add ASCII-art curve notation (`t"__/''\\__"`) that compiles to Cedar's existing TIMELINE opcode breakpoints, enabling visual automation envelopes in Akkado.

**Architecture:** The curve notation extends the existing mini-notation pipeline (lexer -> parser -> eval -> codegen) with a `curve_mode` flag. In curve mode, characters like `_`, `.`, `-`, `^`, `'` become value levels, `/` and `\` become ramps, and `~` becomes a smooth modifier. The evaluated curve events are converted to `TimelineState::Breakpoint` entries and emitted via `StateInitData::Type::Timeline`. No Cedar VM changes required — the `TIMELINE` opcode and `TimelineState` already exist.

**Tech Stack:** C++20, Catch2 (testing), CMake

**PRD:** `docs/PRD-Timeline-Curve-Notation.md`

---

## File Structure

### New Files
None — all changes modify existing files.

### Modified Files

| File | Responsibility |
|------|---------------|
| `akkado/include/akkado/mini_token.hpp` | Add `CurveLevel`, `CurveRamp`, `CurveSmooth` token types + `MiniCurveLevelData` |
| `akkado/include/akkado/mini_lexer.hpp` | Add `curve_mode` constructor param |
| `akkado/src/mini_lexer.cpp` | Curve-mode lexing for `_./^'-\~` + `/` disambiguation |
| `akkado/include/akkado/ast.hpp` | Add `CurveLevel`, `CurveRamp` to `MiniAtomKind`; add `curve_value`, `curve_smooth` to `MiniAtomData` |
| `akkado/include/akkado/mini_parser.hpp` | Declare `parse_curve_level()`, `parse_curve_ramp()` |
| `akkado/src/mini_parser.cpp` | Parse curve tokens into AST atoms; update `is_atom_start()` |
| `akkado/include/akkado/pattern_event.hpp` | Add `CurveLevel`, `CurveRamp` to `PatternEventType`; add `curve_value`, `curve_smooth` to `PatternEvent` |
| `akkado/src/pattern_eval.cpp` | Handle curve atoms in `eval_atom()` |
| `akkado/include/akkado/token.hpp` | Add `Timeline` to `TokenType` |
| `akkado/src/lexer.cpp` | Lex `t"..."` / `t\`...\`` prefix |
| `akkado/src/parser.cpp` | Handle `TokenType::Timeline` in `parse_prefix()`, route to curve-mode `parse_mini` |
| `akkado/src/codegen_patterns.cpp` | `events_to_breakpoints()` conversion; emit TIMELINE opcode with `StateInitData::Type::Timeline` |
| `web/wasm/nkido_wasm.cpp` | Handle `Timeline` type in `cedar_apply_state_inits()` |
| `akkado/tests/test_mini_notation.cpp` | All curve notation tests (lexer, parser, eval) |
| `akkado/tests/test_lexer.cpp` | `t"..."` prefix tokenization tests |
| `akkado/tests/test_codegen.cpp` | Breakpoint generation and TIMELINE opcode emission tests |

---

## Task 1: Mini-Lexer Curve Token Types

Add curve token types and curve-mode lexing so the mini-lexer can tokenize curve strings.

**Files:**
- Modify: `akkado/include/akkado/mini_token.hpp:14-51` (MiniTokenType enum), `:54-83` (name function), `:109-117` (MiniTokenValue variant)
- Modify: `akkado/include/akkado/mini_lexer.hpp:10-11,30-36,87-95` (constructor + curve_mode field)
- Modify: `akkado/src/mini_lexer.cpp:10-14` (constructor), `:202-283` (lex_token), `:639-644` (lex_mini)
- Test: `akkado/tests/test_mini_notation.cpp`

- [ ] **Step 1: Write failing tests for curve token lexing**

Add to `akkado/tests/test_mini_notation.cpp`:

```cpp
// ============================================================================
// Curve Mode Lexer Tests [curve_lexer]
// ============================================================================

TEST_CASE("Curve mode lexes level characters", "[curve_lexer]") {
    auto [tokens, diags] = lex_mini("_ . - ^ '", {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(tokens.size() == 6); // 5 levels + Eof

    CHECK(tokens[0].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[0].value).value == 0.0f);

    CHECK(tokens[1].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[1].value).value == 0.25f);

    CHECK(tokens[2].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[2].value).value == 0.5f);

    CHECK(tokens[3].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[3].value).value == 0.75f);

    CHECK(tokens[4].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[4].value).value == 1.0f);
}

TEST_CASE("Curve mode lexes ramp characters", "[curve_lexer]") {
    auto [tokens, diags] = lex_mini("/ \\", {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(tokens.size() == 3); // 2 ramps + Eof
    CHECK(tokens[0].type == MiniTokenType::CurveRamp);
    CHECK(tokens[1].type == MiniTokenType::CurveRamp);
}

TEST_CASE("Curve mode lexes smooth modifier", "[curve_lexer]") {
    auto [tokens, diags] = lex_mini("~", {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(tokens.size() == 2); // CurveSmooth + Eof
    CHECK(tokens[0].type == MiniTokenType::CurveSmooth);
}

TEST_CASE("Curve mode _ is CurveLevel not Elongate", "[curve_lexer]") {
    // In standard mode, _ is Elongate
    auto [std_tokens, std_diags] = lex_mini("_", {}, false, false);
    CHECK(std_tokens[0].type == MiniTokenType::Elongate);

    // In curve mode, _ is CurveLevel(0.0)
    auto [curve_tokens, curve_diags] = lex_mini("_", {}, false, true);
    CHECK(curve_tokens[0].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(curve_tokens[0].value).value == 0.0f);
}

TEST_CASE("Curve mode ~ is CurveSmooth not Rest", "[curve_lexer]") {
    // In standard mode, ~ is Rest
    auto [std_tokens, std_diags] = lex_mini("~", {}, false, false);
    CHECK(std_tokens[0].type == MiniTokenType::Rest);

    // In curve mode, ~ is CurveSmooth
    auto [curve_tokens, curve_diags] = lex_mini("~", {}, false, true);
    CHECK(curve_tokens[0].type == MiniTokenType::CurveSmooth);
}

TEST_CASE("Curve mode / disambiguation", "[curve_lexer]") {
    // / followed by digit = Slash (slow modifier)
    auto [tokens1, diags1] = lex_mini("_/2", {}, false, true);
    REQUIRE(diags1.empty());
    CHECK(tokens1[0].type == MiniTokenType::CurveLevel);
    CHECK(tokens1[1].type == MiniTokenType::Slash);
    CHECK(tokens1[2].type == MiniTokenType::Number);

    // / followed by non-digit = CurveRamp
    auto [tokens2, diags2] = lex_mini("_/'", {}, false, true);
    REQUIRE(diags2.empty());
    CHECK(tokens2[0].type == MiniTokenType::CurveLevel);
    CHECK(tokens2[1].type == MiniTokenType::CurveRamp);
    CHECK(tokens2[2].type == MiniTokenType::CurveLevel);
}

TEST_CASE("Curve mode grouping and modifiers still work", "[curve_lexer]") {
    auto [tokens, diags] = lex_mini("[_'] *4", {}, false, true);
    REQUIRE(diags.empty());
    CHECK(tokens[0].type == MiniTokenType::LBracket);
    CHECK(tokens[1].type == MiniTokenType::CurveLevel);
    CHECK(tokens[2].type == MiniTokenType::CurveLevel);
    CHECK(tokens[3].type == MiniTokenType::RBracket);
    CHECK(tokens[4].type == MiniTokenType::Star);
}

TEST_CASE("Curve mode mini_token_type_name", "[curve_lexer]") {
    CHECK(mini_token_type_name(MiniTokenType::CurveLevel) == "CurveLevel");
    CHECK(mini_token_type_name(MiniTokenType::CurveRamp) == "CurveRamp");
    CHECK(mini_token_type_name(MiniTokenType::CurveSmooth) == "CurveSmooth");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[curve_lexer]"`
Expected: Compilation fails — `CurveLevel`, `CurveRamp`, `CurveSmooth`, `MiniCurveLevelData` don't exist yet.

- [ ] **Step 3: Add curve token types to mini_token.hpp**

In `akkado/include/akkado/mini_token.hpp`, add to the `MiniTokenType` enum (after `Elongate`, before `Number`):

```cpp
    // Curve notation (timeline automation)
    CurveLevel,     // _, ., -, ^, ' (value level atom in curve mode)
    CurveRamp,      // /, \          (ramp atom in curve mode)
    CurveSmooth,    // ~             (smooth modifier prefix in curve mode)
```

Add `MiniCurveLevelData` struct (after `MiniChordData`):

```cpp
/// Curve level data for timeline notation
struct MiniCurveLevelData {
    float value;  // 0.0, 0.25, 0.5, 0.75, or 1.0
};
```

Add `MiniCurveLevelData` to the `MiniTokenValue` variant:

```cpp
using MiniTokenValue = std::variant<
    std::monostate,     // For punctuation/operators
    double,             // For numbers
    MiniPitchData,      // For pitch tokens
    MiniSampleData,     // For sample tokens
    MiniChordData,      // For chord tokens
    MiniCurveLevelData, // For curve level tokens
    std::string         // For error messages
>;
```

Add to `mini_token_type_name()`:

```cpp
        case MiniTokenType::CurveLevel:  return "CurveLevel";
        case MiniTokenType::CurveRamp:   return "CurveRamp";
        case MiniTokenType::CurveSmooth: return "CurveSmooth";
```

Add accessor to `MiniToken`:

```cpp
    /// Get curve level data (assumes type == CurveLevel)
    [[nodiscard]] const MiniCurveLevelData& as_curve_level() const {
        return std::get<MiniCurveLevelData>(value);
    }
```

- [ ] **Step 4: Add curve_mode to MiniLexer**

In `akkado/include/akkado/mini_lexer.hpp`, modify the constructor signature:

```cpp
    explicit MiniLexer(std::string_view pattern, SourceLocation base_location = {},
                       bool sample_only = false, bool curve_mode = false);
```

Add private member:

```cpp
    bool curve_mode_ = false;  // When true, lex curve notation characters
```

Update the free function signature:

```cpp
std::pair<std::vector<MiniToken>, std::vector<Diagnostic>>
lex_mini(std::string_view pattern, SourceLocation base_location = {},
         bool sample_only = false, bool curve_mode = false);
```

- [ ] **Step 5: Implement curve-mode lexing in mini_lexer.cpp**

Update constructor (line 10-14):

```cpp
MiniLexer::MiniLexer(std::string_view pattern, SourceLocation base_location,
                     bool sample_only, bool curve_mode)
    : pattern_(pattern)
    , base_location_(base_location)
    , sample_only_(sample_only)
    , curve_mode_(curve_mode)
{}
```

In `lex_token()` (line 202), add curve-mode handling right after `start_ = current_;` and the EOF check (before the `_` Elongate check at line 213):

```cpp
    // Curve mode: handle curve-specific characters
    if (curve_mode_) {
        switch (c) {
            case '_': advance(); return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{0.00f});
            case '.': advance(); return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{0.25f});
            case '-': advance(); return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{0.50f});
            case '^': advance(); return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{0.75f});
            case '\'': advance(); return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{1.00f});
            case '~': advance(); return make_token(MiniTokenType::CurveSmooth);
            case '\\': advance(); return make_token(MiniTokenType::CurveRamp);
            case '/':
                // Disambiguation: / + digit = Slash (slow modifier), / + non-digit = CurveRamp
                if (is_digit(peek_next())) {
                    advance();
                    return make_token(MiniTokenType::Slash);
                }
                advance();
                return make_token(MiniTokenType::CurveRamp);
            default:
                break; // Fall through to standard lexing for brackets, modifiers, etc.
        }
    }
```

Update the `lex_mini` free function (line 639-644):

```cpp
std::pair<std::vector<MiniToken>, std::vector<Diagnostic>>
lex_mini(std::string_view pattern, SourceLocation base_location, bool sample_only, bool curve_mode) {
    MiniLexer lexer(pattern, base_location, sample_only, curve_mode);
    auto tokens = lexer.lex_all();
    return {std::move(tokens), lexer.diagnostics()};
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[curve_lexer]"`
Expected: All curve lexer tests pass.

- [ ] **Step 7: Commit**

```bash
git add akkado/include/akkado/mini_token.hpp akkado/include/akkado/mini_lexer.hpp akkado/src/mini_lexer.cpp akkado/tests/test_mini_notation.cpp
git commit -m "Add curve notation token types and curve-mode lexing for timeline notation"
```

---

## Task 2: AST and Mini-Parser for Curve Atoms

Add curve atom kinds to the AST and parse curve tokens into AST nodes.

**Files:**
- Modify: `akkado/include/akkado/ast.hpp:174-180` (MiniAtomKind), `:192-205` (MiniAtomData)
- Modify: `akkado/include/akkado/mini_parser.hpp:69,80-85,107-111` (declarations, parse_mini signature)
- Modify: `akkado/src/mini_parser.cpp:161-209` (is_atom_start, parse_atom), `:519-534` (parse_mini)
- Test: `akkado/tests/test_mini_notation.cpp`

- [ ] **Step 1: Write failing tests for curve AST parsing**

Add to `akkado/tests/test_mini_notation.cpp`:

```cpp
// ============================================================================
// Curve Parser Tests [curve_parser]
// ============================================================================

TEST_CASE("Parse basic curve levels", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_ ' -", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(root != NULL_NODE);

    // Root is MiniPattern with 3 children
    CHECK(arena[root].type == NodeType::MiniPattern);
    REQUIRE(arena.child_count(root) == 3);

    // First child: CurveLevel 0.0
    NodeIndex c0 = arena[root].first_child;
    CHECK(arena[c0].type == NodeType::MiniAtom);
    CHECK(arena[c0].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c0].as_mini_atom().curve_value == 0.0f);
    CHECK(arena[c0].as_mini_atom().curve_smooth == false);

    // Second child: CurveLevel 1.0
    NodeIndex c1 = arena[c0].next_sibling;
    CHECK(arena[c1].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c1].as_mini_atom().curve_value == 1.0f);

    // Third child: CurveLevel 0.5
    NodeIndex c2 = arena[c1].next_sibling;
    CHECK(arena[c2].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c2].as_mini_atom().curve_value == 0.5f);
}

TEST_CASE("Parse curve ramps", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_/'", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(arena.child_count(root) == 3);

    NodeIndex c0 = arena[root].first_child;
    CHECK(arena[c0].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);

    NodeIndex c1 = arena[c0].next_sibling;
    CHECK(arena[c1].as_mini_atom().kind == Node::MiniAtomKind::CurveRamp);

    NodeIndex c2 = arena[c1].next_sibling;
    CHECK(arena[c2].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c2].as_mini_atom().curve_value == 1.0f);
}

TEST_CASE("Parse smooth modifier ~", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_~'", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(arena.child_count(root) == 2);

    NodeIndex c0 = arena[root].first_child;
    CHECK(arena[c0].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c0].as_mini_atom().curve_value == 0.0f);
    CHECK(arena[c0].as_mini_atom().curve_smooth == false);

    NodeIndex c1 = arena[c0].next_sibling;
    CHECK(arena[c1].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c1].as_mini_atom().curve_value == 1.0f);
    CHECK(arena[c1].as_mini_atom().curve_smooth == true);
}

TEST_CASE("Parse curve with grouping", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("[_'] __", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(arena.child_count(root) == 3); // group + 2 levels

    NodeIndex group = arena[root].first_child;
    CHECK(arena[group].type == NodeType::MiniGroup);
    REQUIRE(arena.child_count(group) == 2);
}

TEST_CASE("Parse curve with alternation", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("<_' '_>", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(arena.child_count(root) == 1);

    NodeIndex seq = arena[root].first_child;
    CHECK(arena[seq].type == NodeType::MiniSequence);
    REQUIRE(arena.child_count(seq) == 2);
}

TEST_CASE("Curve ~ before non-level is error", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("~/", arena, {}, false, true);
    CHECK(!diags.empty());
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[curve_parser]"`
Expected: Compilation fails — `CurveLevel`, `CurveRamp` not in `MiniAtomKind`.

- [ ] **Step 3: Add curve kinds to AST**

In `akkado/include/akkado/ast.hpp`, extend `MiniAtomKind` (line 174-180):

```cpp
    enum class MiniAtomKind : std::uint8_t {
        Pitch,      // Note pitch (MIDI note number)
        Sample,     // Sample name with optional variant
        Rest,       // Rest/silence (~)
        Elongate,   // _ - extend previous note's duration (Tidal-compatible)
        Chord,      // Chord symbol (Am, C7, Fmaj7, etc.)
        CurveLevel, // Curve value level (_, ., -, ^, ')
        CurveRamp,  // Curve ramp (/, \)
    };
```

Add `curve_value` and `curve_smooth` fields to `MiniAtomData` (line 192-205):

```cpp
    struct MiniAtomData {
        MiniAtomKind kind;
        std::uint8_t midi_note;     // For Pitch kind
        std::int8_t micro_offset = 0;  // Microtonal step offset
        float velocity = 1.0f;      // 0.0-1.0, from :vel suffix
        std::string sample_name;    // For Sample kind
        std::uint8_t sample_variant; // For Sample kind (e.g., bd:2)
        std::string sample_bank;    // For Sample kind - bank name (empty = default)
        // Chord data (for Chord kind)
        std::string chord_root;             // Root note: "A", "C#", "Bb"
        std::string chord_quality;          // Quality: "", "m", "7", "maj7", etc.
        std::uint8_t chord_root_midi;       // MIDI of root (octave 4)
        std::vector<std::int8_t> chord_intervals;  // Semitone intervals
        // Curve data (for CurveLevel kind)
        float curve_value = 0.0f;    // 0.0, 0.25, 0.5, 0.75, 1.0
        bool curve_smooth = false;   // true if preceded by ~ modifier
    };
```

- [ ] **Step 4: Add parse_mini curve_mode parameter**

In `akkado/include/akkado/mini_parser.hpp`, update the free function signature (line 109-111):

```cpp
std::pair<NodeIndex, std::vector<Diagnostic>>
parse_mini(std::string_view pattern, AstArena& arena,
           SourceLocation base_location = {}, bool sample_only = false,
           bool curve_mode = false);
```

- [ ] **Step 5: Implement curve atom parsing in mini_parser.cpp**

Update `is_atom_start()` (line 161-170) to include curve tokens:

```cpp
bool MiniParser::is_atom_start() const {
    MiniTokenType type = current().type;
    return type == MiniTokenType::PitchToken ||
           type == MiniTokenType::SampleToken ||
           type == MiniTokenType::Rest ||
           type == MiniTokenType::Elongate ||
           type == MiniTokenType::CurveLevel ||
           type == MiniTokenType::CurveRamp ||
           type == MiniTokenType::CurveSmooth ||
           type == MiniTokenType::LBracket ||
           type == MiniTokenType::LAngle ||
           type == MiniTokenType::LBrace;
}
```

Add curve handling to `parse_atom()` (after the Elongate case, before the group checks):

```cpp
    if (match(MiniTokenType::CurveSmooth)) {
        // ~ prefix: next token must be CurveLevel
        if (!match(MiniTokenType::CurveLevel)) {
            error("Expected level character (_, ., -, ^, ') after ~");
            return NULL_NODE;
        }
        const auto& level_data = previous().as_curve_level();
        NodeIndex node = make_node(NodeType::MiniAtom, previous());
        arena_[node].data = Node::MiniAtomData{
            .kind = Node::MiniAtomKind::CurveLevel,
            .midi_note = 0,
            .velocity = 1.0f,
            .sample_name = "",
            .sample_variant = 0,
            .chord_root = "",
            .chord_quality = "",
            .chord_root_midi = 0,
            .chord_intervals = {},
            .curve_value = level_data.value,
            .curve_smooth = true
        };
        return node;
    }

    if (match(MiniTokenType::CurveLevel)) {
        const auto& level_data = previous().as_curve_level();
        NodeIndex node = make_node(NodeType::MiniAtom, previous());
        arena_[node].data = Node::MiniAtomData{
            .kind = Node::MiniAtomKind::CurveLevel,
            .midi_note = 0,
            .velocity = 1.0f,
            .sample_name = "",
            .sample_variant = 0,
            .chord_root = "",
            .chord_quality = "",
            .chord_root_midi = 0,
            .chord_intervals = {},
            .curve_value = level_data.value,
            .curve_smooth = false
        };
        return node;
    }

    if (match(MiniTokenType::CurveRamp)) {
        NodeIndex node = make_node(NodeType::MiniAtom, previous());
        arena_[node].data = Node::MiniAtomData{
            .kind = Node::MiniAtomKind::CurveRamp,
            .midi_note = 0,
            .velocity = 1.0f,
            .sample_name = "",
            .sample_variant = 0,
            .chord_root = "",
            .chord_quality = "",
            .chord_root_midi = 0,
            .chord_intervals = {}
        };
        return node;
    }
```

Update `parse_mini` free function to pass `curve_mode` (line 519-534):

```cpp
std::pair<NodeIndex, std::vector<Diagnostic>>
parse_mini(std::string_view pattern, AstArena& arena, SourceLocation base_location,
           bool sample_only, bool curve_mode) {
    auto [tokens, lex_diags] = lex_mini(pattern, base_location, sample_only, curve_mode);

    MiniParser parser(std::move(tokens), arena, base_location);
    NodeIndex root = parser.parse();

    std::vector<Diagnostic> all_diags = std::move(lex_diags);
    const auto& parse_diags = parser.diagnostics();
    all_diags.insert(all_diags.end(), parse_diags.begin(), parse_diags.end());

    return {root, std::move(all_diags)};
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[curve_parser]"`
Expected: All curve parser tests pass.

- [ ] **Step 7: Run full test suite to verify no regressions**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests`
Expected: All existing tests pass (no regressions from new enum values or function signatures).

- [ ] **Step 8: Commit**

```bash
git add akkado/include/akkado/ast.hpp akkado/include/akkado/mini_parser.hpp akkado/src/mini_parser.cpp akkado/tests/test_mini_notation.cpp
git commit -m "Add curve atom kinds to AST and parse curve tokens in mini-parser"
```

---

## Task 3: Pattern Evaluation for Curve Events

Handle curve atoms in the pattern evaluator so they produce `PatternEvent` objects with curve data.

**Files:**
- Modify: `akkado/include/akkado/pattern_event.hpp:19-25` (PatternEventType), `:32-79` (PatternEvent)
- Modify: `akkado/src/pattern_eval.cpp:177-238` (eval_atom)
- Test: `akkado/tests/test_mini_notation.cpp`

- [ ] **Step 1: Write failing tests for curve event evaluation**

Add to `akkado/tests/test_mini_notation.cpp`:

```cpp
// ============================================================================
// Curve Evaluation Tests [curve_eval]
// ============================================================================

TEST_CASE("Evaluate constant curve", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("____", arena, {}, false, true);
    REQUIRE(diags.empty());

    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 4);

    for (const auto& event : stream.events) {
        CHECK(event.type == PatternEventType::CurveLevel);
        CHECK(event.curve_value == 0.0f);
        CHECK(event.curve_smooth == false);
    }

    // Each event gets 1/4 of the cycle
    CHECK_THAT(stream.events[0].time, WithinRel(0.0f, 0.01f));
    CHECK_THAT(stream.events[0].duration, WithinRel(0.25f, 0.01f));
    CHECK_THAT(stream.events[1].time, WithinRel(0.25f, 0.01f));
    CHECK_THAT(stream.events[2].time, WithinRel(0.5f, 0.01f));
    CHECK_THAT(stream.events[3].time, WithinRel(0.75f, 0.01f));
}

TEST_CASE("Evaluate step curve", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("__''", arena, {}, false, true);
    REQUIRE(diags.empty());

    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 4);

    CHECK(stream.events[0].curve_value == 0.0f);
    CHECK(stream.events[1].curve_value == 0.0f);
    CHECK(stream.events[2].curve_value == 1.0f);
    CHECK(stream.events[3].curve_value == 1.0f);
}

TEST_CASE("Evaluate curve with ramp", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_/'", arena, {}, false, true);
    REQUIRE(diags.empty());

    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 3);

    CHECK(stream.events[0].type == PatternEventType::CurveLevel);
    CHECK(stream.events[0].curve_value == 0.0f);

    CHECK(stream.events[1].type == PatternEventType::CurveRamp);

    CHECK(stream.events[2].type == PatternEventType::CurveLevel);
    CHECK(stream.events[2].curve_value == 1.0f);
}

TEST_CASE("Evaluate curve with smooth modifier", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_~'", arena, {}, false, true);
    REQUIRE(diags.empty());

    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 2);

    CHECK(stream.events[0].curve_smooth == false);
    CHECK(stream.events[1].curve_smooth == true);
    CHECK(stream.events[1].curve_value == 1.0f);
}

TEST_CASE("Evaluate curve with weight modifier", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_@3 '", arena, {}, false, true);
    REQUIRE(diags.empty());

    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 2);

    // _ gets 3/4 of the time, ' gets 1/4
    CHECK_THAT(stream.events[0].duration, WithinRel(0.75f, 0.01f));
    CHECK_THAT(stream.events[1].duration, WithinRel(0.25f, 0.01f));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[curve_eval]"`
Expected: Compilation fails — `CurveLevel`, `CurveRamp` not in `PatternEventType`.

- [ ] **Step 3: Add curve types to PatternEvent**

In `akkado/include/akkado/pattern_event.hpp`, extend `PatternEventType` (line 19-25):

```cpp
enum class PatternEventType : std::uint8_t {
    Pitch,      // Melodic note (MIDI note number)
    Sample,     // Sample trigger (sample name + variant)
    Rest,       // Silence (no output)
    Chord,      // Chord (multiple MIDI notes)
    Elongate,   // Internal marker: extends previous event's duration
    CurveLevel, // Curve value level (holds a value for its duration)
    CurveRamp,  // Curve ramp (interpolates between neighbors)
};
```

Add curve fields to `PatternEvent` struct (after `chord_data`, before `should_trigger`):

```cpp
    // Curve data (for CurveLevel/CurveRamp types)
    float curve_value = 0.0f;      // For CurveLevel: 0.0, 0.25, 0.5, 0.75, 1.0
    bool curve_smooth = false;     // For CurveLevel: true if ~ prefix (linear interp)
```

Add helper methods:

```cpp
    /// Check if this is a curve level event
    [[nodiscard]] bool is_curve_level() const {
        return type == PatternEventType::CurveLevel;
    }

    /// Check if this is a curve ramp event
    [[nodiscard]] bool is_curve_ramp() const {
        return type == PatternEventType::CurveRamp;
    }
```

- [ ] **Step 4: Handle curve atoms in eval_atom()**

In `akkado/src/pattern_eval.cpp`, add cases to `eval_atom()` (line 177-238), inside the switch on `atom_data.kind` (after the `Elongate` case):

```cpp
        case Node::MiniAtomKind::CurveLevel:
            event.type = PatternEventType::CurveLevel;
            event.curve_value = atom_data.curve_value;
            event.curve_smooth = atom_data.curve_smooth;
            break;
        case Node::MiniAtomKind::CurveRamp:
            event.type = PatternEventType::CurveRamp;
            break;
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[curve_eval]"`
Expected: All curve evaluation tests pass.

- [ ] **Step 6: Run full test suite for regressions**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests`
Expected: All tests pass.

- [ ] **Step 7: Commit**

```bash
git add akkado/include/akkado/pattern_event.hpp akkado/src/pattern_eval.cpp akkado/tests/test_mini_notation.cpp
git commit -m "Add curve event types to pattern evaluation"
```

---

## Task 4: Main Lexer t"..." Prefix and Parser Integration

Add the `t"..."` string prefix to the main Akkado lexer and route it through the parser with `curve_mode=true`.

**Files:**
- Modify: `akkado/include/akkado/token.hpp:13-89` (TokenType enum), `:92-148` (token_type_name)
- Modify: `akkado/src/lexer.cpp:421-427` (lex_identifier)
- Modify: `akkado/src/parser.cpp:455-456` (parse_prefix), `:1116-1177` (parse_mini_literal)
- Test: `akkado/tests/test_lexer.cpp`
- Test: `akkado/tests/test_mini_notation.cpp`

- [ ] **Step 1: Write failing tests for t"..." lexing**

Add to `akkado/tests/test_lexer.cpp`:

```cpp
TEST_CASE("Lexer timeline string prefix", "[lexer][timeline]") {
    SECTION("t followed by double quote is Timeline token") {
        auto [tokens, diags] = lex("t\"__/''\"");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() >= 3); // Timeline, String, Eof
        CHECK(tokens[0].type == TokenType::Timeline);
        CHECK(tokens[1].type == TokenType::String);
        CHECK(tokens[1].as_string() == "__/''");
    }

    SECTION("t followed by backtick is Timeline token") {
        auto [tokens, diags] = lex("t`__/''`");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == TokenType::Timeline);
        CHECK(tokens[1].type == TokenType::String);
    }

    SECTION("standalone t is Identifier") {
        auto [tokens, diags] = lex("t = 5");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == TokenType::Identifier);
    }

    SECTION("total is Identifier not Timeline") {
        auto [tokens, diags] = lex("total = 10");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "total");
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[lexer][timeline]"`
Expected: Compilation fails — `TokenType::Timeline` doesn't exist.

- [ ] **Step 3: Add Timeline to TokenType**

In `akkado/include/akkado/token.hpp`, add after `Pat` (line 35):

```cpp
    Pat,            // pat(...)
    Timeline,       // t"..." (timeline curve notation)
```

Add to `token_type_name()`:

```cpp
        case TokenType::Timeline:  return "Timeline";
```

- [ ] **Step 4: Add t"..." detection to lexer.cpp**

In `akkado/src/lexer.cpp`, in `lex_identifier()` (after the `p"..."` check at line 421-427):

```cpp
    // Check for timeline string prefix: t"..." or t`...`
    if (source_[start_] == 't' && current_ == start_ + 1) {
        char next = peek();
        if (next == '"' || next == '`') {
            return make_token(TokenType::Timeline);
        }
    }
```

- [ ] **Step 5: Run lexer tests to verify they pass**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[lexer][timeline]"`
Expected: All timeline lexer tests pass.

- [ ] **Step 6: Update parser to handle Timeline token**

In `akkado/src/parser.cpp`, in `parse_prefix()` (around line 455-456), add Timeline alongside Pat:

```cpp
        case TokenType::Pat:
        case TokenType::Timeline:
            return parse_mini_literal();
```

Also add Timeline to the synchronization switch in the error recovery (around line 89):

```cpp
            case TokenType::Pat:
            case TokenType::Timeline:
```

Modify `parse_mini_literal()` (line 1116-1177) to accept both Pat and Timeline, and pass curve_mode when it's a Timeline:

```cpp
NodeIndex Parser::parse_mini_literal() {
    Token kw_tok = advance();

    bool is_timeline = (kw_tok.type == TokenType::Timeline);

    if (kw_tok.type != TokenType::Pat && kw_tok.type != TokenType::Timeline) {
        error("Expected 'pat' or 't' keyword");
        return NULL_NODE;
    }

    NodeIndex node = make_node(NodeType::MiniLiteral, kw_tok);

    bool has_parens = false;
    if (!is_timeline && check(TokenType::LParen)) {
        has_parens = true;
        advance(); // consume '('
    }

    // First argument: the mini-notation string
    if (!check(TokenType::String)) {
        error("Expected string for mini-notation pattern");
        return node;
    }

    Token pattern_tok = advance();
    const std::string& pattern_str = pattern_tok.as_string();

    // Adjust location to point to content start (skip opening quote)
    SourceLocation content_loc = pattern_tok.location;
    content_loc.offset += 1;
    content_loc.column += 1;
    content_loc.length = pattern_str.length();

    // Parse the mini-notation string into AST nodes
    // Timeline (t"...") uses curve_mode=true
    auto [pattern_ast, mini_diags] = parse_mini(pattern_str, arena_, content_loc,
                                                 false, is_timeline);

    // Add mini-notation diagnostics to our diagnostics
    for (auto& diag : mini_diags) {
        diag.filename = filename_;
        diagnostics_.push_back(std::move(diag));
    }

    // Add the parsed pattern as a child
    if (pattern_ast != NULL_NODE) {
        arena_.add_child(node, pattern_ast);
    }

    if (has_parens) {
        // Optional second argument: closure (only in function-call form)
        if (match(TokenType::Comma)) {
            if (check(TokenType::LParen)) {
                advance();
                NodeIndex closure = parse_closure();
                arena_.add_child(node, closure);
            } else {
                error("Expected closure after comma in pattern");
            }
        }

        consume(TokenType::RParen, "Expected ')' after pattern arguments");
    }

    return node;
}
```

- [ ] **Step 7: Write parser integration test**

Add to `akkado/tests/test_mini_notation.cpp`:

```cpp
// ============================================================================
// Timeline Prefix Integration Tests [timeline_prefix]
// ============================================================================

TEST_CASE("t prefix parses curve AST", "[timeline_prefix]") {
    // This tests the full pipeline: main lexer -> parser -> mini-lexer(curve) -> mini-parser
    auto [tokens, lex_diags] = akkado::lex("t\"__''\"");
    REQUIRE(lex_diags.empty());
    REQUIRE(tokens.size() >= 3);
    CHECK(tokens[0].type == TokenType::Timeline);
    CHECK(tokens[1].type == TokenType::String);
}
```

- [ ] **Step 8: Run full test suite**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests`
Expected: All tests pass.

- [ ] **Step 9: Commit**

```bash
git add akkado/include/akkado/token.hpp akkado/src/lexer.cpp akkado/src/parser.cpp akkado/tests/test_lexer.cpp akkado/tests/test_mini_notation.cpp
git commit -m "Add t\"...\" timeline prefix to lexer and parser with curve-mode routing"
```

---

## Task 5: Events-to-Breakpoints Conversion and TIMELINE Codegen

Convert curve events into `TimelineState::Breakpoint` entries and emit the `TIMELINE` opcode with `StateInitData::Type::Timeline`.

**Files:**
- Modify: `akkado/src/codegen_patterns.cpp` — add `events_to_breakpoints()`, handle Timeline in pattern codegen
- Modify: `web/wasm/nkido_wasm.cpp` — handle `Timeline` type in `cedar_apply_state_inits()`
- Test: `akkado/tests/test_codegen.cpp`

- [ ] **Step 1: Write failing tests for breakpoint conversion**

Add to `akkado/tests/test_codegen.cpp`:

```cpp
#include "akkado/pattern_eval.hpp"

// ============================================================================
// Timeline Curve Breakpoint Tests [timeline_codegen]
// ============================================================================

// Helper: parse curve string and evaluate to event stream
static PatternEventStream eval_curve(const std::string& curve_str) {
    AstArena arena;
    auto [root, diags] = parse_mini(curve_str, arena, {}, false, true);
    return evaluate_pattern(root, arena, 0);
}

TEST_CASE("Constant curve produces single breakpoint", "[timeline_codegen]") {
    // t"___" → single breakpoint at value 0.0
    auto stream = eval_curve("___");
    auto breakpoints = events_to_breakpoints(stream.events);

    REQUIRE(breakpoints.size() == 1);
    CHECK_THAT(breakpoints[0].time, WithinRel(0.0f, 0.001f));
    CHECK_THAT(breakpoints[0].value, WithinRel(0.0f, 0.001f));
    CHECK(breakpoints[0].curve == 2); // hold
}

TEST_CASE("Step curve produces two breakpoints", "[timeline_codegen]") {
    // t"__''" → step from 0.0 to 1.0
    auto stream = eval_curve("__''");
    auto breakpoints = events_to_breakpoints(stream.events);

    REQUIRE(breakpoints.size() == 2);
    CHECK_THAT(breakpoints[0].value, WithinRel(0.0f, 0.001f));
    CHECK(breakpoints[0].curve == 2); // hold

    CHECK_THAT(breakpoints[1].time, WithinRel(0.5f, 0.01f));
    CHECK_THAT(breakpoints[1].value, WithinRel(1.0f, 0.001f));
    CHECK(breakpoints[1].curve == 2); // hold
}

TEST_CASE("Ramp produces linear breakpoints", "[timeline_codegen]") {
    // t"_/'" → ramp from 0.0 to 1.0
    auto stream = eval_curve("_/'");
    auto breakpoints = events_to_breakpoints(stream.events);

    // Should have: hold at 0.0, then linear to 1.0, then hold at 1.0
    REQUIRE(breakpoints.size() >= 2);

    // First breakpoint: start value
    CHECK_THAT(breakpoints[0].value, WithinRel(0.0f, 0.001f));

    // Ramp breakpoint should be linear (curve=0)
    bool has_linear = false;
    for (const auto& bp : breakpoints) {
        if (bp.curve == 0) has_linear = true;
    }
    CHECK(has_linear);
}

TEST_CASE("Smooth modifier produces linear breakpoint", "[timeline_codegen]") {
    // t"_~'" → smooth ramp from 0.0 to 1.0
    auto stream = eval_curve("_~'");
    auto breakpoints = events_to_breakpoints(stream.events);

    REQUIRE(breakpoints.size() >= 2);
    // The ~' breakpoint should be linear
    CHECK(breakpoints[1].curve == 0); // linear
    CHECK_THAT(breakpoints[1].value, WithinRel(1.0f, 0.001f));
}

TEST_CASE("Multiple ramps produce proportional interpolation", "[timeline_codegen]") {
    // t"_//'" → 2 ramps from 0.0 to 1.0
    auto stream = eval_curve("_//'");
    auto breakpoints = events_to_breakpoints(stream.events);

    // Should produce intermediate breakpoint at ~0.5
    bool has_mid = false;
    for (const auto& bp : breakpoints) {
        if (bp.value > 0.3f && bp.value < 0.7f && bp.curve == 0) {
            has_mid = true;
        }
    }
    CHECK(has_mid);
}

TEST_CASE("Ramp at start defaults to 0.0", "[timeline_codegen]") {
    // t"/'" → ramp from 0.0 (default) to 1.0
    auto stream = eval_curve("/'");
    auto breakpoints = events_to_breakpoints(stream.events);

    REQUIRE(!breakpoints.empty());
    // First breakpoint should ramp toward 1.0
    CHECK(breakpoints[0].curve == 0); // linear
}

TEST_CASE("All-same levels merge to single breakpoint", "[timeline_codegen]") {
    // t"'''''" → all 1.0, should merge to single breakpoint
    auto stream = eval_curve("'''''");
    auto breakpoints = events_to_breakpoints(stream.events);

    CHECK(breakpoints.size() == 1);
    CHECK_THAT(breakpoints[0].value, WithinRel(1.0f, 0.001f));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[timeline_codegen]"`
Expected: Compilation fails — `events_to_breakpoints` doesn't exist.

- [ ] **Step 3: Implement events_to_breakpoints()**

In `akkado/src/codegen_patterns.cpp`, add the conversion function (as a free function in the `akkado` namespace, or as a static helper). Also declare it in a header accessible by tests. The simplest approach: add a declaration in `akkado/include/akkado/pattern_eval.hpp` (since it consumes PatternEvents):

In `akkado/include/akkado/pattern_eval.hpp`, add after the existing forward declarations:

```cpp
#include <cedar/opcodes/dsp_state.hpp>

/// Convert curve pattern events to TIMELINE breakpoints
/// @param events Flat list of evaluated curve events (CurveLevel, CurveRamp)
/// @return Vector of breakpoints ready for TimelineState
std::vector<cedar::TimelineState::Breakpoint>
events_to_breakpoints(const std::vector<PatternEvent>& events);
```

In `akkado/src/pattern_eval.cpp`, add the implementation:

```cpp
#include <cedar/opcodes/dsp_state.hpp>

std::vector<cedar::TimelineState::Breakpoint>
events_to_breakpoints(const std::vector<PatternEvent>& events) {
    if (events.empty()) return {};

    // Find previous/next CurveLevel value for ramp resolution
    auto find_prev_level = [&](std::size_t idx) -> float {
        for (std::size_t i = idx; i > 0; --i) {
            if (events[i - 1].type == PatternEventType::CurveLevel) {
                return events[i - 1].curve_value;
            }
        }
        return 0.0f; // Default: ramp from 0.0
    };

    auto find_next_level = [&](std::size_t idx) -> float {
        for (std::size_t i = idx + 1; i < events.size(); ++i) {
            if (events[i].type == PatternEventType::CurveLevel) {
                return events[i].curve_value;
            }
        }
        return 0.0f; // Default: ramp to 0.0
    };

    // Count consecutive ramps in a run starting at idx
    auto count_ramp_run = [&](std::size_t idx) -> std::size_t {
        std::size_t count = 0;
        for (std::size_t i = idx; i < events.size(); ++i) {
            if (events[i].type == PatternEventType::CurveRamp) {
                count++;
            } else {
                break;
            }
        }
        return count;
    };

    std::vector<cedar::TimelineState::Breakpoint> breakpoints;

    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];

        if (event.type == PatternEventType::CurveLevel) {
            cedar::TimelineState::Breakpoint bp;
            bp.time = event.time;
            bp.value = event.curve_value;
            bp.curve = event.curve_smooth ? 0 : 2; // 0=linear, 2=hold
            breakpoints.push_back(bp);

        } else if (event.type == PatternEventType::CurveRamp) {
            float prev_val = find_prev_level(i);
            float next_val = find_next_level(i);
            std::size_t run_length = count_ramp_run(i);
            // Position within the ramp run (0-based)
            std::size_t pos_in_run = 0;
            for (std::size_t j = i; j > 0; --j) {
                if (events[j - 1].type == PatternEventType::CurveRamp) {
                    pos_in_run++;
                } else {
                    break;
                }
            }

            // Proportional interpolation
            float t = static_cast<float>(pos_in_run + 1) / static_cast<float>(run_length);
            float interp_value = prev_val + (next_val - prev_val) * t;

            cedar::TimelineState::Breakpoint bp;
            bp.time = event.time;
            bp.value = interp_value;
            bp.curve = 0; // linear
            breakpoints.push_back(bp);
        }
        // Skip other event types (Rest, etc.)
    }

    // Optimization: merge consecutive holds at the same value
    if (breakpoints.size() > 1) {
        std::vector<cedar::TimelineState::Breakpoint> optimized;
        optimized.push_back(breakpoints[0]);

        for (std::size_t i = 1; i < breakpoints.size(); ++i) {
            const auto& prev = optimized.back();
            const auto& curr = breakpoints[i];

            // Skip if same value and both hold
            if (curr.curve == 2 && prev.curve == 2 &&
                std::abs(curr.value - prev.value) < 0.001f) {
                continue;
            }
            optimized.push_back(curr);
        }
        breakpoints = std::move(optimized);
    }

    return breakpoints;
}
```

- [ ] **Step 4: Run breakpoint tests**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[timeline_codegen]"`
Expected: All breakpoint conversion tests pass.

- [ ] **Step 5: Commit breakpoint conversion**

```bash
git add akkado/include/akkado/pattern_eval.hpp akkado/src/pattern_eval.cpp akkado/tests/test_codegen.cpp
git commit -m "Add events_to_breakpoints() conversion for timeline curve notation"
```

- [ ] **Step 6: Add TIMELINE codegen in codegen_patterns.cpp**

The timeline codegen needs to:
1. Detect when a `MiniLiteral` from a `Timeline` token contains curve atoms
2. Evaluate the curve pattern to events
3. Convert events to breakpoints
4. Emit a TIMELINE opcode with a `StateInitData::Type::Timeline`

In `akkado/src/codegen_patterns.cpp`, the existing pattern codegen handles `MiniLiteral` nodes. We need to add a check: if the MiniLiteral came from a `t"..."` prefix, use the timeline codegen path instead of the SEQPAT path.

The key question is: how does the codegen know this MiniLiteral is a timeline vs a pat? The `MiniLiteral` node doesn't currently carry this info. We need to mark it.

**Option:** Store a flag on the `MiniLiteral` node. The simplest approach: use the existing `StringData` variant on the MiniLiteral node to store a marker string like `"timeline"`, or add a simple boolean. Since the MiniLiteral uses `std::monostate` for its data currently, let's use a `StringData` with value `"timeline"` to mark it.

In `akkado/src/parser.cpp`, in `parse_mini_literal()`, after creating the node, mark timeline:

```cpp
    // Mark timeline nodes so codegen can distinguish from pat
    if (is_timeline) {
        arena_[node].data = Node::StringData{.value = "timeline"};
    }
```

Then in `akkado/src/codegen_patterns.cpp`, in the pattern handling code (around the `visit_mini_literal` or equivalent), check if the MiniLiteral has timeline data and branch:

Find where `NodeType::MiniLiteral` is visited in the codegen. This is in the main `visit()` function. Add timeline handling there.

The exact integration point depends on how `MiniLiteral` is handled in codegen. Looking at the code, `MiniLiteral` goes through `visit_pattern_node()` which calls the sequence compiler. For timeline, we skip the sequence compiler and instead:
1. Evaluate the mini-pattern to events
2. Convert to breakpoints
3. Emit TIMELINE opcode

Add a method to the codegen (in `codegen_patterns.cpp`) that handles timeline MiniLiterals:

```cpp
TypedValue CodeGenerator::visit_timeline_literal(NodeIndex node) {
    const Node& n = ast_->arena[node];

    // Get the MiniPattern child
    NodeIndex pattern_child = n.first_child;
    if (pattern_child == NULL_NODE) {
        error(n.location, "Empty timeline curve");
        return TypedValue::void_val();
    }

    // Evaluate the curve pattern to events
    PatternEvaluator evaluator(ast_->arena);
    PatternEventStream stream = evaluator.evaluate(pattern_child, 0);

    // Convert events to breakpoints
    auto breakpoints = events_to_breakpoints(stream.events);
    if (breakpoints.empty()) {
        error(n.location, "Timeline curve produced no breakpoints");
        return TypedValue::void_val();
    }

    if (breakpoints.size() > cedar::TimelineState::MAX_BREAKPOINTS) {
        warning(n.location, "Timeline curve exceeds 64 breakpoints, truncating");
        breakpoints.resize(cedar::TimelineState::MAX_BREAKPOINTS);
    }

    // Allocate state and output buffer
    push_path("timeline");
    std::uint32_t state_id = current_state_id();
    std::uint16_t out_buf = buffers_.allocate();

    // Emit TIMELINE instruction
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::TIMELINE;
    inst.out_buffer = out_buf;
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = state_id;
    emit(inst);

    // Create StateInitData for timeline breakpoints
    StateInitData timeline_init;
    timeline_init.state_id = state_id;
    timeline_init.type = StateInitData::Type::Timeline;
    timeline_init.cycle_length = 4.0f; // 1 cycle = 4 beats

    // Store breakpoints - we need a way to transfer them
    // Use the timeline_breakpoints field (needs to be added to StateInitData)
    timeline_init.timeline_breakpoints = std::move(breakpoints);
    timeline_init.timeline_loop = true;
    timeline_init.timeline_loop_length = 4.0f * stream.cycle_span;

    state_inits_.push_back(std::move(timeline_init));

    pop_path();
    return TypedValue::make_signal(out_buf);
}
```

This requires adding fields to `StateInitData` in `akkado/include/akkado/codegen.hpp`:

```cpp
    // For Timeline: breakpoint data
    std::vector<cedar::TimelineState::Breakpoint> timeline_breakpoints;
    bool timeline_loop = false;
    float timeline_loop_length = 0.0f;
```

And in the visit function for MiniLiteral, check for the timeline marker:

```cpp
    case NodeType::MiniLiteral: {
        const auto* str_data = std::get_if<Node::StringData>(&n.data);
        if (str_data && str_data->value == "timeline") {
            return visit_timeline_literal(node);
        }
        return visit_pattern_node(node);
    }
```

- [ ] **Step 7: Handle Timeline type in cedar_apply_state_inits()**

In `web/wasm/nkido_wasm.cpp`, in `cedar_apply_state_inits()`, add Timeline handling after the SequenceProgram case:

```cpp
        } else if (init.type == akkado::StateInitData::Type::Timeline) {
            // Initialize timeline breakpoints directly
            auto& state = g_vm->get_or_create_state<cedar::TimelineState>(init.state_id);
            state.num_points = std::min(
                static_cast<std::uint32_t>(init.timeline_breakpoints.size()),
                static_cast<std::uint32_t>(cedar::TimelineState::MAX_BREAKPOINTS));
            for (std::uint32_t i = 0; i < state.num_points; ++i) {
                state.points[i] = init.timeline_breakpoints[i];
            }
            state.loop = init.timeline_loop;
            state.loop_length = init.timeline_loop_length;
            count++;
```

Note: `get_or_create_state` may need to be accessed differently depending on the VM API. Check the existing patterns in the WASM file for how state is accessed. If the VM uses `init_sequence_program_state()` for sequences, there may need to be a similar method for timelines. If the StatePool already supports `get_or_create<TimelineState>`, use that pattern.

- [ ] **Step 8: Also handle in CLI (tools/nkido-cli)**

Check `tools/nkido-cli/` for state_init handling and add Timeline support there too if present.

- [ ] **Step 9: Run full test suite**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests && ./build/cedar/tests/cedar_tests`
Expected: All tests pass.

- [ ] **Step 10: Commit**

```bash
git add akkado/src/codegen_patterns.cpp akkado/include/akkado/codegen.hpp akkado/src/parser.cpp web/wasm/nkido_wasm.cpp
git commit -m "Add TIMELINE codegen for curve notation with StateInitData breakpoint population"
```

---

## Task 6: End-to-End Integration Tests

Verify the full pipeline works: `t"..."` source code -> lexer -> parser -> mini-notation -> eval -> breakpoints -> TIMELINE opcode.

**Files:**
- Test: `akkado/tests/test_codegen.cpp`

- [ ] **Step 1: Write end-to-end compilation tests**

Add to `akkado/tests/test_codegen.cpp`:

```cpp
TEST_CASE("Timeline curve compiles to TIMELINE opcode", "[timeline_e2e]") {
    auto result = akkado::compile("t\"____\" |> out(%, %)");
    REQUIRE(result.diagnostics.empty());

    // Should contain at least one TIMELINE instruction
    auto instructions = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
    std::size_t inst_count = result.bytecode.size() / sizeof(cedar::Instruction);

    bool found_timeline = false;
    for (std::size_t i = 0; i < inst_count; ++i) {
        if (instructions[i].opcode == cedar::Opcode::TIMELINE) {
            found_timeline = true;
            break;
        }
    }
    CHECK(found_timeline);
}

TEST_CASE("Timeline curve produces state init", "[timeline_e2e]") {
    auto result = akkado::compile("t\"__''\" |> out(%, %)");
    REQUIRE(result.diagnostics.empty());

    // Should have a Timeline state init
    bool found_timeline_init = false;
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::Timeline) {
            found_timeline_init = true;
            CHECK(!init.timeline_breakpoints.empty());
            CHECK(init.timeline_loop == true);
            CHECK(init.timeline_loop_length > 0.0f);
        }
    }
    CHECK(found_timeline_init);
}

TEST_CASE("Timeline function call form compiles", "[timeline_e2e]") {
    auto result = akkado::compile("timeline(\"__/''\") |> out(%, %)");
    REQUIRE(result.diagnostics.empty());

    // Should also compile to TIMELINE opcode
    auto instructions = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
    std::size_t inst_count = result.bytecode.size() / sizeof(cedar::Instruction);

    bool found_timeline = false;
    for (std::size_t i = 0; i < inst_count; ++i) {
        if (instructions[i].opcode == cedar::Opcode::TIMELINE) {
            found_timeline = true;
        }
    }
    CHECK(found_timeline);
}

TEST_CASE("Timeline curve with math compiles", "[timeline_e2e]") {
    auto result = akkado::compile("t\"__/''\" * 1800 + 200 |> out(%, %)");
    REQUIRE(result.diagnostics.empty());
}
```

- [ ] **Step 2: Run end-to-end tests**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[timeline_e2e]"`
Expected: All pass. If any fail, debug the integration between parser -> codegen -> StateInitData.

- [ ] **Step 3: Run full test suite**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests && ./build/cedar/tests/cedar_tests`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add akkado/tests/test_codegen.cpp
git commit -m "Add end-to-end integration tests for timeline curve notation"
```

---

## Task 7: Handle timeline() Function Call Form

The `timeline("...")` builtin call form needs to route through the same curve-mode parsing and TIMELINE codegen as `t"..."`. Currently `timeline` is registered as a builtin in `builtins.hpp` and recognized in `is_pattern_call()`. We need to make the codegen detect `timeline(...)` calls and use the timeline codegen path.

**Files:**
- Modify: `akkado/src/codegen_patterns.cpp` — detect `timeline()` call and route to timeline codegen
- Test: Already covered in Task 6

- [ ] **Step 1: Update codegen to handle timeline() calls**

In `akkado/src/codegen_patterns.cpp`, in the code path that handles pattern calls (around the `find_pattern_node` helper or the `visit` dispatch for Call nodes), add detection for `timeline()` calls:

When the codegen encounters a `Call` to `"timeline"` with a string/MiniLiteral argument, it should:
1. Parse the string argument with `curve_mode=true` (if not already parsed by the parser)
2. Route to `visit_timeline_literal()`

The challenge: when `timeline("...")` is written as a function call, the parser creates a `Call` node, not a `MiniLiteral`. The string argument is a `StringLit`, not a parsed mini-pattern. So the codegen needs to parse the string at codegen time.

In the visit for Call nodes, when `func_name == "timeline"`:

```cpp
if (func_name == "timeline") {
    // Get the string argument
    NodeIndex first_arg = n.first_child;
    if (first_arg == NULL_NODE) {
        error(n.location, "timeline() requires a curve string argument");
        return TypedValue::void_val();
    }
    const Node& arg = ast_->arena[first_arg];
    NodeIndex actual_arg = first_arg;
    if (arg.type == NodeType::Argument) {
        actual_arg = arg.first_child;
    }
    if (actual_arg == NULL_NODE) {
        error(n.location, "timeline() requires a curve string argument");
        return TypedValue::void_val();
    }
    const Node& str_node = ast_->arena[actual_arg];
    if (str_node.type == NodeType::StringLit) {
        // Parse the string as curve notation
        const std::string& curve_str = str_node.as_string();
        AstArena temp_arena;
        auto [pattern_root, mini_diags] = parse_mini(curve_str, temp_arena, str_node.location, false, true);

        // Report any parse errors
        for (auto& diag : mini_diags) {
            diagnostics_.push_back(std::move(diag));
        }

        if (pattern_root == NULL_NODE) {
            error(str_node.location, "Failed to parse timeline curve");
            return TypedValue::void_val();
        }

        // Evaluate and convert to breakpoints
        PatternEvaluator evaluator(temp_arena);
        PatternEventStream stream = evaluator.evaluate(pattern_root, 0);
        auto breakpoints = events_to_breakpoints(stream.events);

        // Same codegen as visit_timeline_literal from here...
        // (Factor the shared code into a helper)
    }
}
```

The cleaner approach: factor the breakpoint-to-TIMELINE emission into a helper that both `visit_timeline_literal` and the `timeline()` call handler use.

- [ ] **Step 2: Run the end-to-end tests including timeline() form**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests "[timeline_e2e]"`
Expected: The `timeline("__/''")` test passes.

- [ ] **Step 3: Commit**

```bash
git add akkado/src/codegen_patterns.cpp
git commit -m "Route timeline() function call form through curve-mode codegen"
```

---

## Task 8: Serialization Support for Debug UI

The pattern debug panel needs to serialize curve AST nodes. Update `serialize_mini_ast_json()` to handle `CurveLevel` and `CurveRamp` atom kinds.

**Files:**
- Modify: The file containing `serialize_mini_ast_json()` (likely `akkado/src/codegen_patterns.cpp` or a nearby serialization file)

- [ ] **Step 1: Find and update serialize_mini_ast_json**

Search for `serialize_mini_ast_json` and add cases for `CurveLevel` and `CurveRamp` in the atom kind switch:

```cpp
case Node::MiniAtomKind::CurveLevel:
    json += "\"kind\":\"curve_level\",\"value\":";
    json += std::to_string(atom_data.curve_value);
    if (atom_data.curve_smooth) {
        json += ",\"smooth\":true";
    }
    break;
case Node::MiniAtomKind::CurveRamp:
    json += "\"kind\":\"curve_ramp\"";
    break;
```

- [ ] **Step 2: Build and verify no crashes**

Run: `cmake --build build && ./build/akkado/tests/akkado_tests`
Expected: All pass.

- [ ] **Step 3: Commit**

```bash
git add <files with serialize_mini_ast_json>
git commit -m "Add CurveLevel/CurveRamp serialization for debug UI"
```

---

## Notes for the Implementer

### Key Invariants
- **No Cedar changes required.** The `TIMELINE` opcode and `TimelineState` already exist and work. This is purely an Akkado compiler feature.
- **curve_mode is lexer-only context.** The parser and evaluator don't need to know about curve mode — they just handle the new atom kinds.
- **Breakpoint values are always 0.0-1.0.** Users scale with math: `t"__/''" * 1800 + 200`.
- **MAX_BREAKPOINTS = 64.** Warn at compile time if exceeded.

### Testing Commands
```bash
# Build
cmake --build build

# Run all akkado tests
./build/akkado/tests/akkado_tests

# Run only curve/timeline tests
./build/akkado/tests/akkado_tests "[curve_lexer]"
./build/akkado/tests/akkado_tests "[curve_parser]"
./build/akkado/tests/akkado_tests "[curve_eval]"
./build/akkado/tests/akkado_tests "[timeline_codegen]"
./build/akkado/tests/akkado_tests "[timeline_e2e]"
./build/akkado/tests/akkado_tests "[timeline_prefix]"
./build/akkado/tests/akkado_tests "[lexer][timeline]"
```

### Reference Files
- **PRD:** `docs/PRD-Timeline-Curve-Notation.md` — full specification with edge cases
- **TimelineState:** `cedar/include/cedar/opcodes/dsp_state.hpp:211-224`
- **op_timeline:** `cedar/include/cedar/opcodes/sequencing.hpp:248`
- **StateInitData:** `akkado/include/akkado/codegen.hpp:98-134`
- **Builtin entry:** `akkado/include/akkado/builtins.hpp:634-637`
