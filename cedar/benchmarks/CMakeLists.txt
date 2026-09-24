# cedar_bench — per-opcode microbenchmark (docs/prd-simd-autoresearch.md §5.3)
add_executable(cedar_bench bench_opcodes.cpp)
target_link_libraries(cedar_bench PRIVATE cedar $<BUILD_INTERFACE:nkido_compiler_options>)
set_target_properties(cedar_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
