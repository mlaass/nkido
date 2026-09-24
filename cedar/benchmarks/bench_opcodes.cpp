// cedar_bench — per-opcode microbenchmark + output dump for the SIMD
// autoresearch loop (docs/prd-simd-autoresearch.md §5.3).
//
//   cedar_bench --list
//   cedar_bench --opcode op_mul [--reps 500] [--warmup 200] [--json]
//   cedar_bench --opcode op_mul --dump out.f32 [--blocks 1024]
//   add --scalar to force the scalar fallback on a CEDAR_SIMD build
//
// Timing: one program of INSTANCES copies of the opcode (distinct state ids,
// shared stimulus inputs) is run through VM::process_block; ns/block is the
// per-copy cost, with an interleaved empty-program VM's time subtracted. Stimuli rotate
// through STIM_BLOCKS precomputed blocks so the opcode never sees a constant.
//
// Dump: a fresh single-copy VM is run for --blocks blocks; the opcode's two
// output buffers (out, out+1) are written as raw float32, L block then R
// block. This is the A/B oracle for the equality gate — the stimuli are the
// same ones the timing path uses (one source of truth).

#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/dsp/simd.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace cedar;

namespace {

constexpr std::size_t INSTANCES = 64;
constexpr std::size_t STIM_BLOCKS = 256;
constexpr std::uint16_t IN_BASE = 0;    // stimulus buffers 0..4
constexpr std::uint16_t OUT_BASE = 8;   // outputs at 8 + 2k (stereo pairs)
constexpr float SR = 48000.0f;
constexpr std::uint16_t NONE = 0xFFFF;

// Stimulus for one input slot at absolute sample index n (0..STIM_BLOCKS*BLOCK_SIZE).
using Stim = std::function<float(std::size_t n)>;

float frac(std::size_t n, std::size_t period) {
    return static_cast<float>(n % period) / static_cast<float>(period);
}

// Audio-rate signal input: seeded noise → log sine sweep → impulse train →
// ramp, a quarter of the stimulus window each (PRD §4.3 fixed stimuli).
struct AudioStim {
    float gain;
    std::vector<float> noise;
    explicit AudioStim(float g) : gain(g), noise(STIM_BLOCKS * BLOCK_SIZE) {
        std::mt19937 rng(0xCEDA5u);
        std::uniform_real_distribution<float> d(-1.0f, 1.0f);
        for (auto& x : noise) x = d(rng);
    }
    float operator()(std::size_t n) const {
        constexpr std::size_t total = STIM_BLOCKS * BLOCK_SIZE;
        constexpr std::size_t quarter = total / 4;
        std::size_t seg = n / quarter, k = n % quarter;
        float t = static_cast<float>(k) / static_cast<float>(quarter);
        switch (seg) {
            case 0: return gain * noise[n];
            case 1: {
                // 20 Hz → 20 kHz exponential sweep, phase-integrated
                double f0 = 20.0, f1 = 20000.0, T = double(quarter) / double(SR);
                double tt = double(k) / double(SR);
                double kk = std::log(f1 / f0) / T;
                double ph = 2.0 * 3.14159265358979 * f0 * (std::exp(kk * tt) - 1.0) / kk;
                return gain * static_cast<float>(std::sin(ph));
            }
            case 2: return (k % 997 == 0) ? gain : 0.0f;
            default: return gain * (2.0f * t - 1.0f);
        }
    }
};

Stim ramp(float lo, float hi, std::size_t period) {
    return [=](std::size_t n) { return lo + (hi - lo) * frac(n, period); };
}
Stim constant(float v) { return [=](std::size_t) { return v; }; }

struct Case {
    const char* name;
    Opcode op;
    std::vector<Stim> inputs;      // wired to inputs[0..]; remaining slots NONE
    std::uint16_t first_unused = 0; // inputs >= this index are NONE
};

std::vector<Case> make_cases() {
    AudioStim audio(1.0f), hot(3.0f);
    constexpr std::size_t P = 48000;  // param sweep period (~1 s)
    std::vector<Case> c;
    c.push_back({"op_add", Opcode::ADD, {audio, ramp(-1, 1, 777)}});
    c.push_back({"op_sub", Opcode::SUB, {audio, ramp(-1, 1, 777)}});
    c.push_back({"op_mul", Opcode::MUL, {audio, ramp(-1, 1, 777)}});
    c.push_back({"op_distort_tanh", Opcode::DISTORT_TANH, {hot, ramp(0.5f, 8.0f, P)}});
    c.push_back({"op_distort_soft", Opcode::DISTORT_SOFT, {hot, ramp(0.1f, 2.0f, P)}});
    c.push_back({"op_filter_formant", Opcode::FILTER_FORMANT,
                 {audio, ramp(0, 4, P), constant(2.0f), ramp(0, 1, P / 3), ramp(1, 20, P / 2)}});
    for (auto [name, op] : {std::pair{"op_filter_svf_lp", Opcode::FILTER_SVF_LP},
                            std::pair{"op_filter_svf_hp", Opcode::FILTER_SVF_HP},
                            std::pair{"op_filter_svf_bp", Opcode::FILTER_SVF_BP}}) {
        c.push_back({name, op, {audio, ramp(40, 12000, P), ramp(0.5f, 8.0f, P / 2)}});
    }
    // C5 survey candidates (PRD OQ5) — ranked by --list-ranking, not gated
    // unless nominated.
    c.push_back({"op_reverb_freeverb", Opcode::REVERB_FREEVERB,
                 {audio, ramp(0, 1, P), ramp(0, 1, P / 2), constant(0.28f), constant(0.7f)}});
    c.push_back({"op_reverb_fdn", Opcode::REVERB_FDN, {audio, ramp(0, 0.95f, P), ramp(0, 1, P / 2)}});
    c.push_back({"op_dynamics_comp", Opcode::DYNAMICS_COMP,
                 {hot, ramp(-40, 0, P), ramp(1, 20, P / 2)}});
    c.push_back({"op_distort_tube", Opcode::DISTORT_TUBE, {audio, ramp(1, 20, P), ramp(0, 0.3f, P / 2)}});
    c.push_back({"op_distort_tape", Opcode::DISTORT_TAPE,
                 {hot, ramp(1, 10, P), ramp(0, 1, P / 2), constant(0.5f), constant(0.7f)}});
    for (auto& k : c) k.first_unused = static_cast<std::uint16_t>(k.inputs.size());
    return c;
}

Instruction make_inst(const Case& c, std::size_t k) {
    Instruction inst{};
    inst.opcode = c.op;
    inst.out_buffer = static_cast<std::uint16_t>(OUT_BASE + 2 * k);
    for (std::size_t s = 0; s < 5; ++s)
        inst.inputs[s] = s < c.first_unused ? static_cast<std::uint16_t>(IN_BASE + s) : NONE;
    inst.state_id = static_cast<std::uint32_t>(0x5EED0000u + k);
    return inst;
}

struct Rig {
    std::unique_ptr<VM> vm = std::make_unique<VM>();
    std::vector<std::vector<float>> stim;  // [slot][STIM_BLOCKS*BLOCK_SIZE]
    std::array<float, BLOCK_SIZE> l{}, r{};

    Rig(const Case& c, std::size_t copies) {
        vm->set_sample_rate(SR);
        std::vector<Instruction> prog;
        for (std::size_t k = 0; k < copies; ++k) prog.push_back(make_inst(c, k));
        if (!vm->load_program_immediate(prog)) {
            std::fprintf(stderr, "load_program_immediate failed\n");
            std::exit(2);
        }
        for (const auto& s : c.inputs) {
            std::vector<float> v(STIM_BLOCKS * BLOCK_SIZE);
            for (std::size_t n = 0; n < v.size(); ++n) v[n] = s(n);
            stim.push_back(std::move(v));
        }
    }
    void load(std::size_t b) {
        std::size_t off = (b % STIM_BLOCKS) * BLOCK_SIZE;
        for (std::size_t s = 0; s < stim.size(); ++s)
            std::memcpy(vm->buffers().get(static_cast<std::uint16_t>(IN_BASE + s)),
                        stim[s].data() + off, BLOCK_SIZE * sizeof(float));
    }
    void run() { vm->process_block(l.data(), r.data()); }
    void block(std::size_t b) { load(b); run(); }
};

std::string read_first_line(const char* path, std::string_view key = {}) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (key.empty()) return line;
        if (line.rfind(key, 0) == 0) {
            auto p = line.find(':');
            return p == std::string::npos ? line : line.substr(p + 2);
        }
    }
    return "unknown";
}

std::string json_escape(std::string s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\';
        o += ch;
    }
    return o;
}

int usage() {
    std::fprintf(stderr,
        "usage: cedar_bench --list\n"
        "       cedar_bench --opcode NAME [--reps N] [--warmup N] [--json]\n"
        "       cedar_bench --opcode NAME --dump FILE [--blocks N]\n"
        "       --scalar forces the scalar fallback path (A/B the dispatch)\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string opcode, dump;
    std::size_t reps = 500, warmup = 200, blocks = 1024;
    bool json = false, list = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--list") list = true;
        else if (a == "--json") json = true;
        else if (a == "--opcode" && (v = next())) opcode = v;
        else if (a == "--dump" && (v = next())) dump = v;
        else if (a == "--reps" && (v = next())) reps = std::stoul(v);
        else if (a == "--warmup" && (v = next())) warmup = std::stoul(v);
        else if (a == "--blocks" && (v = next())) blocks = std::stoul(v);
        else if (a == "--scalar") simd::force(simd::Isa::Scalar);
        else return usage();
    }

    auto cases = make_cases();
    if (list) {
        for (const auto& c : cases) std::printf("%s\n", c.name);
        return 0;
    }
    auto it = std::find_if(cases.begin(), cases.end(),
                           [&](const Case& c) { return opcode == c.name; });
    if (it == cases.end()) {
        std::fprintf(stderr, "unknown opcode '%s' (see --list)\n", opcode.c_str());
        return 2;
    }

    if (!dump.empty()) {
        Rig rig(*it, 1);
        std::ofstream f(dump, std::ios::binary);
        for (std::size_t b = 0; b < blocks; ++b) {
            rig.block(b);
            for (std::uint16_t ch = 0; ch < 2; ++ch)
                f.write(reinterpret_cast<const char*>(rig.vm->buffers().get(
                            static_cast<std::uint16_t>(OUT_BASE + ch))),
                        BLOCK_SIZE * sizeof(float));
        }
        return f ? 0 : 1;
    }

    Rig rig(*it, INSTANCES);
    for (std::size_t b = 0; b < warmup; ++b) rig.block(b);
    std::vector<double> ns(reps);
    // One rep = REP_BLOCKS blocks spread evenly over the stimulus window, so
    // every rep sees every stimulus segment (tanh cost is input-dependent;
    // per-block timing would make p10/p90 measure the stimulus, not noise).
    // Stimulus copies stay outside the timed region.
    // An empty-program VM is timed interleaved with the real one and
    // subtracted: process_block has ~450 ns of fixed per-block overhead,
    // which otherwise swamps cheap opcodes like op_mul.
    constexpr std::size_t REP_BLOCKS = 32;
    Rig empty(*it, 0);
    using clk = std::chrono::steady_clock;
    auto dt = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::nano>(b - a).count();
    };
    for (std::size_t r = 0; r < reps; ++r) {
        double t = 0.0;
        for (std::size_t j = 0; j < REP_BLOCKS; ++j) {
            rig.load(r + j * (STIM_BLOCKS / REP_BLOCKS));
            auto t0 = clk::now();
            empty.run();
            auto t1 = clk::now();
            rig.run();
            auto t2 = clk::now();
            t += dt(t1, t2) - dt(t0, t1);
        }
        ns[r] = t / (REP_BLOCKS * INSTANCES);
    }
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) { return ns[static_cast<std::size_t>(p * double(reps - 1))]; };
    double med = pct(0.5), p10 = pct(0.1), p90 = pct(0.9);

    std::string cpu = read_first_line("/proc/cpuinfo", "model name");
    std::string gov = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    const char* isa = simd::name(simd::active());

    if (json) {
        std::printf("{\"opcode\":\"%s\",\"ns_per_block_median\":%.2f,\"ns_p10\":%.2f,"
                    "\"ns_p90\":%.2f,\"reps\":%zu,\"isa\":\"%s\",\"cpu\":\"%s\","
                    "\"governor\":\"%s\"}\n",
                    it->name, med, p10, p90, reps, isa, json_escape(cpu).c_str(),
                    json_escape(gov).c_str());
    } else {
        std::printf("%-20s median %8.2f ns/block  p10 %8.2f  p90 %8.2f  (%s, %s)\n",
                    it->name, med, p10, p90, isa, gov.c_str());
    }
    if (gov != "performance")
        std::fprintf(stderr, "warning: governor is '%s', not 'performance' — results unstable\n",
                     gov.c_str());
    return 0;
}
