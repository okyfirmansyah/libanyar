# AnyarEmbed.cmake — Helper to embed frontend assets into a LibAnyar binary
#
# Usage:
#   include(AnyarEmbed)
#   anyar_embed_frontend(<target> <dist_directory>)
#
# This:
#   1. Globs all files in <dist_directory>
#   2. Creates a cmrc resource library with namespace "anyar_embedded"
#   3. Links the resource library to <target>
#   4. Adds ANYAR_EMBED_FRONTEND compile definition
#   5. Adds the cmrc include directory to the target

include(${CMAKE_CURRENT_LIST_DIR}/CMakeRC.cmake)

function(anyar_embed_frontend TARGET DIST_DIR)
    if(NOT EXISTS "${DIST_DIR}")
        message(WARNING "anyar_embed_frontend: dist directory not found: ${DIST_DIR}")
        message(WARNING "  Build the frontend first (npm run build), then re-run cmake.")
        return()
    endif()

    # Glob all files in the dist directory
    file(GLOB_RECURSE _embed_files
        RELATIVE "${DIST_DIR}"
        "${DIST_DIR}/*"
    )

    if(NOT _embed_files)
        message(WARNING "anyar_embed_frontend: no files found in ${DIST_DIR}")
        return()
    endif()

    # Build the list of absolute paths for cmrc
    set(_abs_files "")
    foreach(_f ${_embed_files})
        list(APPEND _abs_files "${DIST_DIR}/${_f}")
    endforeach()

    list(LENGTH _embed_files _file_count)
    message(STATUS "Embedding ${_file_count} frontend files from ${DIST_DIR}")

    # Create the resource library with fixed namespace "anyar_embedded"
    # WHENCE strips the DIST_DIR prefix so files are accessible as
    # "index.html", "assets/index-abc.js", etc.
    cmrc_add_resource_library(${TARGET}_embedded_resources
        NAMESPACE anyar_embedded
        WHENCE "${DIST_DIR}"
        ${_abs_files}
    )

    # Link and configure the target
    target_link_libraries(${TARGET} PRIVATE ${TARGET}_embedded_resources)
    target_compile_definitions(${TARGET} PRIVATE ANYAR_EMBED_FRONTEND)
endfunction()
