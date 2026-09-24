#include "catalog/database.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>

#include "sqlite3.h"

namespace catalog {

namespace {

// Schema v1 (prd-studio-sample-catalog §5, frozen at Phase 0 exit).
// The full surface lands in v1 even though tags/collections/FTS are consumed
// by later phases — later phases add code, not schema, wherever possible.
constexpr const char* kSchemaV1 = R"sql(
CREATE TABLE schema_info (key TEXT PRIMARY KEY, value TEXT);

CREATE TABLE roots (
  id           INTEGER PRIMARY KEY,
  path         TEXT NOT NULL UNIQUE,
  kind         TEXT NOT NULL DEFAULT 'user',
  enabled      INTEGER NOT NULL DEFAULT 1,
  last_scan_at INTEGER
);

CREATE TABLE files (
  id            INTEGER PRIMARY KEY,
  root_id       INTEGER NOT NULL REFERENCES roots(id),
  rel_path      TEXT NOT NULL,
  filename      TEXT NOT NULL,
  ext           TEXT NOT NULL,
  size_bytes    INTEGER NOT NULL,
  mtime         INTEGER NOT NULL,
  content_hash  BLOB,
  duration_s    REAL,
  sample_rate   INTEGER,
  channels      INTEGER,
  bit_depth     INTEGER,
  bpm           REAL,
  bpm_source    TEXT,
  key_root      INTEGER,
  key_mode      TEXT,
  key_source    TEXT,
  root_note     INTEGER,
  one_shot      INTEGER,
  pack          TEXT,
  artist        TEXT,
  license       TEXT,
  source_url    TEXT,
  decode_error  TEXT,
  missing_since INTEGER,
  added_at      INTEGER NOT NULL,
  UNIQUE(root_id, rel_path)
);
CREATE INDEX idx_files_hash ON files(content_hash);

CREATE TABLE tags (
  id        INTEGER PRIMARY KEY,
  name      TEXT UNIQUE COLLATE NOCASE,
  parent_id INTEGER REFERENCES tags(id)
);
CREATE TABLE file_tags (
  file_id INTEGER REFERENCES files(id),
  tag_id  INTEGER REFERENCES tags(id),
  source  TEXT NOT NULL,
  PRIMARY KEY (file_id, tag_id)
);

CREATE TABLE collections (
  id    INTEGER PRIMARY KEY,
  name  TEXT NOT NULL UNIQUE,
  query TEXT
);
CREATE TABLE collection_pins (
  collection_id INTEGER REFERENCES collections(id),
  file_id       INTEGER REFERENCES files(id),
  PRIMARY KEY (collection_id, file_id)
);

CREATE VIRTUAL TABLE files_fts USING fts5(
  filename, folder_terms, descriptors, pack, artist, tags,
  content=''
);

CREATE TABLE vendor_state (
  provider           TEXT PRIMARY KEY,
  schema_fingerprint TEXT,
  enrichment_enabled INTEGER NOT NULL DEFAULT 1,
  cursor             TEXT
);

INSERT INTO schema_info (key, value) VALUES ('version', '1');
)sql";

struct Migration {
    int version;
    const char* sql;
};

// Migrations run in order for every version > the stored one. v1 is the
// creation script; future versions append ALTER/CREATE statements here.
constexpr Migration kMigrations[] = {
    {1, kSchemaV1},
};

bool set_error(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

int query_schema_version(sqlite3* db) {
    // Missing table (fresh or pre-v1 file) reads as version 0.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM schema_info WHERE key='version'", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return 0;
    }
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (const unsigned char* text = sqlite3_column_text(stmt, 0)) {
            version = std::atoi(reinterpret_cast<const char*>(text));
        }
    }
    sqlite3_finalize(stmt);
    return version;
}

bool integrity_ok(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (const unsigned char* text = sqlite3_column_text(stmt, 0)) {
            ok = std::string(reinterpret_cast<const char*>(text)) == "ok";
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

}  // namespace

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

std::unique_ptr<Database> Database::open(const std::string& path, std::string* error) {
    auto make = [&](bool rebuilt) -> std::unique_ptr<Database> {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
            SQLITE_OK) {
            const std::string msg = db ? sqlite3_errmsg(db) : "cannot open database";
            if (db) sqlite3_close(db);
            set_error(error, "sqlite open failed: " + msg);
            return nullptr;
        }
        auto d = std::unique_ptr<Database>(new Database());
        d->db_ = db;
        d->path_ = path;
        d->rebuilt_ = rebuilt;

        char* errmsg = nullptr;
        if (sqlite3_exec(db,
                         "PRAGMA journal_mode=WAL;"
                         "PRAGMA synchronous=NORMAL;"
                         "PRAGMA foreign_keys=ON;"
                         "PRAGMA busy_timeout=5000;",
                         nullptr, nullptr, &errmsg) != SQLITE_OK) {
            set_error(error, errmsg ? errmsg : "pragma setup failed");
            sqlite3_free(errmsg);
            return nullptr;
        }
        return d;
    };

    namespace fs = std::filesystem;
    if (!path.empty() && path != ":memory:") {
        std::error_code ec;
        const auto parent = fs::path(path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
    }

    const bool existing = path != ":memory:" && std::filesystem::exists(path) &&
                          std::filesystem::file_size(path) > 0;

    auto d = make(false);

    // Existing file that won't open (SQLITE_NOTADB from the pragmas) or
    // fails its integrity check: rename aside, rebuild empty. User-authored
    // metadata recovery from the sidecar journal is Phase 3's concern; a
    // fresh DB is always preferable to a wedged one.
    const bool corrupt = existing && (!d || !integrity_ok(d->db_));
    if (!d && !corrupt) return nullptr;
    if (corrupt) {
        d.reset();  // close before renaming
        const std::string aside = path + ".corrupt-" + std::to_string(std::time(nullptr));
        std::error_code ec;
        std::filesystem::rename(path, aside, ec);
        if (ec) {
            set_error(error, "corrupt database and rename-aside failed: " + ec.message());
            return nullptr;
        }
        // WAL/SHM sidecars belong to the corrupt file; drop them.
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
        d = make(true);
        if (!d) return nullptr;
    }

    const int current = query_schema_version(d->db_);
    if (current > kSchemaVersion) {
        set_error(error, "database schema v" + std::to_string(current) +
                             " is newer than this build (v" + std::to_string(kSchemaVersion) + ")");
        return nullptr;
    }
    for (const auto& m : kMigrations) {
        if (m.version <= current) continue;
        std::string exec_error;
        if (!d->begin()) return set_error(error, "migration begin failed"), nullptr;
        if (!d->exec(m.sql, &exec_error)) {
            d->rollback();
            set_error(error, "migration to v" + std::to_string(m.version) + " failed: " + exec_error);
            return nullptr;
        }
        if (m.version > 1) {
            const std::string bump = "UPDATE schema_info SET value='" + std::to_string(m.version) +
                                     "' WHERE key='version'";
            if (!d->exec(bump.c_str(), &exec_error)) {
                d->rollback();
                set_error(error, "migration version bump failed: " + exec_error);
                return nullptr;
            }
        }
        if (!d->commit()) return set_error(error, "migration commit failed"), nullptr;
    }
    return d;
}

int Database::schema_version() const { return query_schema_version(db_); }

bool Database::exec(const char* sql, std::string* error) {
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        set_error(error, errmsg ? errmsg : "exec failed");
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

bool Database::begin() { return exec("BEGIN IMMEDIATE"); }
bool Database::commit() { return exec("COMMIT"); }
bool Database::rollback() { return exec("ROLLBACK"); }

std::int64_t Database::add_root(const std::string& path, const std::string& kind,
                                std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(path), ec);
    if (ec) {
        set_error(error, "cannot resolve path: " + ec.message());
        return -1;
    }
    std::string normalized = abs.lexically_normal().string();
    while (normalized.size() > 1 && normalized.back() == fs::path::preferred_separator) {
        normalized.pop_back();
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO roots (path, kind) VALUES (?1, ?2) "
                           "ON CONFLICT(path) DO UPDATE SET kind=kind RETURNING id",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, normalized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
    std::int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (id < 0) set_error(error, sqlite3_errmsg(db_));
    return id;
}

std::vector<Database::Root> Database::roots() const {
    std::vector<Root> out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT id, path, kind, enabled, COALESCE(last_scan_at,0) "
                           "FROM roots ORDER BY id",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Root r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        r.enabled = sqlite3_column_int(stmt, 3) != 0;
        r.last_scan_at = sqlite3_column_int64(stmt, 4);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool Database::set_root_last_scan(std::int64_t root_id, std::int64_t unix_time) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE roots SET last_scan_at=?2 WHERE id=?1", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, root_id);
    sqlite3_bind_int64(stmt, 2, unix_time);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

Database::Stats Database::stats() const {
    Stats s;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT (SELECT COUNT(*) FROM roots),"
                           " COUNT(*),"
                           " COALESCE(SUM(missing_since IS NOT NULL),0),"
                           " COALESCE(SUM(decode_error IS NOT NULL),0),"
                           " COALESCE(SUM(size_bytes),0),"
                           " COALESCE(SUM(duration_s),0)"
                           " FROM files",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return s;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        s.roots = sqlite3_column_int64(stmt, 0);
        s.files = sqlite3_column_int64(stmt, 1);
        s.missing = sqlite3_column_int64(stmt, 2);
        s.decode_errors = sqlite3_column_int64(stmt, 3);
        s.total_bytes = sqlite3_column_int64(stmt, 4);
        s.total_duration_s = sqlite3_column_double(stmt, 5);
    }
    sqlite3_finalize(stmt);
    return s;
}

}  // namespace catalog
