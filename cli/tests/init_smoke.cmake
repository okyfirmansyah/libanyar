# CLI smoke test (cmake -P): `anyar init` produces a project whose
# CMakeLists.txt points at this libanyar tree with a CMake-safe path.
#
#   -DANYAR=<anyar executable>  -DWORK_DIR=<scratch dir>  -DLIBANYAR_ROOT=<repo>

macro(fail msg)
    message(FATAL_ERROR "cli_init_smoke: ${msg}")
endmacro()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(COMMAND "${ANYAR}" --version
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0)
    fail("--version failed (${rc})")
endif()
if(NOT out MATCHES "anyar")
    fail("--version printed: ${out}")
endif()

execute_process(COMMAND "${ANYAR}" build --help RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
    fail("build --help failed (${rc})")
endif()

# init runs from inside the libanyar tree, so the root is found by walking up.
execute_process(COMMAND "${ANYAR}" init smokeapp --template vanilla --no-install
                WORKING_DIRECTORY "${WORK_DIR}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    fail("init failed (${rc}): ${out} ${err}")
endif()

set(app "${WORK_DIR}/smokeapp")
foreach(f CMakeLists.txt src-cpp/main.cpp frontend/package.json frontend/index.html)
    if(NOT EXISTS "${app}/${f}")
        fail("missing ${f}")
    endif()
endforeach()
if(EXISTS "${app}/frontend/node_modules")
    fail("--no-install still ran npm install")
endif()

file(READ "${app}/CMakeLists.txt" cml)
if(NOT cml MATCHES "project\\(smokeapp")
    fail("project() name not set")
endif()

# LIBANYAR_DIR must be this repo, with forward slashes: a Windows path like
# C:\Users\... inside a CMake string is an invalid escape (\U).
if(NOT cml MATCHES "set\\(LIBANYAR_DIR \"([^\"]*)\"")
    fail("LIBANYAR_DIR not found")
endif()
set(dir "${CMAKE_MATCH_1}")
string(FIND "${dir}" "\\" backslash)
if(NOT backslash EQUAL -1)
    fail("LIBANYAR_DIR has backslashes: ${dir}")
endif()
get_filename_component(dir_real "${dir}" REALPATH)
get_filename_component(root_real "${LIBANYAR_ROOT}" REALPATH)
if(NOT dir_real STREQUAL root_real)
    fail("LIBANYAR_DIR=${dir}, expected ${LIBANYAR_ROOT}")
endif()

# Same for the @libanyar/api alias in vite.config (a JS string: backslashes
# would be eaten as escapes).
file(GLOB vite_cfg "${app}/frontend/vite.config.*")
if(NOT vite_cfg)
    fail("no frontend/vite.config.*")
endif()
file(READ "${vite_cfg}" vite)
if(NOT vite MATCHES "'([^']*)/js-bridge/src'")
    fail("@libanyar/api alias not found in ${vite_cfg}")
endif()
get_filename_component(alias_real "${CMAKE_MATCH_1}" REALPATH)
if(NOT alias_real STREQUAL root_real)
    fail("vite alias root ${CMAKE_MATCH_1}, expected ${LIBANYAR_ROOT}")
endif()

message(STATUS "cli_init_smoke: OK")
