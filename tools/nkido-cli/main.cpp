#include "asset_loader.hpp"
#include "bytecode_loader.hpp"
#include "bytecode_dump.hpp"
#include "audio_engine.hpp"
#include "serve_mode.hpp"
#include "ui/ui_mode.hpp"
#include "akkado/akkado.hpp"
#include "cedar/vm/vm.hpp"
#include "cedar/opcodes/dsp_state.hpp"
#include "cedar/io/file_cache.hpp"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace {

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [mode] [options] [input]\n\n"
              << "Modes:\n"
              << "  play      Compile (if needed) and play audio (default)\n"
              << "  dump      Display bytecode in human-readable format\n"
              << "  compile   Compile source to bytecode file\n"
              << "  check     Syntax check only\n"
              << "  ui        Interactive editor mode\n"
              << "  render    Compile + render to WAV (offline)\n"
              << "  serve     Headless mode: read JSON commands from stdin\n\n"
              << "Input:\n"
              << "  <file.akkado>   Akkado source file\n"
              << "  <file.cedar>    Cedar bytecode file\n"
              << "  --source <code> Inline source string\n"
              << "  -               Read from stdin\n\n"
              << "Options:\n"
              << "  -r, --rate <hz>    Sample rate (default: 48000)\n"
              << "  -b, --buffer <n>   Buffer size (default: 128)\n"
              << "  --dump-bytecode    Show bytecode before playing\n"
              << "  --json             JSON output for errors/dump\n"
              << "  -v, --verbose      Show compilation stats\n"
              << "  -o, --output <f>   Output file (for compile/render mode)\n"
              << "  --seconds <f>      Render duration in seconds (default: 4)\n"
              << "  --bpm <f>          BPM for render mode (default: 120)\n"
              << "  --trace-poly <f>   Write per-block poly voice state to JSONL\n"
              << "  --list-devices     List audio capture devices and exit\n"
              << "  --input-device <n> Capture device name for in() (default: system default)\n"
              << "  --bank <uri>       Sample-bank manifest URI (strudel.json). May repeat.\n"
              << "                     Schemes: file://, http(s)://, github:user/repo, bundled://...\n"
              << "                     Bare paths are treated as file://.\n"
              << "  --soundfont <uri>  SoundFont (SF2) URI. May repeat.\n"
              << "  --sample <uri>     Single-sample URI ('name=uri' or just URI). May repeat.\n"
              << "  -h, --help         Show this help\n\n"
              << "Examples:\n"
              << "  " << program << " play song.akkado\n"
              << "  " << program << " --source \"sine(440) |> out(%,%)\" play\n"
              << "  cat song.akkado | " << program << " play -\n"
              << "  " << program << " dump song.cedar\n"
              << "  " << program << " compile -o out.cedar song.akkado\n"
              << "  " << program << " ui\n";
}

std::optional<nkido::Options> parse_args(int argc, char* argv[]) {
    nkido::Options opts;
    bool has_mode = false;
    bool has_input = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // Modes
        if (arg == "play" && !has_mode) {
            opts.mode = nkido::Mode::Play;
            has_mode = true;
        } else if (arg == "dump" && !has_mode) {
            opts.mode = nkido::Mode::Dump;
            has_mode = true;
        } else if (arg == "compile" && !has_mode) {
            opts.mode = nkido::Mode::Compile;
            has_mode = true;
        } else if (arg == "check" && !has_mode) {
            opts.mode = nkido::Mode::Check;
            has_mode = true;
        } else if (arg == "ui" && !has_mode) {
            opts.mode = nkido::Mode::UI;
            has_mode = true;
        } else if (arg == "render" && !has_mode) {
            opts.mode = nkido::Mode::Render;
            has_mode = true;
        } else if (arg == "serve" && !has_mode) {
            opts.mode = nkido::Mode::Serve;
            has_mode = true;
        }
        // Options
        else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else if (arg == "-r" || arg == "--rate") {
            if (++i >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return std::nullopt;
            }
            opts.sample_rate = static_cast<std::uint32_t>(std::stoul(argv[i]));
        } else if (arg == "-b" || arg == "--buffer") {
            if (++i >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return std::nullopt;
            }
            opts.buffer_size = static_cast<std::uint32_t>(std::stoul(argv[i]));
        } else if (arg == "--source") {
            if (++i >= argc) {
                std::cerr << "error: --source requires a value\n";
                return std::nullopt;
            }
            opts.input = argv[i];
            opts.input_type = nkido::InputType::InlineSource;
            has_input = true;
        } else if (arg == "-o" || arg == "--output") {
            if (++i >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return std::nullopt;
            }
            opts.output_file = argv[i];
        } else if (arg == "--seconds") {
            if (++i >= argc) {
                std::cerr << "error: --seconds requires a value\n";
                return std::nullopt;
            }
            opts.render_seconds = std::stof(argv[i]);
        } else if (arg == "--bpm") {
            if (++i >= argc) {
                std::cerr << "error: --bpm requires a value\n";
                return std::nullopt;
            }
            opts.render_bpm = std::stof(argv[i]);
        } else if (arg == "--trace-poly") {
            if (++i >= argc) {
                std::cerr << "error: --trace-poly requires a value\n";
                return std::nullopt;
            }
            opts.trace_poly_file = argv[i];
        } else if (arg == "--list-devices") {
            opts.list_devices = true;
        } else if (arg == "--input-device") {
            if (++i >= argc) {
                std::cerr << "error: --input-device requires a value\n";
                return std::nullopt;
            }
            opts.input_device = argv[i];
        } else if (arg == "--bank") {
            if (++i >= argc) {
                std::cerr << "error: --bank requires a URI\n";
                return std::nullopt;
            }
            opts.bank_uris.push_back(argv[i]);
        } else if (arg == "--soundfont") {
            if (++i >= argc) {
                std::cerr << "error: --soundfont requires a URI\n";
                return std::nullopt;
            }
            opts.soundfont_uris.push_back(argv[i]);
        } else if (arg == "--sample") {
            if (++i >= argc) {
                std::cerr << "error: --sample requires a URI (or 'name=uri')\n";
                return std::nullopt;
            }
            opts.sample_uris.push_back(argv[i]);
        } else if (arg == "--dump-bytecode") {
            opts.dump_bytecode = true;
        } else if (arg == "--json") {
            opts.json_output = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "-") {
            opts.input = "-";
            opts.input_type = nkido::InputType::Stdin;
            has_input = true;
        } else if (arg[0] != '-' && !has_input) {
            opts.input = arg;
            opts.input_type = nkido::detect_input_type(arg);
            has_input = true;
        } else {
            std::cerr << "error: unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }

    // Validate (UI/Serve modes and --list-devices don't need input)
    if (!has_input && opts.input_type != nkido::InputType::InlineSource &&
        opts.mode != nkido::Mode::UI && opts.mode != nkido::Mode::Serve &&
        !opts.list_devices) {
        std::cerr << "error: no input specified\n";
        print_usage(argv[0]);
        return std::nullopt;
    }

    // Compile mode requires output file
    if (opts.mode == nkido::Mode::Compile && !opts.output_file) {
        // Default output filename
        if (opts.input_type == nkido::InputType::SourceFile) {
            auto pos = opts.input.rfind('.');
            if (pos != std::string::npos) {
                opts.output_file = opts.input.substr(0, pos) + ".cedar";
            } else {
                opts.output_file = opts.input + ".cedar";
            }
        } else {
            opts.output_file = "out.cedar";
        }
    }

    return opts;
}

int handle_check_mode(const nkido::Options& opts) {
    // For check mode, we only need to compile and report errors
    std::string source;
    std::string filename;

    if (opts.input_type == nkido::InputType::InlineSource) {
        source = opts.input;
        filename = "<inline>";
    } else if (opts.input_type == nkido::InputType::Stdin) {
        std::string line;
        while (std::getline(std::cin, line)) {
            source += line + "\n";
        }
        filename = "<stdin>";
    } else if (opts.input_type == nkido::InputType::SourceFile) {
        std::ifstream file(opts.input);
        if (!file) {
            std::cerr << "error: cannot open file: " << opts.input << "\n";
            return EXIT_FAILURE;
        }
        source = std::string((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
        filename = opts.input;
    } else {
        std::cerr << "error: check mode requires source input\n";
        return EXIT_FAILURE;
    }

    auto result = akkado::compile(source, filename);

    if (result.success) {
        if (opts.verbose) {
            std::cerr << "OK: " << result.bytecode.size() / 16 << " instructions\n";
        }
        return EXIT_SUCCESS;
    }

    // Output errors
    for (const auto& diag : result.diagnostics) {
        if (opts.json_output) {
            std::cerr << akkado::format_diagnostic_json(diag) << "\n";
        } else {
            std::cerr << akkado::format_diagnostic(diag, source) << "\n";
        }
    }

    return EXIT_FAILURE;
}

// ----------------------------------------------------------------------------
// Render mode: compile + apply state inits + run VM offline + write WAV
// ----------------------------------------------------------------------------

// Minimal WAV writer (16-bit PCM stereo)
bool write_wav_16(const std::string& path, const std::vector<float>& interleaved,
                  std::uint32_t sample_rate, std::uint16_t channels) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    auto write_u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto write_u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    auto write_str = [&](const char* s) { out.write(s, 4); };

    const std::uint32_t num_samples = static_cast<std::uint32_t>(interleaved.size());
    const std::uint32_t data_size = num_samples * 2;  // 16-bit = 2 bytes per sample
    const std::uint32_t fmt_size = 16;
    const std::uint32_t riff_size = 36 + data_size;
    const std::uint32_t byte_rate = sample_rate * channels * 2;
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * 2);

    write_str("RIFF"); write_u32(riff_size); write_str("WAVE");
    write_str("fmt "); write_u32(fmt_size);
    write_u16(1);                   // PCM format
    write_u16(channels);
    write_u32(sample_rate);
    write_u32(byte_rate);
    write_u16(block_align);
    write_u16(16);                  // bits per sample
    write_str("data"); write_u32(data_size);

    for (float f : interleaved) {
        // Clamp and convert to int16
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        std::int16_t s = static_cast<std::int16_t>(f * 32767.0f);
        out.write(reinterpret_cast<const char*>(&s), 2);
    }

    return out.good();
}

// Apply state_inits from a CompileResult to the VM (mirrors web/wasm)
void apply_state_inits(cedar::VM& vm, const akkado::CompileResult& result,
                       std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    seq_storage.reserve(result.state_inits.size());
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            std::vector<cedar::Sequence> seq_copy = init.sequences;
            for (std::size_t i = 0; i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
                if (!init.sequence_events[i].empty()) {
                    seq_copy[i].events = const_cast<cedar::Event*>(init.sequence_events[i].data());
                    seq_copy[i].num_events = static_cast<std::uint32_t>(init.sequence_events[i].size());
                    seq_copy[i].capacity = static_cast<std::uint32_t>(init.sequence_events[i].size());
                }
            }
            seq_storage.push_back(std::move(seq_copy));
            auto& stored = seq_storage.back();
            vm.init_sequence_program_state(
                init.state_id, stored.data(), stored.size(),
                init.cycle_length, init.is_sample_pattern, init.total_events);
            if (init.iter_n > 0) {
                vm.init_sequence_iter_state(init.state_id, init.iter_n, init.iter_dir);
            }
        } else if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            vm.init_poly_state(init.state_id, init.poly_seq_state_id,
                               init.poly_max_voices, init.poly_mode,
                               init.poly_steal_strategy);
        } else if (init.type == akkado::StateInitData::Type::Timeline) {
            auto& state = vm.states().get_or_create<cedar::TimelineState>(init.state_id);
            state.num_points = std::min(
                static_cast<std::uint32_t>(init.timeline_breakpoints.size()),
                static_cast<std::uint32_t>(cedar::TimelineState::MAX_BREAKPOINTS));
            for (std::uint32_t i = 0; i < state.num_points; ++i) {
                state.points[i] = init.timeline_breakpoints[i];
            }
            state.loop = init.timeline_loop;
            state.loop_length = init.timeline_loop_length;
        }
    }
}

// Derive a bank name from a manifest URI. Mirrors the web
// `BankRegistry.extractBankName` heuristic: the last URL/path segment with
// any trailing `.json` / `.strudel.json` removed, or — if that segment is
// "strudel" — the parent segment (so `…/Dirt-Samples/strudel.json` →
// `Dirt-Samples`). For bare `github:user/repo` URIs the last `/`-separated
// segment is the repo name.
std::string derive_bank_name(const std::string& uri) {
    std::string last;
    std::string parent;
    std::size_t pos = 0;
    while (pos < uri.size()) {
        auto slash = uri.find('/', pos);
        if (slash == std::string::npos) {
            parent = last;
            last = uri.substr(pos);
            break;
        }
        if (slash > pos) {
            parent = last;
            last = uri.substr(pos, slash - pos);
        }
        pos = slash + 1;
    }
    auto strip_ext = [](std::string& s, const std::string& ext) {
        if (s.size() > ext.size() &&
            s.compare(s.size() - ext.size(), ext.size(), ext) == 0) {
            s.resize(s.size() - ext.size());
        }
    };
    strip_ext(last, ".strudel.json");
    strip_ext(last, ".json");
    if (last == "strudel" && !parent.empty()) return parent;
    if (last.empty()) return "unnamed";
    return last;
}

// Resolve and register every asset declared via CLI flags (`--bank`,
// `--soundfont`, `--sample`) and via top-level akkado directives
// (`samples()`'s required_uris, `required_samples_extended`). Returns
// false if any *required* fetch failed in a way that should abort the
// run; warnings are printed to stderr but not propagated.
bool load_program_assets(cedar::VM& vm,
                          const nkido::Options& opts,
                          const akkado::CompileResult& cr) {
    // Build the bank list. Order matters: --bank flags first, then
    // samples() declarations in source order. Each bank is registered
    // both into `default_banks` (searched first-hit-wins for unqualified
    // RequiredSample lookups) and `named_banks` (keyed by derived bank
    // name for `.bank("Name")` qualified lookups).
    std::vector<nkido::BankManifest> default_banks;
    std::unordered_map<std::string, nkido::BankManifest> named_banks;

    auto load_bank = [&](const std::string& uri) -> bool {
        try {
            auto m = nkido::fetch_bank_manifest(uri);
            std::string name = derive_bank_name(uri);
            std::cerr << "Loaded bank manifest '" << name << "' from " << uri
                      << " (" << m.samples.size() << " samples)\n";
            default_banks.push_back(m);
            named_banks[std::move(name)] = std::move(m);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "error: bank '" << uri << "' failed to load: " << e.what() << "\n";
            return false;
        }
    };

    for (const auto& uri : opts.bank_uris) {
        if (!load_bank(uri)) return false;
    }
    for (const auto& req : cr.required_uris) {
        if (req.kind == akkado::UriKind::SampleBank) {
            if (!load_bank(req.uri)) return false;
        }
    }

    // SoundFonts (one fetch each, registered under their URI tail as
    // display name).
    for (const auto& uri : opts.soundfont_uris) {
        std::string display = uri;
        auto last = uri.find_last_of('/');
        if (last != std::string::npos) display = uri.substr(last + 1);
        const int sf_id = nkido::load_soundfont_uri(vm, uri, display);
        if (sf_id < 0) {
            std::cerr << "error: SoundFont '" << uri << "' failed to load\n";
            return false;
        }
    }

    // Single-sample URIs (`--sample name=uri` or `--sample uri`; uri
    // tail used as the registry name when no explicit name).
    for (const auto& spec : opts.sample_uris) {
        std::string name, uri;
        auto eq = spec.find('=');
        if (eq != std::string::npos) {
            name = spec.substr(0, eq);
            uri  = spec.substr(eq + 1);
        } else {
            uri = spec;
            auto last = uri.find_last_of('/');
            std::string filename = last == std::string::npos ? uri : uri.substr(last + 1);
            auto dot = filename.find_last_of('.');
            name = dot == std::string::npos ? filename : filename.substr(0, dot);
        }
        if (nkido::load_sample_uri(vm, uri, name) == 0) {
            std::cerr << "error: sample '" << uri << "' failed to load\n";
            return false;
        }
    }

    // Resolve every RequiredSample referenced by the program against the
    // loaded bank list. Default-bank events search `default_banks`
    // first-hit-wins; events qualified with `.bank("Name")` look up
    // `named_banks` by the derived bank name.
    if (!cr.required_samples_extended.empty()) {
        nkido::register_required_samples(
            vm, cr.required_samples_extended, default_banks, named_banks);
    }

    return true;
}

int handle_render_mode(const nkido::Options& opts) {
    // Read source
    std::string source, filename;
    if (opts.input_type == nkido::InputType::InlineSource) {
        source = opts.input;
        filename = "<inline>";
    } else if (opts.input_type == nkido::InputType::Stdin) {
        std::string line;
        while (std::getline(std::cin, line)) source += line + "\n";
        filename = "<stdin>";
    } else if (opts.input_type == nkido::InputType::SourceFile) {
        std::ifstream file(opts.input);
        if (!file) {
            std::cerr << "error: cannot open file: " << opts.input << "\n";
            return EXIT_FAILURE;
        }
        source.assign((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
        filename = opts.input;
    } else {
        std::cerr << "error: render mode needs source input (.akkado / inline / stdin)\n";
        return EXIT_FAILURE;
    }

    // Compile
    auto cr = akkado::compile(source, filename);
    if (!cr.success) {
        for (const auto& diag : cr.diagnostics) {
            std::cerr << akkado::format_diagnostic(diag, source) << "\n";
        }
        return EXIT_FAILURE;
    }

    // Build instruction vector
    std::vector<cedar::Instruction> instructions;
    std::size_t num_instructions = cr.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(num_instructions);
    std::memcpy(instructions.data(), cr.bytecode.data(), cr.bytecode.size());

    // VM setup (heap-allocate — VM is large)
    auto vm = std::make_unique<cedar::VM>();
    vm->set_sample_rate(static_cast<float>(opts.sample_rate));
    vm->set_bpm(opts.render_bpm);

    // Resolve every URI-keyed asset (banks, soundfonts, samples) declared
    // via CLI flags or via top-level akkado directives.
    if (!load_program_assets(*vm, opts, cr)) {
        return EXIT_FAILURE;
    }

    // Load any wavetable banks declared via wt_load() in source order, so
    // their runtime slot IDs match the inst.rate values the compiler baked
    // into the bytecode. Path is interpreted relative to CWD if not absolute.
    for (const auto& wt : cr.required_wavetables) {
        std::string err;
        const int id = vm->wavetable_registry().load_from_file(
            wt.name, wt.path, &err);
        if (id < 0) {
            std::cerr << "error: " << err << "\n";
            return EXIT_FAILURE;
        }
        if (id != wt.id) {
            std::cerr << "warning: wavetable '" << wt.name
                      << "' got runtime slot " << id
                      << " but compiler expected " << wt.id << "\n";
        }
    }

    if (!vm->load_program_immediate(std::span<const cedar::Instruction>(instructions))) {
        std::cerr << "error: failed to load program into VM\n";
        return EXIT_FAILURE;
    }

    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_state_inits(*vm, cr, seq_storage);

    // Find PolyAllocState IDs (for tracing)
    std::vector<std::uint32_t> poly_state_ids;
    for (const auto& init : cr.state_inits) {
        if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            poly_state_ids.push_back(init.state_id);
        }
    }

    // Determine output WAV path
    std::string wav_path = opts.output_file.value_or("out.wav");

    // Optional poly trace
    std::ofstream trace;
    if (opts.trace_poly_file) {
        trace.open(*opts.trace_poly_file);
        if (!trace) {
            std::cerr << "error: cannot open trace file: " << *opts.trace_poly_file << "\n";
            return EXIT_FAILURE;
        }
    }

    // Render loop
    const std::uint32_t total_blocks = static_cast<std::uint32_t>(
        opts.render_seconds * static_cast<float>(opts.sample_rate) / cedar::BLOCK_SIZE);
    std::vector<float> interleaved;
    interleaved.reserve(total_blocks * cedar::BLOCK_SIZE * 2);

    std::array<float, cedar::BLOCK_SIZE> left{}, right{};

    for (std::uint32_t b = 0; b < total_blocks; ++b) {
        vm->process_block(left.data(), right.data());

        for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
            interleaved.push_back(left[i]);
            interleaved.push_back(right[i]);
        }

        // Write per-block voice state for each PolyAllocState
        if (trace.is_open()) {
            for (std::uint32_t state_id : poly_state_ids) {
                auto* poly = vm->states().get_if<cedar::PolyAllocState>(state_id);
                if (!poly || !poly->voices) continue;
                trace << "{\"block\":" << b
                      << ",\"state_id\":" << state_id
                      << ",\"max_voices\":" << static_cast<int>(poly->max_voices)
                      << ",\"voices\":[";
                bool first = true;
                for (std::uint16_t v = 0; v < poly->max_voices; ++v) {
                    const auto& voice = poly->voices[v];
                    if (!voice.active) continue;
                    if (!first) trace << ",";
                    first = false;
                    trace << "{\"i\":" << v
                          << ",\"freq\":" << voice.freq
                          << ",\"vel\":" << voice.vel
                          << ",\"gate\":" << voice.gate
                          << ",\"releasing\":" << (voice.releasing ? "true" : "false")
                          << ",\"age\":" << voice.age
                          << ",\"event\":" << voice.event_index
                          << ",\"cycle\":" << voice.cycle
                          << "}";
                }
                trace << "]}\n";
            }
        }
    }

    // Write WAV
    if (!write_wav_16(wav_path, interleaved, opts.sample_rate, 2)) {
        std::cerr << "error: failed to write WAV: " << wav_path << "\n";
        return EXIT_FAILURE;
    }

    if (opts.verbose) {
        std::cerr << "Rendered " << opts.render_seconds << "s "
                  << "(" << total_blocks << " blocks, "
                  << total_blocks * cedar::BLOCK_SIZE << " samples) to "
                  << wav_path << "\n";
        if (opts.trace_poly_file) {
            std::cerr << "Poly trace: " << *opts.trace_poly_file << "\n";
        }
    }

    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Parse arguments
    auto opts = parse_args(argc, argv);
    if (!opts) {
        return EXIT_FAILURE;
    }

    // Set up the URI resolver with native handlers (file:// http(s)://
    // github: bundled://). The cache is process-scoped under
    // $XDG_CACHE_HOME/nkido (or platform equivalent) and outlives all
    // resolver lookups so HTTP fetches benefit from disk caching across
    // runs.
    static cedar::FileCache uri_cache;
    nkido::register_native_handlers(uri_cache);

    // --list-devices stands on its own; no input required.
    if (opts->list_devices) {
        nkido::AudioEngine::list_capture_devices(std::cout);
        return EXIT_SUCCESS;
    }

    // Handle check mode separately
    if (opts->mode == nkido::Mode::Check) {
        return handle_check_mode(*opts);
    }

    // Handle render mode (offline WAV)
    if (opts->mode == nkido::Mode::Render) {
        return handle_render_mode(*opts);
    }

    // Handle UI mode
    if (opts->mode == nkido::Mode::UI) {
        nkido::ui::UIMode ui;
        if (!ui.init(opts->sample_rate, opts->buffer_size)) {
            std::cerr << "error: failed to initialize UI\n";
            return EXIT_FAILURE;
        }
        return ui.run();
    }

    // Handle serve mode (headless JSON-over-stdio for editor integration)
    if (opts->mode == nkido::Mode::Serve) {
        return nkido::run_serve_mode(*opts);
    }

    // Load/compile bytecode
    auto result = nkido::load_bytecode(*opts);
    if (!result.success) {
        std::cerr << result.error_message << "\n";
        return EXIT_FAILURE;
    }

    // Show compilation stats if verbose
    if (opts->verbose && result.stats) {
        std::cerr << "Compiled " << result.stats->source_bytes << " bytes "
                  << "to " << result.stats->instruction_count << " instructions "
                  << "in " << result.stats->compile_time_ms << " ms\n";
    }

    // Handle dump mode or --dump-bytecode
    if (opts->mode == nkido::Mode::Dump || opts->dump_bytecode) {
        if (opts->json_output) {
            std::cout << nkido::format_program_json(result.instructions);
        } else {
            std::cout << nkido::format_program(result.instructions);
        }

        if (opts->mode == nkido::Mode::Dump) {
            return EXIT_SUCCESS;
        }
    }

    // Handle compile mode
    if (opts->mode == nkido::Mode::Compile) {
        if (!opts->output_file) {
            std::cerr << "error: no output file specified\n";
            return EXIT_FAILURE;
        }

        if (!nkido::write_bytecode_file(*opts->output_file, result.instructions)) {
            std::cerr << "error: failed to write output file: " << *opts->output_file << "\n";
            return EXIT_FAILURE;
        }

        if (opts->verbose) {
            std::cerr << "Wrote " << result.instructions.size() << " instructions to "
                      << *opts->output_file << "\n";
        }

        return EXIT_SUCCESS;
    }

    // Play mode - initialize audio engine
    nkido::AudioEngine engine;
    nkido::AudioEngine::Config audio_config{
        opts->sample_rate,
        opts->buffer_size,
        2  // stereo
    };

    if (!engine.init(audio_config)) {
        std::cerr << "error: failed to initialize audio\n";
        return EXIT_FAILURE;
    }

    // Open capture device if the program contains an INPUT instruction (i.e.
    // uses in()) or if --input-device was explicitly requested. Failure is
    // non-fatal — in() returns silence per the audio-input PRD contract.
    bool program_uses_input = false;
    for (const auto& inst : result.instructions) {
        if (inst.opcode == cedar::Opcode::INPUT) {
            program_uses_input = true;
            break;
        }
    }
    if (opts->input_device.has_value() || program_uses_input) {
        const char* dev = opts->input_device ? opts->input_device->c_str() : nullptr;
        engine.init_capture(dev);
    }

    // Load program into VM
    auto load_result = engine.vm().load_program_immediate(result.instructions);
    if (!load_result) {
        std::cerr << "error: failed to load program into VM\n";
        return EXIT_FAILURE;
    }

    // Install signal handlers for graceful shutdown
    nkido::install_signal_handlers();

    // Start playback
    if (!engine.start()) {
        std::cerr << "error: failed to start audio playback\n";
        return EXIT_FAILURE;
    }

    std::cerr << "Playing... (Ctrl+C to stop)\n";
    if (opts->verbose) {
        std::cerr << "Sample rate: " << opts->sample_rate << " Hz\n";
        std::cerr << "Buffer size: " << opts->buffer_size << " samples\n";
        std::cerr << "Instructions: " << result.instructions.size() << "\n";
    }

    // Wait for shutdown signal
    engine.wait_for_shutdown();

    // Clean shutdown
    engine.stop();
    std::cerr << "\nStopped.\n";

    return EXIT_SUCCESS;
}
