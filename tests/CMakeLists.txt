cmake_minimum_required(VERSION 3.16)

# ── Catch2 (bundled with LibAsyik) ───────────────────────────────────────────
set(CATCH2_INCLUDE_DIR "/usr/local/include/libasyik/external/test/catch/single_include")

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

# ── WebGL E2E Tests (requires display) ───────────────────────────────────────
add_subdirectory(webgl)

# ── Native window close regression (requires display) ───────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(window_close)
endif()

# ── Pinhole: lifecycle + unit tests ─────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    anyar_add_test(test_pinhole_linux
        $<TARGET_OBJECTS:catch2_main>
        test_pinhole_linux.cpp
    )
endif()
