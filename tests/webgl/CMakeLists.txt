cmake_minimum_required(VERSION 3.16)

add_executable(test_webgl_canvas main.cpp)
target_link_libraries(test_webgl_canvas PRIVATE anyar_core)
target_compile_features(test_webgl_canvas PRIVATE cxx_std_17)

# Copy the test dist (HTML) to the build output directory
add_custom_command(TARGET test_webgl_canvas POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/dist
        $<TARGET_FILE_DIR:test_webgl_canvas>/dist-webgl-test
    COMMENT "Copying WebGL test dist → build/dist-webgl-test"
)

# Register as a CTest test
# Note: This test requires a display (X11/Wayland). In headless CI,
# use xvfb-run or set the DISPLAY variable.
add_test(
    NAME test_webgl_canvas
    COMMAND test_webgl_canvas
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_webgl_canvas>
)

# Give it a generous timeout — WebGL init can be slow on software renderers
set_tests_properties(test_webgl_canvas PROPERTIES
    TIMEOUT 30
    LABELS "webgl;e2e"
)
