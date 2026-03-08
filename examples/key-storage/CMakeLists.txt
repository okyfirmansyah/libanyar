cmake_minimum_required(VERSION 3.16)
project(key_storage LANGUAGES CXX)

# ── Find SQLite3 via pkg-config ──────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

add_executable(key_storage
    src-cpp/main.cpp
    src-cpp/keystore_plugin.cpp
)

target_include_directories(key_storage PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src-cpp
    ${SQLITE3_INCLUDE_DIRS}
)

target_link_libraries(key_storage PRIVATE
    anyar_core
    ${SQLITE3_LIBRARIES}
)

# Copy the frontend dist to the build output directory
set(FRONTEND_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist)
if(EXISTS ${FRONTEND_DIST_DIR})
    add_custom_command(TARGET key_storage POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${FRONTEND_DIST_DIR}
            $<TARGET_FILE_DIR:key_storage>/dist
        COMMENT "Copying key-storage frontend dist → build/dist"
    )
endif()
