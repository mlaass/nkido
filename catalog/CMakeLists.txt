# Catalog - local sample catalog library (SQLite + FTS5)
# Open core of nkido-studio's prd-studio-sample-catalog; a sibling of cedar
# and akkado. Can be built standalone: cmake -B build /path/to/catalog

cmake_minimum_required(VERSION 3.21)

# Allow standalone build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_version_file "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
    if(EXISTS "${_version_file}")
        file(STRINGS "${_version_file}" _catalog_version)
    else()
        set(_catalog_version "0.0.0")
    endif()
    project(catalog
        VERSION ${_catalog_version}
        DESCRIPTION "nkido sample catalog library"
        LANGUAGES C CXX
    )

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    option(CATALOG_BUILD_TESTS "Build catalog tests" ON)

    add_library(nkido_compiler_options INTERFACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(nkido_compiler_options INTERFACE -Wall -Wextra -Wpedantic)
    endif()

    add_subdirectory(../cedar cedar)
else()
    set(CATALOG_BUILD_TESTS ${NKIDO_BUILD_TESTS})
    # The parent project declares LANGUAGES CXX; sqlite3.c needs C.
    enable_language(C)
endif()

# --- vendored SQLite amalgamation (public domain), FTS5 enabled ---
# Own target so the strict nkido warning flags never touch sqlite3.c.
add_library(catalog_sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(catalog_sqlite3 SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
)
target_compile_definitions(catalog_sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5=1
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS=1
)
find_package(Threads REQUIRED)
target_link_libraries(catalog_sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(catalog_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- the catalog library ---
add_library(catalog STATIC
    src/corpus.cpp
    src/database.cpp
    src/probe.cpp
    src/scanner.cpp
)
add_library(nkido::catalog ALIAS catalog)

target_include_directories(catalog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# Compiler options via BUILD_INTERFACE to avoid export issues (cedar pattern).
target_link_libraries(catalog
    PRIVATE
        cedar
        catalog_sqlite3
        $<BUILD_INTERFACE:nkido_compiler_options>
)
target_compile_features(catalog PUBLIC cxx_std_20)

# --- tests ---
if(CATALOG_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# --- install ---
include(GNUInstallDirs)
install(TARGETS catalog catalog_sqlite3
    EXPORT catalogTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/catalog DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT catalogTargets
    FILE catalogTargets.cmake
    NAMESPACE catalog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/catalog
)
