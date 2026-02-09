#include <catch2/catch_test_macros.hpp>

#include <cedar/io/buffer.hpp>

#ifndef __EMSCRIPTEN__
#include <cedar/io/file_loader.hpp>
#endif

#include <cstdint>
#include <vector>
#include <fstream>
#include <filesystem>

using namespace cedar;

// ============================================================================
// MemoryView tests [file_loader]
// ============================================================================

TEST_CASE("MemoryView construction", "[file_loader]") {
    SECTION("default construction is empty") {
        MemoryView view;
        CHECK(view.empty());
        CHECK(view.data == nullptr);
        CHECK(view.size == 0);
    }

    SECTION("construct from pointer and size") {
        std::uint8_t data[] = {1, 2, 3, 4};
        MemoryView view(data, 4);

        CHECK_FALSE(view.empty());
        CHECK(view.data == data);
        CHECK(view.size == 4);
    }

    SECTION("construct from vector") {
        std::vector<std::uint8_t> vec = {10, 20, 30};
        MemoryView view(vec);

        CHECK_FALSE(view.empty());
        CHECK(view.data == vec.data());
        CHECK(view.size == 3);
    }

    SECTION("empty vector gives empty view") {
        std::vector<std::uint8_t> vec;
        MemoryView view(vec);

        // data may be non-null for empty vector, but size is 0
        CHECK(view.size == 0);
    }

    SECTION("iteration") {
        std::uint8_t data[] = {5, 10, 15};
        MemoryView view(data, 3);

        std::uint8_t sum = 0;
        for (auto byte : view) {
            sum += byte;
        }
        CHECK(sum == 30);
    }

    SECTION("begin and end") {
        std::uint8_t data[] = {1, 2};
        MemoryView view(data, 2);

        CHECK(view.begin() == data);
        CHECK(view.end() == data + 2);
        CHECK(view.end() - view.begin() == 2);
    }
}

// ============================================================================
// OwnedBuffer tests [file_loader]
// ============================================================================

TEST_CASE("OwnedBuffer", "[file_loader]") {
    SECTION("default construction is empty") {
        OwnedBuffer buf;
        CHECK(buf.empty());
        CHECK(buf.size() == 0);
    }

    SECTION("construct from vector") {
        std::vector<std::uint8_t> data = {1, 2, 3, 4, 5};
        OwnedBuffer buf(std::move(data));

        CHECK_FALSE(buf.empty());
        CHECK(buf.size() == 5);
    }

    SECTION("view() returns valid MemoryView") {
        std::vector<std::uint8_t> data = {10, 20, 30};
        OwnedBuffer buf(std::move(data));

        MemoryView view = buf.view();
        CHECK_FALSE(view.empty());
        CHECK(view.size == 3);
        CHECK(view.data[0] == 10);
        CHECK(view.data[1] == 20);
        CHECK(view.data[2] == 30);
    }

    SECTION("take() moves data out") {
        std::vector<std::uint8_t> data = {1, 2, 3};
        OwnedBuffer buf(std::move(data));

        auto taken = buf.take();
        CHECK(taken.size() == 3);
        CHECK(taken[0] == 1);

        // After take, buffer should be empty
        CHECK(buf.empty());
    }

    SECTION("move construction") {
        std::vector<std::uint8_t> data = {1, 2, 3};
        OwnedBuffer buf1(std::move(data));
        OwnedBuffer buf2(std::move(buf1));

        CHECK(buf2.size() == 3);
        CHECK(buf1.empty());  // NOLINT: testing moved-from state
    }

    SECTION("move assignment") {
        std::vector<std::uint8_t> data1 = {1, 2};
        std::vector<std::uint8_t> data2 = {3, 4, 5};
        OwnedBuffer buf1(std::move(data1));
        OwnedBuffer buf2(std::move(data2));

        buf1 = std::move(buf2);
        CHECK(buf1.size() == 3);
        CHECK(buf2.empty());  // NOLINT: testing moved-from state
    }
}

// ============================================================================
// FileLoader tests [file_loader] (native only)
// ============================================================================

#ifndef __EMSCRIPTEN__

TEST_CASE("FileLoader operations", "[file_loader]") {
    namespace fs = std::filesystem;

    // Create a temporary test file
    auto temp_dir = fs::temp_directory_path() / "cedar_test";
    fs::create_directories(temp_dir);
    auto test_file = temp_dir / "test_data.bin";

    // Write test data
    {
        std::ofstream out(test_file, std::ios::binary);
        std::uint8_t data[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"
        out.write(reinterpret_cast<const char*>(data), 5);
    }

    SECTION("load existing file") {
        auto result = FileLoader::load(test_file.string());

        REQUIRE(result.success());
        CHECK(result.buffer().size() == 5);

        auto view = result.buffer().view();
        CHECK(view.data[0] == 0x48);
        CHECK(view.data[4] == 0x6F);
    }

    SECTION("load missing file") {
        auto result = FileLoader::load((temp_dir / "nonexistent.bin").string());

        CHECK_FALSE(result.success());
        CHECK(result.error().code == FileError::NotFound);
    }

    SECTION("exists returns true for existing file") {
        CHECK(FileLoader::exists(test_file.string()));
    }

    SECTION("exists returns false for missing file") {
        CHECK_FALSE(FileLoader::exists((temp_dir / "missing.bin").string()));
    }

    SECTION("file_size returns correct size") {
        auto size = FileLoader::file_size(test_file.string());
        REQUIRE(size.has_value());
        CHECK(*size == 5);
    }

    SECTION("file_size returns nullopt for missing file") {
        auto size = FileLoader::file_size((temp_dir / "missing.bin").string());
        CHECK_FALSE(size.has_value());
    }

    SECTION("load empty file") {
        auto empty_file = temp_dir / "empty.bin";
        { std::ofstream out(empty_file, std::ios::binary); }

        auto result = FileLoader::load(empty_file.string());
        REQUIRE(result.success());
        CHECK(result.buffer().size() == 0);
    }

    // Cleanup
    fs::remove_all(temp_dir);
}

#endif  // __EMSCRIPTEN__
