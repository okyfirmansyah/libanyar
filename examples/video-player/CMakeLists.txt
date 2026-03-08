cmake_minimum_required(VERSION 3.16)
project(video_player LANGUAGES CXX)

# ── Find FFmpeg via pkg-config ───────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC  REQUIRED libavcodec)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(SWSCALE  REQUIRED libswscale)
pkg_check_modules(AVUTIL   REQUIRED libavutil)

add_executable(video_player
    src-cpp/main.cpp
    src-cpp/video_plugin.cpp
)

target_include_directories(video_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src-cpp
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)

target_link_libraries(video_player PRIVATE
    anyar_core
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
)

# Copy the frontend dist to the build output directory
set(FRONTEND_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist)
if(EXISTS ${FRONTEND_DIST_DIR})
    add_custom_command(TARGET video_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${FRONTEND_DIST_DIR}
            $<TARGET_FILE_DIR:video_player>/dist
        COMMENT "Copying video-player frontend dist → build/dist"
    )
endif()
