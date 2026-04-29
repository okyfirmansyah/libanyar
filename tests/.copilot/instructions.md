# tests/ — Catch2 C++ Test Suite

## Purpose
Unit + integration tests for `anyar_core`. Uses the **Catch2 single-header bundled with LibAsyik** (no extra dependency). WebGL E2E lives under `tests/webgl/`.

## Layout

```
tests/
├── CMakeLists.txt
├── test_main.cpp                # Catch2 main
├── test_command_registry.cpp
├── test_event_bus.cpp           # 18 cases including per-window sinks
├── test_types.cpp
├── test_fs_plugin.cpp
├── test_shell_plugin.cpp
├── test_shared_buffer.cpp       # 17+ cases, 800+ assertions
├── test_integration.cpp         # IpcRouter + DbPlugin + headless App
└── webgl/                       # E2E WebGL pixel verification under xvfb
    ├── main.cpp                 # 5s _exit() watchdog safety net
    ├── dist/index.html
    └── CMakeLists.txt
```

## Testability Tiers
| Tier | What | Components |
|---|---|---|
| 1 | Pure unit, no service/GTK | CommandRegistry, EventBus, IPC types, FsPlugin |
| 2 | Lightweight side effects | ShellPlugin (real fork/exec, temp files) |
| 3 | Needs LibAsyik service/fiber | IpcRouter, DbPlugin, App headless |
| 4 | Needs display | Window, Dialog, Clipboard, WebGL E2E (CI uses xvfb) |

## Conventions
- File: `test_<area>.cpp`; tag every `TEST_CASE` like `[area]`
- Each `TEST_CASE` exercises ONE responsibility; use `SECTION` for variants
- Temp files via `std::filesystem::temp_directory_path()` + RAII cleanup
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

Wire into `tests/CMakeLists.txt`:
```cmake
add_executable(test_my_feature test_my_feature.cpp)
target_link_libraries(test_my_feature PRIVATE anyar_core Catch2::Catch2)
add_test(NAME test_my_feature COMMAND test_my_feature)
```

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
- Don't assume display is present — guard tier-4 tests with `if (!getenv("DISPLAY")) return;`
- Don't add sleeps to wait for fibers; use synchronization primitives (`std::promise`, `std::condition_variable`)
