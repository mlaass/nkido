#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace catalog {

// One central catalog database (prd-studio-sample-catalog, [OQ5] resolved
// 2026-07-17: central DB). WAL mode, integrity-checked on open, versioned
// migrations from day one (schema_info.version). A corrupt database is
// renamed aside and rebuilt empty — rescan repopulates it.
class Database {
public:
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens (creating if needed), checks integrity, applies migrations.
    // Returns nullptr on failure with *error set. A database written by a
    // NEWER schema than this build understands is refused, not migrated down.
    static std::unique_ptr<Database> open(const std::string& path, std::string* error = nullptr);

    static constexpr int kSchemaVersion = 1;

    int schema_version() const;
    // True when open() found a corrupt file and renamed it aside (to
    // "<path>.corrupt-<unixtime>") before rebuilding.
    bool was_rebuilt() const { return rebuilt_; }
    const std::string& path() const { return path_; }

    // --- roots (watched folders) ---
    struct Root {
        std::int64_t id = 0;
        std::string path;  // absolute, normalized
        std::string kind;  // 'user' | 'vendor:splice' | ...
        bool enabled = true;
        std::int64_t last_scan_at = 0;  // unix seconds; 0 = never
    };
    // Returns the root id, inserting if new (path is normalized to an
    // absolute path with no trailing slash). Returns -1 on error.
    std::int64_t add_root(const std::string& path, const std::string& kind = "user",
                          std::string* error = nullptr);
    std::vector<Root> roots() const;
    bool set_root_last_scan(std::int64_t root_id, std::int64_t unix_time);

    // --- stats (CLI `catalog stats`) ---
    struct Stats {
        std::int64_t roots = 0;
        std::int64_t files = 0;
        std::int64_t missing = 0;      // tombstoned rows
        std::int64_t decode_errors = 0;
        std::int64_t total_bytes = 0;
        double total_duration_s = 0.0;
    };
    Stats stats() const;

    // --- low-level access for the scanner / later phases ---
    sqlite3* raw() { return db_; }
    bool exec(const char* sql, std::string* error = nullptr);
    bool begin();
    bool commit();
    bool rollback();

private:
    Database() = default;

    sqlite3* db_ = nullptr;
    std::string path_;
    bool rebuilt_ = false;
};

}  // namespace catalog
