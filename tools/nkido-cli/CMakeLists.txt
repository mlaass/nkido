# Nkido CLI - Unified synth tool with SDL2 audio output

find_package(SDL2 REQUIRED)

add_executable(nkido-cli
    main.cpp
    audio_engine.cpp
    bytecode_loader.cpp
    bytecode_dump.cpp
    ui/bitmap_font.cpp
    ui/text_buffer.cpp
    ui/ui_mode.cpp
)

target_link_libraries(nkido-cli
    PRIVATE
        akkado
        cedar
        SDL2::SDL2
        nkido_compiler_options
)

target_include_directories(nkido-cli
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

# Platform-specific: SDL2main needed on Windows
if(WIN32)
    target_link_libraries(nkido-cli PRIVATE SDL2::SDL2main)
endif()

install(TARGETS nkido-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
