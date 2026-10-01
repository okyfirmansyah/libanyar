cmake_minimum_required(VERSION 3.16)

add_executable(test_pinhole_win32 main.cpp)
target_link_libraries(test_pinhole_win32 PRIVATE anyar_core gdi32)
target_compile_features(test_pinhole_win32 PRIVATE cxx_std_17)

add_test(
    NAME test_pinhole_win32
    COMMAND test_pinhole_win32
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_pinhole_win32>
)

# Creates a real WebView2 window and reads back composed pixels → needs an
# interactive desktop (CI runs `-LE display`).
set_tests_properties(test_pinhole_win32 PROPERTIES
    TIMEOUT 60
    LABELS "display;pinhole;e2e"
)
