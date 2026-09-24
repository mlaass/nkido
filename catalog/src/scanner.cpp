#include "catalog/scanner.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "catalog/probe.hpp"
#include "sqlite3.h"

namespace catalog {

namespace fs = std::filesystem;

namespace {

bool is_audio_ext(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".wave" || ext == ".flac" || ext == ".ogg" || ext == ".mp3";
}

std::int64_t mtime_unix(const fs::directory_entry& entry, std::error_code& ec) {
    const auto ft = entry.last_write_time(ec);
    if (ec) return 0;
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

struct KnownFile {
    std::int64_t size = 0;
    std::int64_t mtime = 0;
};

// Statement wrapper so the hot loop reuses one prepared UPSERT.
class UpsertStmt {
public:
    UpsertStmt(sqlite3* db) {
        sqlite3_prepare_v2(db,
                           "INSERT INTO files (root_id, rel_path, filename, ext, size_bytes, mtime,"
                           "  duration_s, sample_rate, channels, bit_depth, decode_error, added_at)"
                           " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)"
                           " ON CONFLICT(root_id, rel_path) DO UPDATE SET"
                           "  filename=excluded.filename, ext=excluded.ext,"
                           "  size_bytes=excluded.size_bytes, mtime=excluded.mtime,"
                           "  duration_s=excluded.duration_s, sample_rate=excluded.sample_rate,"
                           "  channels=excluded.channels, bit_depth=excluded.bit_depth,"
                           "  decode_error=excluded.decode_error,"
                           "  content_hash=NULL,"  // stale on content change; Phase 3 recomputes
                           "  missing_since=NULL",
                           -1, &stmt_, nullptr);
    }
    ~UpsertStmt() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    bool valid() const { return stmt_ != nullptr; }

    bool run(std::int64_t root_id, const std::string& rel, const std::string& filename,
             const std::string& ext, std::int64_t size, std::int64_t mtime, const ProbeResult& probe,
             std::int64_t now) {
        sqlite3_reset(stmt_);
        sqlite3_bind_int64(stmt_, 1, root_id);
        sqlite3_bind_text(stmt_, 2, rel.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_, 3, filename.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_, 4, ext.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_, 5, size);
        sqlite3_bind_int64(stmt_, 6, mtime);
        if (probe.success) {
            sqlite3_bind_double(stmt_, 7, probe.duration_s);
            sqlite3_bind_int64(stmt_, 8, probe.sample_rate);
            sqlite3_bind_int64(stmt_, 9, probe.channels);
            if (probe.bit_depth) {
                sqlite3_bind_int64(stmt_, 10, *probe.bit_depth);
            } else {
                sqlite3_bind_null(stmt_, 10);
            }
            sqlite3_bind_null(stmt_, 11);
        } else {
            sqlite3_bind_null(stmt_, 7);
            sqlite3_bind_null(stmt_, 8);
            sqlite3_bind_null(stmt_, 9);
            sqlite3_bind_null(stmt_, 10);
            sqlite3_bind_text(stmt_, 11, probe.error.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(stmt_, 12, now);
        return sqlite3_step(stmt_) == SQLITE_DONE;
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace

ScanStats Scanner::scan_root(std::int64_t root_id, const ScanOptions& options) {
    ScanStats stats;

    std::string root_path;
    for (const auto& r : db_.roots()) {
        if (r.id == root_id) root_path = r.path;
    }
    if (root_path.empty()) {
        stats.root_missing = true;
        return stats;
    }
    std::error_code ec;
    if (!fs::is_directory(root_path, ec) || ec) {
        // Missing root: not deleted, not tombstoned here (grace-period
        // tombstones are Phase 3) — just report and leave rows untouched.
        stats.root_missing = true;
        return stats;
    }

    // One query up front instead of a SELECT per file: rel_path -> (size,
    // mtime). 100k rows is ~10 MB — the unchanged-re-scan budget lives here.
    std::unordered_map<std::string, KnownFile> known;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_.raw(),
                               "SELECT rel_path, size_bytes, mtime FROM files WHERE root_id=?1", -1,
                               &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, root_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                KnownFile kf;
                kf.size = sqlite3_column_int64(stmt, 1);
                kf.mtime = sqlite3_column_int64(stmt, 2);
                known.emplace(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), kf);
            }
            sqlite3_finalize(stmt);
        }
    }

    UpsertStmt upsert(db_.raw());
    if (!upsert.valid()) return stats;

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    bool in_txn = db_.begin();
    int batch = 0;

    // Directory symlinks are not followed in Phase 0; the visited-inode loop
    // guard arrives with the Phase 3 watcher/rescan work.
    auto it = fs::recursive_directory_iterator(root_path,
                                               fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const auto& p = entry.path();
        if (!is_audio_ext(p.extension().string())) continue;

        ++stats.seen;
        const std::string rel = p.lexically_relative(root_path).generic_string();
        std::error_code fec;
        const std::int64_t size = static_cast<std::int64_t>(entry.file_size(fec));
        const std::int64_t mtime = mtime_unix(entry, fec);

        const auto found = known.find(rel);
        const bool existed = found != known.end();
        if (existed && found->second.size == size && found->second.mtime == mtime) {
            ++stats.unchanged;
        } else {
            const ProbeResult probe = probe_file(p.string());
            if (!probe.success) ++stats.errors;
            std::string ext = p.extension().string().substr(1);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (upsert.run(root_id, rel, p.filename().string(), ext, size, mtime, probe, now)) {
                existed ? ++stats.updated : ++stats.added;
            }
            if (++batch >= options.batch_size && in_txn) {
                db_.commit();
                in_txn = db_.begin();
                batch = 0;
            }
        }

        if (options.progress && stats.seen % static_cast<std::uint64_t>(std::max(
                                                 1, options.progress_interval)) ==
                                    0) {
            if (!options.progress(stats)) {
                stats.cancelled = true;
                break;
            }
        }
    }

    if (in_txn) db_.commit();
    if (!stats.cancelled) db_.set_root_last_scan(root_id, now);
    return stats;
}

ScanStats Scanner::scan_all(const ScanOptions& options) {
    ScanStats total;
    for (const auto& r : db_.roots()) {
        if (!r.enabled) continue;
        const ScanStats s = scan_root(r.id, options);
        total.seen += s.seen;
        total.added += s.added;
        total.updated += s.updated;
        total.unchanged += s.unchanged;
        total.errors += s.errors;
        total.cancelled = total.cancelled || s.cancelled;
        total.root_missing = total.root_missing || s.root_missing;
        if (total.cancelled) break;
    }
    return total;
}

}  // namespace catalog
