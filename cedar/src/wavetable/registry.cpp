#include "cedar/wavetable/registry.hpp"
#include "cedar/wavetable/preprocessor.hpp"
#include "cedar/audio/wav_loader.hpp"

#include <utility>

namespace cedar {

namespace {

// Common path for the public load_from_{file,memory} entry points: take a
// fully-built bank and either return the existing ID for `name` (dedup) or
// append a new slot. Returns -1 if the registry is full. Caller holds mu_.
int insert_or_replace_locked(std::vector<std::shared_ptr<const WavetableBank>>& banks,
                              std::unordered_map<std::string, int>& name_to_id,
                              const std::string& name,
                              std::shared_ptr<const WavetableBank> bank,
                              std::string* error_out) {
    auto it = name_to_id.find(name);
    if (it != name_to_id.end()) {
        // Replace the existing bank in-place; ID is preserved so existing
        // bytecode references stay valid.
        banks[static_cast<std::size_t>(it->second)] = std::move(bank);
        return it->second;
    }
    if (banks.size() >= MAX_WAVETABLE_BANKS) {
        if (error_out) {
            *error_out = "wt_load: registry is full (max "
                       + std::to_string(MAX_WAVETABLE_BANKS) + " banks)";
        }
        return -1;
    }
    const int id = static_cast<int>(banks.size());
    banks.push_back(std::move(bank));
    name_to_id.emplace(name, id);
    return id;
}

}  // namespace

int WavetableBankRegistry::load_from_file(const std::string& name,
                                           const std::string& path,
                                           std::string* error_out) {
#ifdef CEDAR_NO_FILE_IO
    (void)name;
    (void)path;
    if (error_out) {
        *error_out = "wt_load: filesystem disabled in this build "
                     "(use the WASM byte-buffer bridge instead)";
    }
    return -1;
#else
    WavData wav = WavLoader::load_from_file(path);
    if (!wav.success) {
        if (error_out) *error_out = "wt_load: " + wav.error_message;
        return -1;
    }
    auto bank = build_bank_from_wav(name, wav, error_out);
    if (!bank) return -1;
    std::lock_guard<std::mutex> lk(mu_);
    return insert_or_replace_locked(banks_, name_to_id_, name,
                                     std::move(bank), error_out);
#endif
}

int WavetableBankRegistry::load_from_memory(const std::string& name,
                                              MemoryView data,
                                              std::string* error_out) {
    WavData wav = WavLoader::load_from_memory(data);
    if (!wav.success) {
        if (error_out) *error_out = "wt_load: " + wav.error_message;
        return -1;
    }
    auto bank = build_bank_from_wav(name, wav, error_out);
    if (!bank) return -1;
    std::lock_guard<std::mutex> lk(mu_);
    return insert_or_replace_locked(banks_, name_to_id_, name,
                                     std::move(bank), error_out);
}

int WavetableBankRegistry::set_named(const std::string& name,
                                      std::shared_ptr<const WavetableBank> bank) {
    std::lock_guard<std::mutex> lk(mu_);
    return insert_or_replace_locked(banks_, name_to_id_, name,
                                     std::move(bank), nullptr);
}

int WavetableBankRegistry::find_id(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = name_to_id_.find(name);
    return it == name_to_id_.end() ? -1 : it->second;
}

std::shared_ptr<const WavetableBank> WavetableBankRegistry::get(int id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (id < 0 || static_cast<std::size_t>(id) >= banks_.size()) return nullptr;
    return banks_[static_cast<std::size_t>(id)];
}

std::size_t WavetableBankRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return banks_.size();
}

bool WavetableBankRegistry::has(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    return name_to_id_.find(name) != name_to_id_.end();
}

void WavetableBankRegistry::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    banks_.clear();
    name_to_id_.clear();
}

void WavetableBankRegistry::snapshot(
    WavetableBankSnapshot& out_snapshot,
    std::array<std::shared_ptr<const WavetableBank>,
                MAX_WAVETABLE_BANKS>& pins) const {
    std::lock_guard<std::mutex> lk(mu_);
    const std::size_t n = std::min(banks_.size(), MAX_WAVETABLE_BANKS);
    for (std::size_t i = 0; i < n; ++i) {
        pins[i] = banks_[i];                 // pin lifetime
        out_snapshot.banks[i] = pins[i].get();
    }
    for (std::size_t i = n; i < MAX_WAVETABLE_BANKS; ++i) {
        pins[i].reset();
        out_snapshot.banks[i] = nullptr;
    }
    out_snapshot.count = static_cast<std::uint8_t>(n);
}

}  // namespace cedar
