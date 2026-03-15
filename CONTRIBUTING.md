# Contributing to LibAnyar

Thank you for your interest in contributing to LibAnyar! This document covers the development workflow, code conventions, and guidelines.

## Development Setup

### Prerequisites

- GCC 11+ (C++17 required)
- CMake ≥ 3.16
- Node.js ≥ 18
- [LibAsyik](https://github.com/okyfirmansyah/libasyik) 1.5+ installed

### Build from Source

```bash
git clone https://github.com/libanyar/libanyar.git
cd libanyar
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DANYAR_BUILD_TESTS=ON
make -j$(nproc)
```

### Run Tests

```bash
cd build
ctest --output-on-failure
```

## Project Structure

```
libanyar/
├── core/                    # C++ framework library
│   ├── include/anyar/       # Public headers
│   │   └── plugins/         # Built-in plugin headers
│   └── src/                 # Implementation
│       └── plugins/         # Plugin implementations
├── js-bridge/               # @libanyar/api TypeScript package
│   └── src/
│       └── modules/         # FS, dialog, shell, db, event modules
├── cli/                     # anyar CLI tool
│   └── src/
├── third_party/             # Vendored dependencies (webview)
├── examples/
│   ├── hello-world/
│   ├── video-player/
│   └── key-storage/
├── tests/                   # Catch2 unit tests
├── docs/                    # Guides and documentation
└── ARCHITECTURE.md          # System architecture overview
```

## Code Style

### C++

- **Standard:** C++17
- **Naming:** `snake_case` for functions/variables, `PascalCase` for classes, `UPPER_CASE` for constants
- **Indentation:** 4 spaces
- **Braces:** K&R style (opening brace on same line)
- **Headers:** Use `#pragma once`
- **Includes:** Group by: standard library → third-party → project headers
- **Documentation:** Use `///` Doxygen comments for all public APIs

```cpp
/// @brief Send a greeting message.
/// @param name The recipient name.
/// @return JSON with a "message" field.
json greet(const std::string& name) {
    return {{"message", "Hello, " + name + "!"}};
}
```

### TypeScript (js-bridge)

- **Style:** Standard TypeScript with JSDoc comments
- **Exports:** Named exports (no default exports)
- **Docs:** Use `@param`, `@returns`, `@example` JSDoc tags

### Frontend (Examples)

- **Framework:** Svelte 5 (preferred), React 18, or Vanilla JS
- **CSS:** Tailwind CSS 4 for Svelte/React; inline styles for Vanilla
- **Theme:** Dark theme with CSS custom properties

## Making Changes

### 1. Core Library Changes

Edit files under `core/include/anyar/` and `core/src/`. The library builds as a static library (`anyar_core`).

After changes:
```bash
cd build && make -j$(nproc)
ctest --output-on-failure
```

### 2. JS Bridge Changes

Edit files under `js-bridge/src/`.

Build the bridge:
```bash
cd js-bridge && npm run build
```

### 3. Adding a New Built-in Plugin

1. Create `core/include/anyar/plugins/my_plugin.h`
2. Create `core/src/plugins/my_plugin.cpp`
3. Add source to `core/CMakeLists.txt`
4. Register in `App::App()` constructor (see `core/src/app.cpp`)
5. Add JS module at `js-bridge/src/modules/my_plugin.ts`
6. Export from `js-bridge/src/index.ts`
7. Add tests under `tests/`
8. Document in the plugin header with Doxygen comments

### 4. Adding a New Example

1. Create `examples/my-example/` with:
   - `CMakeLists.txt` — build config
   - `src-cpp/main.cpp` — C++ backend
   - `frontend/` — Vite frontend project
   - `README.md` — what it demonstrates
2. Add `add_subdirectory(examples/my-example)` to root `CMakeLists.txt`
3. Build frontend: `cd frontend && npm install && npm run build`

## Commit Messages

Use conventional commit format:

```
feat: add clipboard:read command
fix: prevent dialog crash on empty filter
docs: add database integration guide
test: add FsPlugin readDir tests
refactor: simplify IPC router setup
```

## Testing

Tests use [Catch2](https://github.com/catchorg/Catch2) (bundled with LibAsyik).

### Adding Tests

Create a test file under `tests/`:

```cpp
#include <catch2/catch.hpp>
#include <anyar/command_registry.h>

TEST_CASE("My feature works", "[my-feature]") {
    SECTION("basic case") {
        // ...
        REQUIRE(result == expected);
    }
}
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(test_my_feature test_my_feature.cpp)
target_link_libraries(test_my_feature PRIVATE anyar_core Catch2::Catch2)
add_test(NAME test_my_feature COMMAND test_my_feature)
```

## Documentation

- **C++ API:** Doxygen comments in headers → `doxygen Doxyfile` generates HTML
- **JS API:** JSDoc/TypeDoc in `js-bridge/src/` → TypeDoc generates HTML
- **Guides:** Markdown files in `docs/`
- **Architecture:** See [ARCHITECTURE.md](ARCHITECTURE.md)

## Questions?

Open an issue on GitHub or start a discussion in the repository.
