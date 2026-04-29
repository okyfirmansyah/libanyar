cmake_minimum_required(VERSION 3.16)
project(pinhole_hello LANGUAGES CXX)

add_executable(pinhole_hello
    src-cpp/main.cpp
)

target_link_libraries(pinhole_hello PRIVATE anyar_core)

# ── Frontend: plain static HTML (no Vite build step needed) ─────────────────
# frontend/ contains index.html directly — use it as the dist.
set(FRONTEND_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
set(FRONTEND_DIST_DIR ${FRONTEND_DIR}/dist)

if(ANYAR_EMBED_FRONTEND AND EXISTS ${FRONTEND_DIST_DIR})
    include(AnyarEmbed)
    anyar_embed_frontend(pinhole_hello "${FRONTEND_DIST_DIR}")
elseif(ANYAR_EMBED_FRONTEND AND EXISTS ${FRONTEND_DIR}/index.html)
    include(AnyarEmbed)
    anyar_embed_frontend(pinhole_hello "${FRONTEND_DIR}")
else()
    # Prefer a built dist/ if present, otherwise copy the raw frontend/ dir.
    if(EXISTS ${FRONTEND_DIST_DIR})
        set(_SRC ${FRONTEND_DIST_DIR})
    elseif(EXISTS ${FRONTEND_DIR}/index.html)
        set(_SRC ${FRONTEND_DIR})
    endif()
    if(DEFINED _SRC)
        add_custom_command(TARGET pinhole_hello POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${_SRC}
                $<TARGET_FILE_DIR:pinhole_hello>/dist
            COMMENT "Copying pinhole-hello frontend → build/dist"
        )
    endif()
endif()
