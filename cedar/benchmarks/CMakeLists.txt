# cedar_bench — per-opcode microbenchmark (docs/prd-simd-autoresearch.md §5.3)
add_executable(cedar_bench bench_opcodes.cpp)
target_link_libraries(cedar_bench PRIVATE cedar $<BUILD_INTERFACE:nkido_compiler_options>)
set_target_properties(cedar_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(EMSCRIPTEN)
    # node: real filesystem for --dump, and a heap that fits the VM.
    target_link_options(cedar_bench PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1)
endif()
