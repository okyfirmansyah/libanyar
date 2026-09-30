cmake_minimum_required(VERSION 3.16)

add_executable(test_early_close main.cpp)
target_link_libraries(test_early_close PRIVATE anyar_core)
target_compile_features(test_early_close PRIVATE cxx_std_17)

add_test(
    NAME test_early_close
    COMMAND test_early_close
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_early_close>
)

set_tests_properties(test_early_close PROPERTIES
    TIMEOUT 30
    LABELS "display;shutdown;e2e"
)
