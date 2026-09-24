#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "catalog/corpus.hpp"
#include "catalog/database.hpp"
#include "catalog/scanner.hpp"
#include "catalog/wav_writer.hpp"
#include "sqlite3.h"
#include "test_helpers.hpp"

using catalog::Database;
using catalog::ScanOptions;
using catalog::Scanner;
using catalog::test::TempDir;
using catalog::test::write_text;

namespace {

std::int64_t count_rows(Database& db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db.raw(), sql, -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    const auto n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

struct Tree {
    TempDir tmp;
    std::string root;

    Tree() : root(tmp.str("lib")) {
        namespace fs = std::filesystem;
        fs::create_directories(root + "/drums/kicks");
        fs::create_directories(root + "/synth");
        const auto pcm = catalog::sine_pcm16(2205, 1, 220.0, 22050);  // 0.1 s
        REQUIRE(catalog::write_wav16(root + "/drums/kicks/kick_01.wav", pcm, 22050, 1));
        REQUIRE(catalog::write_wav16(root + "/drums/snare_88.wav", pcm, 22050, 1));
        REQUIRE(catalog::write_wav16(root + "/synth/pad_Amin.wav", pcm, 22050, 1));
        write_text(root + "/synth/broken.wav", "not really a wav file at all");
        write_text(root + "/readme.txt", "not audio");
    }
};

}  // namespace

TEST_CASE("scan indexes a tree with metadata and error flags", "[scanner]") {
    Tree t;
    auto db = Database::open(t.tmp.str("catalog.db"));
    REQUIRE(db);
    const auto root_id = db->add_root(t.root);
    REQUIRE(root_id > 0);

    Scanner scanner(*db);
    const auto stats = scanner.scan_root(root_id);
    CHECK(stats.seen == 4);   // .txt filtered out
    CHECK(stats.added == 4);
    CHECK(stats.updated == 0);
    CHECK(stats.unchanged == 0);
    CHECK(stats.errors == 1);  // broken.wav
    CHECK_FALSE(stats.cancelled);
    CHECK_FALSE(stats.root_missing);

    CHECK(count_rows(*db, "SELECT COUNT(*) FROM files") == 4);
    CHECK(count_rows(*db, "SELECT COUNT(*) FROM files WHERE decode_error IS NOT NULL") == 1);
    CHECK(count_rows(*db, "SELECT COUNT(*) FROM files WHERE ext='wav'") == 4);

    // Metadata of a healthy row.
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db->raw(),
                               "SELECT sample_rate, channels, bit_depth, duration_s, rel_path"
                               " FROM files WHERE filename='kick_01.wav'",
                               -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 22050);
    CHECK(sqlite3_column_int(stmt, 1) == 1);
    CHECK(sqlite3_column_int(stmt, 2) == 16);
    CHECK(sqlite3_column_double(stmt, 3) > 0.09);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))) ==
          "drums/kicks/kick_01.wav");
    sqlite3_finalize(stmt);

    // Scan completion stamps the root.
    CHECK(db->roots()[0].last_scan_at > 0);
}

TEST_CASE("unchanged rescan probes nothing and adds nothing", "[scanner]") {
    Tree t;
    auto db = Database::open(t.tmp.str("catalog.db"));
    REQUIRE(db);
    const auto root_id = db->add_root(t.root);
    Scanner scanner(*db);
    REQUIRE(scanner.scan_root(root_id).added == 4);

    const auto again = scanner.scan_root(root_id);
    CHECK(again.seen == 4);
    CHECK(again.added == 0);
    CHECK(again.updated == 0);
    CHECK(again.unchanged == 4);
    CHECK(count_rows(*db, "SELECT COUNT(*) FROM files") == 4);
}

TEST_CASE("changed file is re-probed and updated in place", "[scanner]") {
    Tree t;
    auto db = Database::open(t.tmp.str("catalog.db"));
    REQUIRE(db);
    const auto root_id = db->add_root(t.root);
    Scanner scanner(*db);
    REQUIRE(scanner.scan_root(root_id).added == 4);

    // Rewrite with a different length — size change alone must trigger it.
    const auto pcm = catalog::sine_pcm16(4410, 1, 220.0, 22050);
    REQUIRE(catalog::write_wav16(t.root + "/drums/kicks/kick_01.wav", pcm, 22050, 1));

    const auto stats = scanner.scan_root(root_id);
    CHECK(stats.updated == 1);
    CHECK(stats.unchanged == 3);
    CHECK(count_rows(*db, "SELECT COUNT(*) FROM files") == 4);  // updated, not duplicated
    CHECK(count_rows(*db,
                     "SELECT COUNT(*) FROM files WHERE filename='kick_01.wav'"
                     " AND duration_s > 0.19") == 1);
}

TEST_CASE("interrupted scan resumes without duplicate rows", "[scanner]") {
    TempDir tmp;
    const std::string root = tmp.str("corpus");
    REQUIRE(catalog::generate_corpus(root, 200, 50) == 200);

    auto db = Database::open(tmp.str("catalog.db"));
    REQUIRE(db);
    const auto root_id = db->add_root(root);
    Scanner scanner(*db);

    ScanOptions cancel_opts;
    cancel_opts.batch_size = 20;
    cancel_opts.progress_interval = 10;
    cancel_opts.progress = [](const catalog::ScanStats& s) { return s.seen < 60; };
    const auto first = scanner.scan_root(root_id, cancel_opts);
    CHECK(first.cancelled);
    const auto partial = count_rows(*db, "SELECT COUNT(*) FROM files");
    CHECK(partial > 0);
    CHECK(partial < 200);
    // A cancelled scan must not stamp the root as fully scanned.
    CHECK(db->roots()[0].last_scan_at == 0);

    const auto second = scanner.scan_root(root_id);
    CHECK_FALSE(second.cancelled);
    CHECK(second.added + second.unchanged + second.updated == 200);
    CHECK(count_rows(*db, "SELECT COUNT(*) FROM files") == 200);
    CHECK(count_rows(*db, "SELECT COUNT(DISTINCT root_id || '/' || rel_path) FROM files") == 200);

    const auto third = scanner.scan_root(root_id);
    CHECK(third.unchanged == 200);
}

TEST_CASE("missing root reports without touching rows", "[scanner]") {
    TempDir tmp;
    auto db = Database::open(tmp.str("catalog.db"));
    REQUIRE(db);
    const auto root_id = db->add_root(tmp.str("never_created"));
    REQUIRE(root_id > 0);
    Scanner scanner(*db);
    const auto stats = scanner.scan_root(root_id);
    CHECK(stats.root_missing);
    CHECK(stats.seen == 0);
}

TEST_CASE("scan_all covers every enabled root", "[scanner]") {
    Tree t;
    TempDir other;
    const std::string root2 = other.str("more");
    REQUIRE(catalog::generate_corpus(root2, 10, 5) == 10);

    auto db = Database::open(t.tmp.str("catalog.db"));
    REQUIRE(db);
    REQUIRE(db->add_root(t.root) > 0);
    REQUIRE(db->add_root(root2) > 0);

    Scanner scanner(*db);
    const auto stats = scanner.scan_all();
    CHECK(stats.seen == 14);
    CHECK(stats.added == 14);
    CHECK(count_rows(*db, "SELECT COUNT(*) FROM files") == 14);

    const auto s = db->stats();
    CHECK(s.roots == 2);
    CHECK(s.files == 14);
    CHECK(s.decode_errors == 1);
}
