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
    add_subdirectory(examples/video-player)
    add_subdirectory(examples/key-storage)
    add_subdirectory(examples/wifi-analyzer)
endif()

# ── CLI Tool ─────────────────────────────────────────────────────────────────
if(ANYAR_BUILD_CLI)
    add_subdirectory(cli)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(ANYAR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
