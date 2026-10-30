cmake_minimum_required(VERSION 3.16)

# Debug tooling end to end (docs/debugging.md): console forwarding, injected
# test script + anyarTest, debug:input / debug:screenshot (where supported),
# debug:exit code.  Plain exe; needs a display.
add_executable(test_debug_tools main.cpp)
target_link_libraries(test_debug_tools PRIVATE anyar_core)
target_compile_features(test_debug_tools PRIVATE cxx_std_17)

add_test(
    NAME test_debug_tools
    COMMAND test_debug_tools
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_debug_tools>
)

set_tests_properties(test_debug_tools PROPERTIES
    TIMEOUT 60
    LABELS "display;debug;e2e"
)
