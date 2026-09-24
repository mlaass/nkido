#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "catalog/database.hpp"

namespace catalog {

struct ScanStats {
    std::uint64_t seen = 0;       // audio files encountered on disk
    std::uint64_t added = 0;      // new rows
    std::uint64_t updated = 0;    // size/mtime changed => re-probed
    std::uint64_t unchanged = 0;  // skipped, no probe
    std::uint64_t errors = 0;     // probe failures (row kept, decode_error set)
    bool cancelled = false;       // progress callback asked to stop
    bool root_missing = false;    // root path did not exist
};

struct ScanOptions {
    int batch_size = 500;  // files per write transaction
    // Called every `progress_interval` files; return false to cancel. A
    // cancelled scan commits the current batch — rows stay consistent and a
    // rescan resumes where it left off (unchanged files are skipped).
    std::function<bool(const ScanStats&)> progress;
    int progress_interval = 250;
};

// Recursive scan of watched roots into `files` (Phase 0: no hashing, no
// watchers — those are Phase 3). Single-threaded; the Phase-0 perf budget
// (cold >= 2k files/min, unchanged re-scan of 100k <= 5 s) is asserted by
// scripts/catalog/check_perf.sh.
class Scanner {
public:
    explicit Scanner(Database& db) : db_(db) {}

    ScanStats scan_root(std::int64_t root_id, const ScanOptions& options = {});
    // Scans every enabled root; sums the per-root stats.
    ScanStats scan_all(const ScanOptions& options = {});

private:
    Database& db_;
};

}  // namespace catalog
