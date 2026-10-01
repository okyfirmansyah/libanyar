cmake_minimum_required(VERSION 3.16)
project(key_storage LANGUAGES CXX)

# ── SQLite3 ──────────────────────────────────────────────────────────────────
# CMake's FindSQLite3 (3.14+) finds libsqlite3-dev on Linux and the vcpkg
# `sqlite3` port on Windows.  OpenSSL comes in through anyar_core.
find_package(SQLite3 REQUIRED)

add_executable(key_storage
    src-cpp/main.cpp
    src-cpp/keystore_plugin.cpp
)

target_include_directories(key_storage PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src-cpp
)

target_link_libraries(key_storage PRIVATE
    anyar_core
    SQLite::SQLite3
)

# Copy the frontend dist to the build output directory
set(FRONTEND_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist)
if(ANYAR_EMBED_FRONTEND AND EXISTS ${FRONTEND_DIST_DIR})
    include(AnyarEmbed)
    anyar_embed_frontend(key_storage "${FRONTEND_DIST_DIR}")
else()
    if(EXISTS ${FRONTEND_DIST_DIR})
        add_custom_command(TARGET key_storage POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${FRONTEND_DIST_DIR}
                $<TARGET_FILE_DIR:key_storage>/dist
            COMMENT "Copying key-storage frontend dist → build/dist"
        )
    endif()
endif()
