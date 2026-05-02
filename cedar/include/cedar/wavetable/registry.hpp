#pragma once

#include "cedar/io/buffer.hpp"
#include "cedar/wavetable/bank.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cedar {

/// Maximum number of wavetable banks that can be registered simultaneously.
/// 8 bits of inst.rate are used to encode the bank ID at the opcode level,
/// so the absolute ceiling is 256; we cap at 64 for headroom and to keep
/// the per-block snapshot array compact.
constexpr std::size_t MAX_WAVETABLE_BANKS = 64;

/// Audio-thread snapshot of all currently registered banks. Indexed by
/// bank ID (0..MAX_WAVETABLE_BANKS-1). Slots beyond `count` are guaranteed
/// nullptr.
struct WavetableBankSnapshot {
    std::array<const WavetableBank*, MAX_WAVETABLE_BANKS> banks{};
    std::uint8_t count = 0;
};

/// Multi-bank registry. Each registered bank gets a sequential ID assigned
/// at registration time; the opcode reads the bank by ID via inst.rate.
/// Same model as `SoundFontRegistry`: name → ID at compile time, ID → bank
/// at audio time.
///
/// Threading: the host thread owns mutation (load_from_*, set_named, clear).
/// The audio thread reads via `snapshot(...)` once per block, which copies
/// the current bank-pointer array and pins shared_ptrs to keep the banks
/// alive through the block. The mutex is held only for the duration of the
/// snapshot copy (a few atomic refcount ops).
class WavetableBankRegistry {
public:
    WavetableBankRegistry() = default;

    WavetableBankRegistry(const WavetableBankRegistry&) = delete;
    WavetableBankRegistry& operator=(const WavetableBankRegistry&) = delete;

    /// Native: load a WAV file from disk, preprocess it, and register it
    /// under `name`. Returns the assigned bank ID (≥ 0) on success, -1 on
    /// failure. If a bank with `name` already exists, returns its existing
    /// ID without reloading. CEDAR_NO_FILE_IO builds always return -1.
    int load_from_file(const std::string& name, const std::string& path,
                       std::string* error_out = nullptr);

    /// WASM/host: load from an in-memory WAV byte buffer.
    int load_from_memory(const std::string& name, MemoryView data,
                         std::string* error_out = nullptr);

    /// Register a pre-built bank under `name`. Used by tests / synthetic
    /// banks. Returns the assigned bank ID (≥ 0).
    int set_named(const std::string& name,
                  std::shared_ptr<const WavetableBank> bank);

    /// Look up the bank ID for `name`, or -1 if not registered.
    [[nodiscard]] int find_id(const std::string& name) const;

    /// Get a strong reference to the bank with the given ID. Returns null
    /// if id is out of range. Safe to call from any thread.
    [[nodiscard]] std::shared_ptr<const WavetableBank> get(int id) const;

    /// Number of registered banks.
    [[nodiscard]] std::size_t size() const;

    /// True iff a bank named `name` is registered.
    [[nodiscard]] bool has(const std::string& name) const;

    /// Drop all banks. Existing shared_ptr references held by audio-thread
    /// snapshots remain valid until the next block boundary.
    void clear();

    /// Audio-thread snapshot. The output array's raw pointers stay valid
    /// for the lifetime of `pins` — pin those shared_ptrs for the duration
    /// of the audio block.
    void snapshot(WavetableBankSnapshot& out_snapshot,
                  std::array<std::shared_ptr<const WavetableBank>,
                              MAX_WAVETABLE_BANKS>& pins) const;

private:
    mutable std::mutex mu_;
    std::vector<std::shared_ptr<const WavetableBank>> banks_;
    std::unordered_map<std::string, int> name_to_id_;
};

}  // namespace cedar
