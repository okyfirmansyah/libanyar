cmake_minimum_required(VERSION 3.16)
project(libanyar VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────────────────────────
option(ANYAR_ENABLE_SOCI    "Enable SOCI database support (SQLite + PostgreSQL)" ON)
option(ANYAR_BUILD_EXAMPLES "Build example applications" ON)
option(ANYAR_BUILD_CLI      "Build anyar CLI tool" ON)
option(ANYAR_BUILD_TESTS    "Build tests" OFF)
option(ANYAR_BUILD_BENCHMARKS "Build performance benchmark harness" OFF)
option(ANYAR_ASAN           "Build with AddressSanitizer (debug lifetime bugs)" OFF)

if(ANYAR_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()
option(ANYAR_EMBED_FRONTEND "Embed frontend assets into binaries (requires built frontend)" OFF)

# Make cmake/ modules available (CMakeRC, AnyarEmbed)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Point to LibAsyik source tree (for development, before `make install`)
# Usage: cmake .. -DLIBASYIK_SOURCE_DIR=/path/to/libasyik
set(LIBASYIK_SOURCE_DIR "" CACHE PATH "Path to libasyik source tree (optional, for dev builds)")

# ── Core library ─────────────────────────────────────────────────────────────
add_subdirectory(core)

# ── Examples ─────────────────────────────────────────────────────────────────
if(ANYAR_BUILD_EXAMPLES)
    add_subdirectory(examples/hello-world)
    add_subdirectory(examples/pinhole-hello)
    add_subdirectory(examples/video-player)   # skips itself on Windows without FFmpeg
    add_subdirectory(examples/key-storage)
    if(NOT WIN32)
        # Linux-only: wifi-analyzer (libnl / nl80211).
        add_subdirectory(examples/wifi-analyzer)
    endif()
endif()

# Enable CTest before any subdirectory registers tests (cli/ has one).
if(ANYAR_BUILD_TESTS)
    enable_testing()
endif()

# ── CLI Tool ─────────────────────────────────────────────────────────────────
if(ANYAR_BUILD_CLI)
    add_subdirectory(cli)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(ANYAR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ── Benchmarks ───────────────────────────────────────────────────────────────
if(ANYAR_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
