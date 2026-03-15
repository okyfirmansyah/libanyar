# Getting Started with LibAnyar

This guide walks you through installing LibAnyar's dependencies, building the framework, and creating your first application.

## Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| GCC / Clang | 11+ / 10+ | C++17 compiler |
| CMake | ≥ 3.16 | Build system |
| Node.js | ≥ 18 | Frontend tooling (Vite) |
| npm | ≥ 9 | Package manager |

### System Libraries (Ubuntu 22.04)

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake pkg-config \
    libwebkit2gtk-4.0-dev \
    libgtk-3-dev \
    libssl-dev \
    nlohmann-json3-dev
```

### LibAsyik

LibAnyar depends on [LibAsyik](https://github.com/okyfirmansyah/libasyik) 1.5+, which includes Boost 1.81+ and SOCI 4.0.3.

Install LibAsyik from source:

```bash
git clone https://github.com/okyfirmansyah/libasyik.git
cd libasyik
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

## Building LibAnyar

```bash
git clone https://github.com/libanyar/libanyar.git
cd libanyar
mkdir build && cd build

# Basic build (without database support)
cmake .. -DCMAKE_BUILD_TYPE=Debug

# With SQLite/PostgreSQL database support
cmake .. -DCMAKE_BUILD_TYPE=Debug -DANYAR_ENABLE_SOCI=ON

make -j$(nproc)
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ANYAR_ENABLE_SOCI` | `ON` | Enable SQLite/PostgreSQL database plugin |
| `ANYAR_BUILD_EXAMPLES` | `ON` | Build example applications |
| `ANYAR_BUILD_CLI` | `ON` | Build the `anyar` CLI tool |
| `ANYAR_BUILD_TESTS` | `OFF` | Build unit tests |

## Using the CLI (Recommended)

The easiest way to create a new project is with the `anyar` CLI:

```bash
# From the build directory after building:
./cli/anyar init myapp
cd myapp
```

The CLI will:
1. Ask for a project name and frontend template
2. Generate the project structure
3. Install frontend dependencies

Then start developing:

```bash
anyar dev     # Start Vite HMR + build/run C++ backend
anyar build   # Production build
anyar build --embed                            # Single-binary (frontend compiled in)
anyar build --package deb --version 1.0.0      # Build + create .deb
anyar build --package appimage --version 1.0.0 # Build + create AppImage
anyar build --package all --version 1.0.0      # Build + both formats
```

See [Building Your First App](building-first-app.md) for a step-by-step walkthrough.

## Manual Project Setup

If you prefer to set things up manually:

### 1. Project Structure

```
myapp/
├── CMakeLists.txt          # C++ build config
├── src-cpp/
│   └── main.cpp            # C++ backend entry point
└── frontend/
    ├── package.json         # Frontend dependencies
    ├── vite.config.js       # Vite configuration
    ├── index.html           # Entry HTML
    └── src/
        └── App.svelte       # Frontend app (Svelte/React/Vanilla)
```

### 2. CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(myapp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point to LibAnyar source tree
set(LIBANYAR_DIR "/path/to/libanyar" CACHE PATH "Path to libanyar source tree")
add_subdirectory(${LIBANYAR_DIR}/core anyar_core)

add_executable(myapp src-cpp/main.cpp)
target_link_libraries(myapp PRIVATE anyar_core)

# Copy frontend dist to build directory
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist)
    add_custom_command(TARGET myapp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist
            $<TARGET_FILE_DIR:myapp>/dist
        COMMENT "Copying frontend dist → build/dist"
    )
endif()
```

### 3. main.cpp

```cpp
#include <anyar/app.h>

int main() {
    anyar::AppConfig config;
    config.dist_path = "./dist";

    anyar::App app(config);

    // Register a command callable from the frontend
    app.command("greet", [](const json& args) -> json {
        std::string name = args.value("name", "World");
        return {{"message", "Hello, " + name + "!"}};
    });

    // Create a window
    anyar::WindowConfig win;
    win.title = "My App";
    win.width = 1024;
    win.height = 768;
    app.create_window(win);

    return app.run();
}
```

### 4. Frontend

```bash
cd frontend
npm install vite @libanyar/api   # or use the alias in vite.config.js
```

In your Vite config, alias `@libanyar/api` to the js-bridge source:

```js
// vite.config.js
import { defineConfig } from 'vite';
import path from 'path';

export default defineConfig({
  resolve: {
    alias: {
      '@libanyar/api': path.resolve(__dirname, '/path/to/libanyar/js-bridge/src'),
    },
  },
});
```

### 5. Build & Run

```bash
# Build frontend
cd frontend && npm run build && cd ..

# Build C++ backend
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# Run
./myapp
```

## Next Steps

- [Building Your First App](building-first-app.md) — Step-by-step tutorial
- [Writing Plugins](writing-plugins.md) — Extend your app with custom C++ plugins
- [Database Integration](database-integration.md) — SQLite and PostgreSQL guide
- [Architecture Overview](../ARCHITECTURE.md) — How LibAnyar works internally
