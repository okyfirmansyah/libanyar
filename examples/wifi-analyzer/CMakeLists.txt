cmake_minimum_required(VERSION 3.16)
project(wifi_analyzer LANGUAGES CXX)

# ── Find libnl via pkg-config ────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(NL3     REQUIRED libnl-3.0)
pkg_check_modules(NLGENL3 REQUIRED libnl-genl-3.0)

add_executable(wifi_analyzer
    src-cpp/main.cpp
    src-cpp/wifi_plugin.cpp
)

target_include_directories(wifi_analyzer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src-cpp
    ${NL3_INCLUDE_DIRS}
    ${NLGENL3_INCLUDE_DIRS}
)

target_link_libraries(wifi_analyzer PRIVATE
    anyar_core
    ${NL3_LIBRARIES}
    ${NLGENL3_LIBRARIES}
)

# Copy the frontend dist to the build output directory
set(FRONTEND_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist)
if(EXISTS ${FRONTEND_DIST_DIR})
    add_custom_command(TARGET wifi_analyzer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${FRONTEND_DIST_DIR}
            $<TARGET_FILE_DIR:wifi_analyzer>/dist
        COMMENT "Copying wifi-analyzer frontend dist → build/dist"
    )
endif()
