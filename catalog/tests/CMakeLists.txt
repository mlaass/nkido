# Catalog tests

# Standalone build: need to fetch Catch2
if(NOT TARGET Catch2::Catch2WithMain)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

include(Catch)

add_executable(catalog_tests
    test_database.cpp
    test_probe.cpp
    test_scanner.cpp
)

target_link_libraries(catalog_tests
    PRIVATE
        catalog
        catalog_sqlite3
        nkido_compiler_options
        Catch2::Catch2WithMain
)

catch_discover_tests(catalog_tests)
