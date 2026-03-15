cmake_minimum_required(VERSION 3.16)
project(hello_world LANGUAGES CXX)

add_executable(hello_world
    src-cpp/main.cpp
)

target_link_libraries(hello_world PRIVATE anyar_core)

# ── Frontend: embed into binary or copy to build dir ────────────────────────
set(FRONTEND_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist)

if(ANYAR_EMBED_FRONTEND AND EXISTS ${FRONTEND_DIST_DIR})
    include(AnyarEmbed)
    anyar_embed_frontend(hello_world "${FRONTEND_DIST_DIR}")
else()
    # Copy frontend dist to build output directory at build time
    if(EXISTS ${FRONTEND_DIST_DIR})
        add_custom_command(TARGET hello_world POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${FRONTEND_DIST_DIR}
                $<TARGET_FILE_DIR:hello_world>/dist
            COMMENT "Copying frontend dist → build/dist"
        )
    endif()
endif()
