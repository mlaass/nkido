#include "serve_mode.hpp"

#include "audio_engine.hpp"
#include "akkado/akkado.hpp"
#include "akkado/diagnostics.hpp"
#include "cedar/vm/vm.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nkido {

namespace {

// ----------------------------------------------------------------------------
// JSON output helpers
// ----------------------------------------------------------------------------

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        switch (u) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", u);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void emit(std::string_view line) {
    std::cout << line << '\n';
    std::cout.flush();
}

void emit_event(std::string_view name) {
    std::string out;
    out.reserve(32);
    out += R"({"event":")";
    out += name;
    out += R"("})";
    emit(out);
}

void emit_compiled(bool ok) {
    emit(ok
        ? std::string_view{R"({"event":"compiled","ok":true})"}
        : std::string_view{R"({"event":"compiled","ok":false})"});
}

void emit_diagnostic(const akkado::Diagnostic& diag) {
    // Reuse akkado::format_diagnostic_json for the body; splice in the event tag.
    std::string body = akkado::format_diagnostic_json(diag);
    if (body.size() < 2 || body.front() != '{') {
        return;  // malformed; skip
    }
    std::string out;
    out.reserve(body.size() + 32);
    out += R"({"event":"diagnostic",)";
    out.append(body, 1, body.size() - 1);  // drop leading '{'
    emit(out);
}

void emit_error(std::string_view msg) {
    std::string out;
    out.reserve(msg.size() + 32);
    out += R"({"event":"error","message":")";
    out += escape_json(msg);
    out += R"("})";
    emit(out);
}

void emit_param_changed(std::string_view name, float value, bool ok) {
    std::string out;
    out.reserve(96);
    out += R"({"event":"param_changed","name":")";
    out += escape_json(name);
    out += R"(","value":)";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value));
    out += buf;
    out += R"(,"ok":)";
    out += ok ? "true" : "false";
    out += "}";
    emit(out);
}

// ----------------------------------------------------------------------------
// Tiny JSON object parser
// ----------------------------------------------------------------------------
//
// Parses a single top-level JSON object whose values are strings, numbers,
// booleans, or null. Sufficient for the serve-mode wire protocol; not a
// general-purpose parser. Supports the standard escape sequences used by
// JSON.stringify (\", \\, \/, \b, \f, \n, \r, \t, \uXXXX).

struct JsonValue {
    enum class Kind { String, Number, Bool, Null };
    Kind kind = Kind::Null;
    std::string str;
    double num = 0.0;
    bool boolean = false;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : src_(input), pos_(0) {}

    std::optional<std::unordered_map<std::string, JsonValue>> parse_object() {
        skip_ws();
        if (!consume('{')) return std::nullopt;
        std::unordered_map<std::string, JsonValue> out;
        skip_ws();
        if (peek() == '}') { ++pos_; return out; }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return std::nullopt;
            skip_ws();
            if (!consume(':')) return std::nullopt;
            skip_ws();
            JsonValue v;
            if (!parse_value(v)) return std::nullopt;
            out[std::move(key)] = std::move(v);
            skip_ws();
            char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; break; }
            return std::nullopt;
        }
        return out;
    }

private:
    char peek() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
    bool consume(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    // Encode a Unicode code point as UTF-8 into out. Caller validates range.
    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= src_.size()) return false;
                char e = src_[pos_++];
                switch (e) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) return false;
                        std::uint32_t cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                            else return false;
                        }
                        // Surrogate pair handling: if high surrogate and the
                        // following bytes are \uXXXX of low surrogate, decode
                        // to a single code point.
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            pos_ + 6 <= src_.size() &&
                            src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            std::uint32_t low = 0;
                            std::size_t save = pos_;
                            pos_ += 2;
                            for (int i = 0; i < 4; ++i) {
                                char h = src_[pos_++];
                                low <<= 4;
                                if (h >= '0' && h <= '9') low |= static_cast<std::uint32_t>(h - '0');
                                else if (h >= 'a' && h <= 'f') low |= static_cast<std::uint32_t>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') low |= static_cast<std::uint32_t>(h - 'A' + 10);
                                else { pos_ = save; low = 0; break; }
                            }
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                pos_ = save;
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;  // unterminated
    }

    bool parse_value(JsonValue& v) {
        char c = peek();
        if (c == '"') {
            v.kind = JsonValue::Kind::String;
            return parse_string(v.str);
        }
        if (c == 't' || c == 'f') {
            std::string_view rest = src_.substr(pos_);
            if (rest.substr(0, 4) == "true") {
                pos_ += 4;
                v.kind = JsonValue::Kind::Bool;
                v.boolean = true;
                return true;
            }
            if (rest.substr(0, 5) == "false") {
                pos_ += 5;
                v.kind = JsonValue::Kind::Bool;
                v.boolean = false;
                return true;
            }
            return false;
        }
        if (c == 'n') {
            if (src_.substr(pos_, 4) == "null") {
                pos_ += 4;
                v.kind = JsonValue::Kind::Null;
                return true;
            }
            return false;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::size_t start = pos_;
            if (c == '-') ++pos_;
            while (pos_ < src_.size()) {
                char d = src_[pos_];
                if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' ||
                    d == '+' || d == '-') ++pos_;
                else break;
            }
            try {
                v.num = std::stod(std::string(src_.substr(start, pos_ - start)));
            } catch (...) {
                return false;
            }
            v.kind = JsonValue::Kind::Number;
            return true;
        }
        return false;
    }

    std::string_view src_;
    std::size_t pos_;
};

}  // namespace

// ----------------------------------------------------------------------------
// Public entry point
// ----------------------------------------------------------------------------

int run_serve_mode(const Options& opts) {
    // Heap-allocate: AudioEngine holds a cedar::VM by value, which is large
    // enough to overflow the default thread stack in some build configurations.
    auto engine = std::make_unique<AudioEngine>();
    AudioEngine::Config audio_config{
        opts.sample_rate,
        opts.buffer_size,
        2,
    };

    install_signal_handlers();

    // Defer engine->init() (which calls SDL_OpenAudioDevice) until the first
    // `load` arrives. Opening the device early and leaving it paused for the
    // many seconds it takes the user to evaluate causes PulseAudio /
    // pipewire-pulse to cork the stream; when start() finally unpauses, the
    // audio thread runs but no samples reach the sink. play mode never trips
    // this because it opens, loads, and unpauses back-to-back.
    bool audio_initialized = false;
    bool playing = false;
    emit_event("ready");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (g_signal_received.load()) break;
        if (line.empty()) continue;

        JsonParser parser(line);
        auto obj_opt = parser.parse_object();
        if (!obj_opt) {
            emit_error("invalid JSON");
            continue;
        }
        const auto& obj = *obj_opt;

        auto cmd_it = obj.find("cmd");
        if (cmd_it == obj.end() || cmd_it->second.kind != JsonValue::Kind::String) {
            emit_error("missing 'cmd'");
            continue;
        }
        const std::string& cmd = cmd_it->second.str;

        if (cmd == "load") {
            auto src_it = obj.find("source");
            if (src_it == obj.end() || src_it->second.kind != JsonValue::Kind::String) {
                emit_error("'load' requires 'source' (string)");
                emit_compiled(false);
                continue;
            }
            std::string filename = "<vscode>";
            if (auto uri_it = obj.find("uri");
                uri_it != obj.end() && uri_it->second.kind == JsonValue::Kind::String) {
                filename = uri_it->second.str;
            }

            auto cr = akkado::compile(src_it->second.str, filename);
            if (!cr.success) {
                for (const auto& diag : cr.diagnostics) {
                    emit_diagnostic(diag);
                }
                emit_compiled(false);
                continue;
            }

            const std::size_t n = cr.bytecode.size() / sizeof(cedar::Instruction);
            std::vector<cedar::Instruction> instructions(n);
            std::memcpy(instructions.data(), cr.bytecode.data(), cr.bytecode.size());

            if (playing) {
                // Hot-swap: state is preserved via Cedar's crossfade path.
                auto load_result = engine->vm().load_program(instructions);
                if (load_result != cedar::VM::LoadResult::Success) {
                    const char* reason = "load failed";
                    switch (load_result) {
                        case cedar::VM::LoadResult::SlotBusy: reason = "VM busy (try again)"; break;
                        case cedar::VM::LoadResult::TooLarge: reason = "program too large"; break;
                        default: break;
                    }
                    emit_error(reason);
                    emit_compiled(false);
                    continue;
                }
            } else {
                if (!audio_initialized) {
                    if (!engine->init(audio_config)) {
                        emit_error("failed to initialize audio");
                        emit_compiled(false);
                        continue;
                    }
                    audio_initialized = true;
                }
                if (!engine->vm().load_program_immediate(instructions)) {
                    emit_error("failed to load program (invalid bytecode?)");
                    emit_compiled(false);
                    continue;
                }
                if (!engine->start()) {
                    emit_error("failed to start audio");
                    emit_compiled(false);
                    continue;
                }
                playing = true;
            }
            emit_compiled(true);

        } else if (cmd == "stop") {
            if (playing) {
                engine->pause();  // pause not stop, so state survives next load
                playing = false;
            }
            emit_event("stopped");

        } else if (cmd == "set_param") {
            auto name_it = obj.find("name");
            auto value_it = obj.find("value");
            if (name_it == obj.end() || name_it->second.kind != JsonValue::Kind::String ||
                value_it == obj.end() || value_it->second.kind != JsonValue::Kind::Number) {
                emit_error("'set_param' requires 'name' (string) and 'value' (number)");
                continue;
            }
            const float v = static_cast<float>(value_it->second.num);
            const bool ok = engine->vm().set_param(name_it->second.str.c_str(), v);
            emit_param_changed(name_it->second.str, v, ok);

        } else if (cmd == "quit") {
            break;

        } else {
            emit_error("unknown cmd");
        }
    }

    if (playing) {
        engine->stop();
    }
    return EXIT_SUCCESS;
}

}  // namespace nkido
