cmake_minimum_required(VERSION 3.16)
project(anatomy_atlas LANGUAGES CXX)

add_executable(anatomy_atlas
    src-cpp/main.cpp
    src-cpp/atlas_plugin.cpp
    src-cpp/atlas_pack.cpp
)
target_link_libraries(anatomy_atlas PRIVATE anyar_core)

# Decoding millions of triangles: keep it fast in Debug builds too.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src-cpp/atlas_pack.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()

# ── Anatomy data (BodyParts3D, CC BY 4.0 — see assets/bodyparts3d/README.md) ─
# The whole-body sample (12 mm, 2.8 MB) is always embedded.
set(ATLAS_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets/bodyparts3d)
include(CMakeRC)
cmrc_add_resource_library(anatomy_atlas_assets
    NAMESPACE atlas_assets
    WHENCE ${ATLAS_ASSETS}
    ${ATLAS_ASSETS}/sample.pack
    ${ATLAS_ASSETS}/MANIFEST.json
)
target_link_libraries(anatomy_atlas PRIVATE anatomy_atlas_assets)

# The full pack (90 MB, three LODs) is a GitHub Release asset, never in Git.
#   -DANYAR_FETCH_ANATOMY=ON          download it (SHA-256 checked)
#   -DANYAR_ANATOMY_PACK=/path/x.pack use a local copy instead
option(ANYAR_FETCH_ANATOMY "Download the full-body anatomy pack (90 MB)" OFF)
set(ANYAR_ANATOMY_PACK "" CACHE FILEPATH "Local full-body anatomy pack (overrides the download)")
set(ATLAS_PACK_URL
    "https://github.com/okyfirmansyah/libanyar/releases/download/anatomy-bodyparts3d-v1/bodyparts3d-full.pack")
set(ATLAS_PACK_SHA256 "c1af96b576d122a629deb32381a67cb8f5624e8abc532b833baa8480993dc2c5")

set(_atlas_pack "")
if(ANYAR_ANATOMY_PACK)
    set(_atlas_pack "${ANYAR_ANATOMY_PACK}")
elseif(ANYAR_FETCH_ANATOMY)
    set(_atlas_pack "${CMAKE_BINARY_DIR}/anatomy/bodyparts3d-full.pack")
    if(EXISTS "${_atlas_pack}")
        file(SHA256 "${_atlas_pack}" _have)
    endif()
    if(NOT _have STREQUAL ATLAS_PACK_SHA256)
        message(STATUS "Downloading the full-body anatomy pack (90 MB)…")
        file(DOWNLOAD "${ATLAS_PACK_URL}" "${_atlas_pack}"
            EXPECTED_HASH SHA256=${ATLAS_PACK_SHA256} SHOW_PROGRESS STATUS _st)
        file(DOWNLOAD "${ATLAS_PACK_URL}.manifest.json" "${_atlas_pack}.manifest.json" STATUS _st2)
        list(GET _st 0 _code)
        if(NOT _code EQUAL 0)
            message(WARNING "anatomy-atlas: pack download failed (${_st}); using the embedded sample")
            file(REMOVE "${_atlas_pack}")
            set(_atlas_pack "")
        endif()
    endif()
endif()

if(_atlas_pack)
    # Next to the executable as anatomy/bodyparts3d-full.pack (+ manifest).
    add_custom_command(TARGET anatomy_atlas POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:anatomy_atlas>/anatomy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_atlas_pack}"
                $<TARGET_FILE_DIR:anatomy_atlas>/anatomy/bodyparts3d-full.pack
        COMMENT "Copying full-body anatomy pack")
    if(EXISTS "${_atlas_pack}.manifest.json")
        add_custom_command(TARGET anatomy_atlas POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_atlas_pack}.manifest.json"
                    $<TARGET_FILE_DIR:anatomy_atlas>/anatomy/bodyparts3d-full.pack.manifest.json)
    endif()
    message(STATUS "anatomy-atlas: full-body pack ${_atlas_pack}")
else()
    message(STATUS "anatomy-atlas: embedded sample only (-DANYAR_FETCH_ANATOMY=ON for the full pack)")
endif()

# ── Frontend: embed into binary or copy to build dir ────────────────────────
set(FRONTEND_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist)

if(ANYAR_EMBED_FRONTEND AND EXISTS ${FRONTEND_DIST_DIR})
    include(AnyarEmbed)
    anyar_embed_frontend(anatomy_atlas "${FRONTEND_DIST_DIR}")
else()
    if(EXISTS ${FRONTEND_DIST_DIR})
        add_custom_command(TARGET anatomy_atlas POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${FRONTEND_DIST_DIR}
                $<TARGET_FILE_DIR:anatomy_atlas>/dist
            COMMENT "Copying frontend dist → build/dist"
        )
    endif()
endif()
