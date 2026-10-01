cmake_minimum_required(VERSION 3.16)

# ── Catch2 (bundled with LibAsyik) ───────────────────────────────────────────
# Installed under <libasyik prefix>/include/libasyik/external/test/catch/...
# (/usr/local on Linux; CMAKE_PREFIX_PATH, e.g. build-deps/libasyik, on Windows).
find_path(CATCH2_INCLUDE_DIR catch2/catch.hpp
    PATHS /usr/local/include/libasyik/external/test/catch/single_include
    PATH_SUFFIXES libasyik/external/test/catch/single_include
)
if(NOT CATCH2_INCLUDE_DIR)
    message(FATAL_ERROR "Catch2 (catch2/catch.hpp) not found — it ships with "
        "the LibAsyik install; set CATCH2_INCLUDE_DIR if it lives elsewhere.")
endif()

# ── Helper: create a test executable linked to anyar_core + Catch2 ───────────
function(anyar_add_test TEST_NAME)
    add_executable(${TEST_NAME} ${ARGN})
    target_link_libraries(${TEST_NAME} PRIVATE anyar_core)
    target_include_directories(${TEST_NAME} PRIVATE ${CATCH2_INCLUDE_DIR})
    target_compile_features(${TEST_NAME} PRIVATE cxx_std_17)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# ── Test main (Catch2 runner) ────────────────────────────────────────────────
add_library(catch2_main OBJECT test_main.cpp)
target_include_directories(catch2_main PUBLIC ${CATCH2_INCLUDE_DIR})

# ── Unit Tests ───────────────────────────────────────────────────────────────

# Core: CommandRegistry
anyar_add_test(test_command_registry
    $<TARGET_OBJECTS:catch2_main>
    test_command_registry.cpp
)

# Core: EventBus
anyar_add_test(test_event_bus
    $<TARGET_OBJECTS:catch2_main>
    test_event_bus.cpp
)

# Core: IPC Types (IpcResponse, EventMessage)
anyar_add_test(test_types
    $<TARGET_OBJECTS:catch2_main>
    test_types.cpp
)

# Plugin: FsPlugin
anyar_add_test(test_fs_plugin
    $<TARGET_OBJECTS:catch2_main>
    test_fs_plugin.cpp
)

# Plugin: ShellPlugin
anyar_add_test(test_shell_plugin
    $<TARGET_OBJECTS:catch2_main>
    test_shell_plugin.cpp
)

# ── Integration Tests ────────────────────────────────────────────────────────

anyar_add_test(test_integration
    $<TARGET_OBJECTS:catch2_main>
    test_integration.cpp
)

# Core: SharedBuffer + SharedBufferPool + Buffer IPC commands
anyar_add_test(test_shared_buffer
    $<TARGET_OBJECTS:catch2_main>
    test_shared_buffer.cpp
)

# Core: run_blocking + BackgroundTask
anyar_add_test(test_task
    $<TARGET_OBJECTS:catch2_main>
    test_task.cpp
)

# Core: FrameMailbox + pixel_format helpers
anyar_add_test(test_frame_mailbox
    $<TARGET_OBJECTS:catch2_main>
    test_frame_mailbox.cpp
)

# Core: HTTP Range parsing + serve_file
anyar_add_test(test_http_file
    $<TARGET_OBJECTS:catch2_main>
    test_http_file.cpp
)

# ── WebGL E2E Tests (requires display) ───────────────────────────────────────
# Fetches frames via anyar-shm://, which WebView2 does not serve yet.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(webgl)
endif()

# ── Native window close regression (requires display) ───────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
    add_subdirectory(window_close)
    # window:close-all arriving while the main window is still being created
    add_subdirectory(early_close)
endif()

# ── Native IPC end-to-end through a real webview (requires display) ─────────
# Platform-neutral; enabled on Windows first — turn on for Linux once it has
# been verified under the CI xvfb job.
if(WIN32)
    add_subdirectory(native_ipc)
    # Pinhole (DComp + D3D11) pixel test via PrintWindow
    add_subdirectory(pinhole_win32)
endif()

# ── Pinhole: lifecycle + unit tests ─────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    anyar_add_test(test_pinhole_linux
        $<TARGET_OBJECTS:catch2_main>
        test_pinhole_linux.cpp
    )
    # Creates real GTK/WebKit windows → needs a display (CI: xvfb step).
    set_tests_properties(test_pinhole_linux PROPERTIES LABELS "display;pinhole")
endif()
