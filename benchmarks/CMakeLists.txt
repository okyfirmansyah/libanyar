cmake_minimum_required(VERSION 3.16)

# LibAnyar performance benchmark harness — see benchmarks/README.md.
# Build with CMAKE_BUILD_TYPE=Release for meaningful numbers.

add_executable(anyar_bench main.cpp)
target_link_libraries(anyar_bench PRIVATE anyar_core)
target_compile_features(anyar_bench PRIVATE cxx_std_17)

add_custom_command(TARGET anyar_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/dist
        $<TARGET_FILE_DIR:anyar_bench>/dist-bench
    COMMENT "Copying benchmark frontend → dist-bench"
)

# PROTOTYPE: WebProcess extension exposing zero-copy window.__anyar_shm_map().
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBKIT_WEBEXT REQUIRED webkit2gtk-web-extension-4.0)
add_library(anyar_shm_webext MODULE shm_webext/anyar_shm_webext.cpp)
target_include_directories(anyar_shm_webext PRIVATE ${WEBKIT_WEBEXT_INCLUDE_DIRS})
target_link_libraries(anyar_shm_webext PRIVATE ${WEBKIT_WEBEXT_LIBRARIES} rt)
set_target_properties(anyar_shm_webext PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:anyar_bench>/webext)
add_dependencies(anyar_bench anyar_shm_webext)
