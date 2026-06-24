cmake_minimum_required(VERSION 3.16)
project(Vora-LSP LANGUAGES CXX)

# Tell MSVC to treat source files as UTF-8 (eliminates C4819 code page warnings
# on systems where the default locale uses a narrow code page like 936/GBK).
# Without this, string literals containing Unicode characters like → (U+2192)
# get misinterpreted and appear garbled in hover/completion text in VS Code.
# /utf-8 sets both source and execution character sets to UTF-8.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ──────────────────────────────────────────────────
# Paths to the Vora language core
# ──────────────────────────────────────────────────
# Vora must be built first (cmake --build <vora-build-dir>).
# The vora_lib static library and all headers live there.

set(VORA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Vora"
    CACHE PATH "Path to the Vora repository root")
set(VORA_BUILD "${VORA_ROOT}/build/windows-x64"
    CACHE PATH "Path to the Vora build directory (containing vora_lib)")

# ──────────────────────────────────────────────────
# Locate vora_lib
# ──────────────────────────────────────────────────
# MSVC puts the static lib in <config>/vora_lib.lib;
# Ninja/Make put it directly in the build dir.
if(MSVC)
    find_library(VORA_LIB vora_lib
        PATHS
            "${VORA_BUILD}/Debug"
            "${VORA_BUILD}/Release"
        NO_DEFAULT_PATH
    )
else()
    find_library(VORA_LIB vora_lib
        PATHS "${VORA_BUILD}"
        NO_DEFAULT_PATH
    )
endif()

if(NOT VORA_LIB)
    message(FATAL_ERROR
        "vora_lib not found in ${VORA_BUILD}. "
        "Build Vora first: cmake --build ${VORA_BUILD}")
endif()

message(STATUS "Found vora_lib: ${VORA_LIB}")

# ──────────────────────────────────────────────────
# LSP server executable
# ──────────────────────────────────────────────────
add_executable(vora-lsp
    server/main.cpp
    server/lsp_server.cpp
    server/lsp_server.h
)

target_include_directories(vora-lsp PRIVATE
    "${VORA_ROOT}/src"
    "${VORA_ROOT}/include"
    server
)

target_link_libraries(vora-lsp PRIVATE "${VORA_LIB}")

target_compile_features(vora-lsp PRIVATE cxx_std_17)

# ──────────────────────────────────────────────────
# Copy binary to repo root for VS Code extension
# ──────────────────────────────────────────────────
add_custom_command(TARGET vora-lsp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:vora-lsp>"
        "${CMAKE_CURRENT_SOURCE_DIR}/$<TARGET_FILE_NAME:vora-lsp>"
    COMMENT "Copying vora-lsp binary to repo root for VS Code extension"
)
