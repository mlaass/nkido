#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "catalog/database.hpp"
#include "sqlite3.h"
#include "test_helpers.hpp"

using catalog::Database;
using catalog::test::TempDir;
using catalog::test::write_text;

namespace {

std::string query_single_text(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
    std::string out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (const unsigned char* t = sqlite3_column_text(stmt, 0)) {
            out = reinterpret_cast<const char*>(t);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

}  // namespace

TEST_CASE("open creates schema v1", "[database]") {
    TempDir tmp;
    std::string error;
    auto db = Database::open(tmp.str("catalog.db"), &error);
    REQUIRE(db);
    CHECK(error.empty());
    CHECK(db->schema_version() == 1);
    CHECK_FALSE(db->was_rebuilt());

    // Every table of the frozen v1 surface exists.
    for (const char* table : {"schema_info", "roots", "files", "tags", "file_tags", "collections",
                              "collection_pins", "files_fts", "vendor_state"}) {
        const std::string sql = "SELECT name FROM sqlite_master WHERE name='" +
                                std::string(table) + "'";
        INFO(table);
        CHECK(query_single_text(db->raw(), sql.c_str()) == table);
    }

    CHECK(query_single_text(db->raw(), "PRAGMA journal_mode") == "wal");
}

TEST_CASE("reopen is idempotent", "[database]") {
    TempDir tmp;
    { REQUIRE(Database::open(tmp.str("catalog.db"))); }
    auto db = Database::open(tmp.str("catalog.db"));
    REQUIRE(db);
    CHECK(db->schema_version() == 1);
    CHECK_FALSE(db->was_rebuilt());
}

TEST_CASE("newer schema versions are refused, not migrated down", "[database]") {
    TempDir tmp;
    {
        auto db = Database::open(tmp.str("catalog.db"));
        REQUIRE(db);
        REQUIRE(db->exec("UPDATE schema_info SET value='99' WHERE key='version'"));
    }
    std::string error;
    auto db = Database::open(tmp.str("catalog.db"), &error);
    CHECK_FALSE(db);
    CHECK(error.find("newer") != std::string::npos);
}

TEST_CASE("corrupt database is renamed aside and rebuilt", "[database]") {
    TempDir tmp;
    const std::string path = tmp.str("catalog.db");
    // A valid header with a trashed interior: enough bytes that SQLite opens
    // the file and integrity_check (or the schema read) fails.
    std::string garbage = "SQLite format 3";
    garbage.push_back('\0');
    garbage.append(4096, '\x5a');
    write_text(path, garbage);

    std::string error;
    auto db = Database::open(path, &error);
    REQUIRE(db);
    CHECK(db->was_rebuilt());
    CHECK(db->schema_version() == 1);

    bool aside_exists = false;
    for (const auto& e : std::filesystem::directory_iterator(tmp.path())) {
        if (e.path().filename().string().find("catalog.db.corrupt-") == 0) aside_exists = true;
    }
    CHECK(aside_exists);
}

TEST_CASE("add_root normalizes and dedupes", "[database]") {
    TempDir tmp;
    auto db = Database::open(tmp.str("catalog.db"));
    REQUIRE(db);

    const auto id1 = db->add_root(tmp.str("samples"));
    REQUIRE(id1 > 0);
    // Same path with a trailing slash lands on the same row.
    const auto id2 = db->add_root(tmp.str("samples") + "/");
    CHECK(id2 == id1);

    const auto roots = db->roots();
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].kind == "user");
    CHECK(roots[0].last_scan_at == 0);
    CHECK(roots[0].enabled);

    CHECK(db->set_root_last_scan(id1, 12345));
    CHECK(db->roots()[0].last_scan_at == 12345);
}

TEST_CASE("stats on an empty catalog", "[database]") {
    TempDir tmp;
    auto db = Database::open(tmp.str("catalog.db"));
    REQUIRE(db);
    const auto s = db->stats();
    CHECK(s.roots == 0);
    CHECK(s.files == 0);
    CHECK(s.missing == 0);
    CHECK(s.decode_errors == 0);
    CHECK(s.total_bytes == 0);
}

TEST_CASE("fts5 is compiled in", "[database]") {
    TempDir tmp;
    auto db = Database::open(tmp.str("catalog.db"));
    REQUIRE(db);
    // files_fts is part of schema v1; inserting through it proves FTS5 works.
    REQUIRE(db->exec("INSERT INTO files_fts (rowid, filename) VALUES (1, 'kick_drum')"));
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db->raw(),
                               "SELECT rowid FROM files_fts WHERE files_fts MATCH 'kick_drum'", -1,
                               &stmt, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
}
