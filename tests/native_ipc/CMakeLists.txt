cmake_minimum_required(VERSION 3.16)

add_executable(test_native_ipc main.cpp)
target_link_libraries(test_native_ipc PRIVATE anyar_core)
target_compile_features(test_native_ipc PRIVATE cxx_std_17)

add_test(
    NAME test_native_ipc
    COMMAND test_native_ipc
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_native_ipc>
)

set_tests_properties(test_native_ipc PROPERTIES
    TIMEOUT 30
    LABELS "display;ipc;e2e"
)
