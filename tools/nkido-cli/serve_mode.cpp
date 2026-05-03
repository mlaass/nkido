#include "serve_mode.hpp"

#include "audio_engine.hpp"
#include "akkado/akkado.hpp"
#include "akkado/diagnostics.hpp"
#include "cedar/vm/vm.hpp"

#include <SDL2/SDL.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
// SDL window: live waveform + level meter
// ----------------------------------------------------------------------------

constexpr int    WINDOW_W       = 480;
constexpr int    WINDOW_H       = 200;
constexpr Uint32 SDL_USEREVENT_JSON_LINE = 1;  // user.code value

void render_frame(SDL_Renderer* ren, AudioEngine& engine, bool playing) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);

    // Clear to black.
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    // Pull recent samples (mono mix) from the lock-free ring buffer.
    constexpr std::size_t N = 256;
    float buf[N]{};
    engine.get_waveform(buf, N);

    // Peak for level meter.
    float peak = 0.0f;
    for (std::size_t i = 0; i < N; ++i) {
        float a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }

    // Waveform in the upper 2/3.
    const int wave_h = (h * 2) / 3;
    const int mid    = wave_h / 2;
    const float amp  = static_cast<float>(wave_h) * 0.45f;

    if (playing) SDL_SetRenderDrawColor(ren, 80, 220, 100, 255);
    else         SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);

    int prev_x = 0;
    int prev_y = mid;
    for (std::size_t i = 0; i < N; ++i) {
        int x = static_cast<int>((static_cast<float>(i) / (N - 1)) * (w - 1));
        int y = mid - static_cast<int>(buf[i] * amp);
        if (i > 0) SDL_RenderDrawLine(ren, prev_x, prev_y, x, y);
        prev_x = x;
        prev_y = y;
    }

    // Level meter in the lower 1/3. A bar showing peak amplitude.
    const int meter_y = wave_h + (h - wave_h) / 4;
    const int meter_h = (h - wave_h) / 2;
    const int bar_w   = static_cast<int>(peak * static_cast<float>(w));

    SDL_Rect bg{0, meter_y, w, meter_h};
    SDL_SetRenderDrawColor(ren, 30, 30, 30, 255);
    SDL_RenderFillRect(ren, &bg);

    if (bar_w > 0) {
        SDL_Rect bar{0, meter_y, bar_w, meter_h};
        if (peak > 0.01f) SDL_SetRenderDrawColor(ren, 80, 220, 100, 255);
        else              SDL_SetRenderDrawColor(ren, 60, 90, 60, 255);
        SDL_RenderFillRect(ren, &bar);
    }
}

// ----------------------------------------------------------------------------
// Command dispatch (factored out so the SDL event loop can call it)
// ----------------------------------------------------------------------------

struct ServeState {
    AudioEngine*              engine;
    AudioEngine::Config       audio_config;
    bool                      audio_initialized = false;
    bool                      playing           = false;
    bool                      should_quit       = false;
};

void handle_command_line(ServeState& s, const std::string& line) {
    if (line.empty()) return;

    JsonParser parser(line);
    auto obj_opt = parser.parse_object();
    if (!obj_opt) {
        emit_error("invalid JSON");
        return;
    }
    const auto& obj = *obj_opt;

    auto cmd_it = obj.find("cmd");
    if (cmd_it == obj.end() || cmd_it->second.kind != JsonValue::Kind::String) {
        emit_error("missing 'cmd'");
        return;
    }
    const std::string& cmd = cmd_it->second.str;

    if (cmd == "load") {
        auto src_it = obj.find("source");
        if (src_it == obj.end() || src_it->second.kind != JsonValue::Kind::String) {
            emit_error("'load' requires 'source' (string)");
            emit_compiled(false);
            return;
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
            return;
        }

        const std::size_t n = cr.bytecode.size() / sizeof(cedar::Instruction);
        std::vector<cedar::Instruction> instructions(n);
        std::memcpy(instructions.data(), cr.bytecode.data(), cr.bytecode.size());

        if (s.playing) {
            // Hot-swap: state is preserved via Cedar's crossfade path.
            auto load_result = s.engine->vm().load_program(instructions);
            if (load_result != cedar::VM::LoadResult::Success) {
                const char* reason = "load failed";
                switch (load_result) {
                    case cedar::VM::LoadResult::SlotBusy: reason = "VM busy (try again)"; break;
                    case cedar::VM::LoadResult::TooLarge: reason = "program too large"; break;
                    default: break;
                }
                emit_error(reason);
                emit_compiled(false);
                return;
            }
        } else {
            if (!s.audio_initialized) {
                if (!s.engine->init(s.audio_config)) {
                    emit_error("failed to initialize audio");
                    emit_compiled(false);
                    return;
                }
                s.audio_initialized = true;
            }
            if (!s.engine->vm().load_program_immediate(instructions)) {
                emit_error("failed to load program (invalid bytecode?)");
                emit_compiled(false);
                return;
            }
            if (!s.engine->start()) {
                emit_error("failed to start audio");
                emit_compiled(false);
                return;
            }
            s.playing = true;
        }
        emit_compiled(true);

    } else if (cmd == "stop") {
        if (s.playing) {
            // Tear down the SDL device fully (not just pause). A paused
            // pulseaudio/pipewire-pulse stream gets corked and won't
            // produce audio when unpaused later; the next load will
            // reopen the device cleanly.
            s.engine->stop();
            s.audio_initialized = false;
            s.playing = false;
        }
        emit_event("stopped");

    } else if (cmd == "set_param") {
        auto name_it = obj.find("name");
        auto value_it = obj.find("value");
        if (name_it == obj.end() || name_it->second.kind != JsonValue::Kind::String ||
            value_it == obj.end() || value_it->second.kind != JsonValue::Kind::Number) {
            emit_error("'set_param' requires 'name' (string) and 'value' (number)");
            return;
        }
        const float v = static_cast<float>(value_it->second.num);
        const bool ok = s.engine->vm().set_param(name_it->second.str.c_str(), v);
        emit_param_changed(name_it->second.str, v, ok);

    } else if (cmd == "quit") {
        s.should_quit = true;

    } else {
        emit_error("unknown cmd");
    }
}

// ----------------------------------------------------------------------------
// Public entry point
// ----------------------------------------------------------------------------

int run_serve_mode(const Options& opts) {
    // Heap-allocate: AudioEngine holds a cedar::VM by value, which is large
    // enough to overflow the default thread stack in some build configurations.
    auto engine = std::make_unique<AudioEngine>();
    ServeState state;
    state.engine       = engine.get();
    state.audio_config = AudioEngine::Config{opts.sample_rate, opts.buffer_size, 2};

    install_signal_handlers();

    // Open an SDL window with a live waveform + level meter so the user
    // gets visual confirmation that audio is being produced. Window failure
    // is non-fatal — we fall back to headless serve.
    SDL_Window*   win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
        win = SDL_CreateWindow("nkido serve",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            WINDOW_W, WINDOW_H,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (win) {
            ren = SDL_CreateRenderer(win, -1,
                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!ren) {
                std::fprintf(stderr, "warning: SDL_CreateRenderer failed: %s\n",
                             SDL_GetError());
                SDL_DestroyWindow(win);
                win = nullptr;
            }
        } else {
            std::fprintf(stderr, "warning: SDL_CreateWindow failed: %s\n",
                         SDL_GetError());
        }
    } else {
        std::fprintf(stderr, "warning: SDL_INIT_VIDEO failed: %s\n",
                     SDL_GetError());
    }

    emit_event("ready");

    // Stdin worker: blocks on getline, posts each line as an SDL_USEREVENT.
    // SDL_PushEvent is documented thread-safe; SDL_PollEvent / rendering is
    // not, so the main thread keeps those.
    std::atomic<bool> stop_stdin{false};
    std::thread stdin_thread([&stop_stdin]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (stop_stdin.load(std::memory_order_acquire)) return;
            auto* heap_line = new std::string(std::move(line));
            SDL_Event ev{};
            ev.type       = SDL_USEREVENT;
            ev.user.code  = static_cast<Sint32>(SDL_USEREVENT_JSON_LINE);
            ev.user.data1 = heap_line;
            if (SDL_PushEvent(&ev) <= 0) {
                // Queue full or SDL gone; drop line to avoid leaking
                // when we can't deliver.
                delete heap_line;
            }
        }
        // EOF — ask the main loop to wind down.
        SDL_Event quit_ev{};
        quit_ev.type = SDL_QUIT;
        SDL_PushEvent(&quit_ev);
    });

    // Main loop. Even with no window we still pump SDL so the worker's
    // SDL_PushEvent calls get drained (SDL events still flow through the
    // main queue when only AUDIO is initialized).
    bool running = true;
    while (running) {
        if (g_signal_received.load() || state.should_quit) break;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
                break;
            }
            if (ev.type == SDL_USEREVENT &&
                ev.user.code == static_cast<Sint32>(SDL_USEREVENT_JSON_LINE)) {
                std::unique_ptr<std::string> line(
                    static_cast<std::string*>(ev.user.data1));
                handle_command_line(state, *line);
                if (state.should_quit) {
                    running = false;
                    break;
                }
            }
        }
        if (!running) break;

        if (ren) {
            render_frame(ren, *state.engine, state.playing);
            SDL_RenderPresent(ren);
        } else {
            // No window — sleep a bit so we don't spin the CPU. The worker
            // is what wakes us via SDL_PushEvent.
            SDL_Delay(16);
        }
    }

    // Tear down stdin worker. std::cin is still blocked inside getline; the
    // simplest clean shutdown is to close stdin so getline returns. We can't
    // really do that portably from here, so detach: the process is exiting
    // anyway.
    stop_stdin.store(true, std::memory_order_release);
    stdin_thread.detach();

    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    if (state.playing) {
        state.engine->stop();
    }
    return EXIT_SUCCESS;
}

}  // namespace nkido
