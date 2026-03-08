cmake_minimum_required(VERSION 3.16)
project(hello_world LANGUAGES CXX)

add_executable(hello_world
    src-cpp/main.cpp
)

target_link_libraries(hello_world PRIVATE anyar_core)

# Copy the frontend dist to the build output directory
set(FRONTEND_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist)
if(EXISTS ${FRONTEND_DIST_DIR})
    add_custom_command(TARGET hello_world POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${FRONTEND_DIST_DIR}
            $<TARGET_FILE_DIR:hello_world>/dist
        COMMENT "Copying frontend dist → build/dist"
    )
endif()
