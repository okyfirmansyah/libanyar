cmake_minimum_required(VERSION 3.16)
project(anyar_cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(anyar
    src/main.cpp
    src/cmd_init.cpp
    src/cmd_dev.cpp
    src/cmd_build.cpp
    src/util.cpp
    src/templates.cpp
)

# Platform splits: process spawning + packaging formats
if(WIN32)
    target_sources(anyar PRIVATE
        src/process_win32.cpp
        src/package_win32.cpp     # portable zip
    )
else()
    target_sources(anyar PRIVATE
        src/process_posix.cpp
        src/cmd_package.cpp       # deb, AppImage (Linux)
    )
endif()

target_include_directories(anyar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# We need nlohmann/json for config parsing
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    PATHS /usr/local/include /usr/include
)
if(NLOHMANN_JSON_INCLUDE_DIR)
    target_include_directories(anyar PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

find_package(Threads REQUIRED)
target_link_libraries(anyar PRIVATE Threads::Threads)

# Filesystem library (GCC <9 needs explicit stdc++fs)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(anyar PRIVATE stdc++fs)
endif()

if(MSVC)
    # Sources contain UTF-8 glyphs (✓ ✗ → box drawing) printed to the console.
    target_compile_options(anyar PRIVATE /utf-8)
endif()

# Install the CLI binary
install(TARGETS anyar RUNTIME DESTINATION bin)

# ── Smoke test: `anyar init` generates a configurable project ───────────────
if(ANYAR_BUILD_TESTS)
    add_test(NAME cli_init_smoke
        COMMAND ${CMAKE_COMMAND}
            -DANYAR=$<TARGET_FILE:anyar>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-smoke
            -DLIBANYAR_ROOT=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/init_smoke.cmake)
    set_tests_properties(cli_init_smoke PROPERTIES TIMEOUT 60 LABELS "cli")
endif()
