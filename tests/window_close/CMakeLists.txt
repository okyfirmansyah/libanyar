cmake_minimum_required(VERSION 3.16)

add_executable(test_window_close main.cpp)
target_link_libraries(test_window_close PRIVATE anyar_core)
target_compile_features(test_window_close PRIVATE cxx_std_17)

add_test(
    NAME test_window_close
    COMMAND test_window_close
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_window_close>
)

set_tests_properties(test_window_close PROPERTIES
    TIMEOUT 15
    LABELS "shutdown;e2e"
)