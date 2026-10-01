# tests/ — Catch2 C++ Test Suite

## Purpose
Unit + integration tests for `anyar_core`. Uses the **Catch2 single-header bundled with LibAsyik** (no extra dependency). WebGL E2E lives under `tests/webgl/`.

## Layout

```
tests/
├── CMakeLists.txt               # anyar_add_test() helper + catch2_main OBJECT lib
├── test_main.cpp                # Catch2 main (CATCH_CONFIG_MAIN)
├── test_port.h                  # anyar_test::free_port() — OS-chosen port; NEVER pick random ports (Windows-reserved ranges → bind throws in the fiber → run() hangs)
├── test_command_registry.cpp    # incl. add_async replying later from a thread/fiber
├── test_event_bus.cpp           # 18 cases including per-window sinks
├── test_types.cpp
├── test_fs_plugin.cpp
├── test_shell_plugin.cpp        # POSIX utils on Linux, cmd.exe builtins on Windows
├── test_shared_buffer.cpp       # 24 cases incl. close(), try_acquire_write, release_unpublished, stale release_read, buffer() lifetime
├── test_task.cpp                # run_blocking (off-thread, exceptions, fibers keep running) + BackgroundTask stop/join/restart
├── test_frame_mailbox.cpp       # pixel_format sizes (odd 4:2:0), recycling, consumer pinning, tear-free stress
├── test_http_file.cpp           # Range parsing + serve_file 200/206/416/404 over real HTTP
├── test_integration.cpp         # IpcRouter + DbPlugin + headless App
├── test_pinhole_linux.cpp       # Linux only; [pinhole][headless] + display-gated lifecycle/fallback cases
├── webgl/                       # Linux only (anyar-shm://); E2E WebGL pixel verification under xvfb
│   ├── main.cpp                 # 5s _exit() watchdog safety net
│   ├── dist/index.html
│   └── CMakeLists.txt
├── pinhole_win32/               # Windows only; DComp/D3D11 pinholes checked via PrintWindow(PW_RENDERFULLCONTENT) pixels: position×DPI, z-order/set_z_index, set_visible, set_rect move, force_fallback canvas; LABELS display;pinhole;e2e
├── native_ipc/                  # Windows only for now (platform-neutral code); generated page drives __anyar_ipc__ round-trip, event push, buffer fetch, UI-thread hop, window:close-all; LABELS display;ipc;e2e
├── early_close/                 # Linux + Windows; window:close-all posted (from an on_ready fiber) while the main window is still being created → app.run() must return (20 s watchdog)
└── window_close/                # Linux + Windows; plain exe (no Catch2): native close (gtk_window_close / WM_CLOSE) → app.run() must return
    ├── main.cpp                 # watchdog: FAIL if run() not back 6 s after close (_Exit 3) or no close by 12 s (_Exit 4); ctest TIMEOUT 15, LABELS display;shutdown;e2e
    └── CMakeLists.txt
```

## Testability Tiers
| Tier | What | Components |
|---|---|---|
| 1 | Pure unit, no service/GTK | CommandRegistry, EventBus, IPC types, FsPlugin |
| 2 | Lightweight side effects | ShellPlugin (real fork/exec or CreateProcess, temp files) |
| 3 | Needs LibAsyik service/fiber | IpcRouter, DbPlugin, App headless |
| 4 | Needs display | Window, Pinhole lifecycle, Dialog, Clipboard, WebGL E2E, window_close, early_close, native_ipc (CI: xvfb on Linux; Windows CI runs `-LE display`) |

## Conventions
- File: `test_<area>.cpp`; tag every `TEST_CASE` like `[area]`
- Each `TEST_CASE` exercises ONE responsibility; use `SECTION` for variants
- Temp files via `std::filesystem::temp_directory_path()` + RAII cleanup (no `/tmp`, `getpid()`, `/proc` — tests build on Windows)
- Servers: `anyar_test::free_port()` from `test_port.h`
- Tier-3 tests: build a fresh `asyik::service` per test case, run it on a thread, dispatch via `service_->execute()`

## Adding a Test

```cpp
#include <catch2/catch.hpp>
#include <anyar/command_registry.h>

TEST_CASE("CommandRegistry handles unknown", "[command-registry]") {
    SECTION("dispatch returns error") {
        anyar::CommandRegistry r;
        auto resp = r.dispatch("missing", {});
        REQUIRE(resp.contains("error"));
    }
}
```

Wire into `tests/CMakeLists.txt` (helper links `anyar_core`, adds Catch2 include dir, C++17, `add_test`):
```cmake
anyar_add_test(test_my_feature
    $<TARGET_OBJECTS:catch2_main>
    test_my_feature.cpp
)
```
Linux-only tests: wrap in `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`. Non-Catch2 E2E exes go in a subdir with their own `add_test` (see `window_close/`).

## Running
```bash
cd build
cmake .. -DANYAR_BUILD_TESTS=ON
make -j$(nproc)
ctest --output-on-failure
```

`run.sh` clears snap GTK env before invoking — required when host has snap-installed VS Code etc.

## WebGL E2E (CI)
- Runs under `xvfb-run` (CircleCI Ubuntu 22.04)
- Has a **5-second `_exit(0)` watchdog thread** as safety net for shutdown races (see ADR-007)
- Validates: SharedBuffer create → C++ writes pixels → JS fetch via `anyar-shm://` → WebGL render → `readPixels` matches expected

## Don'ts
- Never use `while(g_main_context_pending())` unbounded — cap at 200 iterations (xvfb generates infinite events during teardown)
- Don't share `asyik::service` instances across `TEST_CASE`s — construct fresh per case
- Don't assume display is present — guard tier-4 tests on `DISPLAY` / `ANYAR_HAS_DISPLAY` and `WARN(...)` + return when absent (see `test_pinhole_linux.cpp`); pinhole tests must also accept `is_native()==false` (GL may fail under xvfb)
- Any test binary that constructs a `Window` (even unshown) is tier 4: give it the ctest label `display` (`set_tests_properties(... LABELS "display;...")`). CI runs `ctest -LE display` headless and `ctest -L display` under `xvfb-run`; an unlabelled window test fails the headless step. Locally, snap-VS-Code GTK env vars make webview tests SIGTRAP — clear them as `run.sh` does.
- Don't add sleeps to wait for fibers; use synchronization primitives (`std::promise`, `std::condition_variable`)
- Every fiber you spawn must have exited before `svc->stop()` — a fiber still sleeping at stop makes process exit spin forever (tests pass, then ctest times out)
- ASAN: `-DANYAR_ASAN=ON`; suppress the Boost.Fiber false positive with a file containing `interceptor_name:sigaltstack` (`ASAN_OPTIONS=detect_leaks=0:suppressions=<file>`)
