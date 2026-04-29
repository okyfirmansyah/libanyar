# examples/ — Example LibAnyar Apps

Reference implementations. Each is a standalone CMake project consumed by the root via `add_subdirectory`.

## Examples
| Dir | Demonstrates |
|---|---|
| `hello-world/` | Minimal — IPC commands, events, fs/dialog/shell |
| `key-storage/` | SQLite + Svelte + AES-256-GCM + multi-window modal + custom plugin |
| `video-player/` | FFmpeg + SharedBufferPool + WebGL canvas (RGBA / YUV420) |
| `wifi-analyzer/` | libnl Wi-Fi scan + WebGL real-time heatmap |

## Standard Layout
```
examples/<name>/
  CMakeLists.txt
  README.md           # what this demonstrates + run steps
  src-cpp/main.cpp    # C++ backend
  frontend/           # Vite (Svelte/React/vanilla)
                      # vite.config alias '@libanyar/api' → '../../js-bridge/src'
```

## Conventions
- Frontend default: **Svelte 5 + TS + Tailwind CSS 4** (preferred); React or vanilla acceptable
- Dark theme via CSS custom properties — keep visual consistency across examples
- C++ entry uses `#ifdef ANYAR_EMBED_FRONTEND` to switch between cmrc resolver and `dist/` filesystem
- All examples build green via root `cmake --build build`

## Build
```bash
cd examples/hello-world/frontend
npm install && npm run build
cd ../../../build && cmake --build . -j
./examples/hello-world/hello_world
# or: ./run.sh examples/hello-world/hello_world  (clears snap GTK env)
```

## C++ Backend Pattern
```cpp
#include <anyar/app.h>
int main() {
    anyar::App app;
    app.http_get("/api/health", [](auto req, auto){
        req->response.body = "{\"status\":\"ok\"}";
        req->response.headers.set("Content-Type", "application/json");
    });
    app.command("greet", [](const json& a) -> json {
        return {{"message", "Hello, " + a.value("name", "world")}};
    });
    // app.allow_file_access("/home/user/Pictures");  // for anyar-file://
    app.on_ready([]{ /* server up */ });
    app.create_window({.title = "My App", .width = 1024, .height = 768});
    return app.run();
}
```

## Adding an Example
1. `examples/my-app/{CMakeLists.txt, README.md, src-cpp/main.cpp, frontend/}`
2. `npm create vite@latest`, install deps, configure Vite alias to `@libanyar/api`
3. `add_subdirectory(examples/my-app)` to root `CMakeLists.txt`
4. Verify embedded build: `anyar build --embed`
5. README.md: explain demonstration, build steps, expected behavior

## Theme Tokens
Dark palette: violet/indigo accents `#7c3aed` / `#4f46e5`; neutrals `#0f172a` / `#1e293b` / `#94a3b8`.

## Multi-Window
For modal child windows: `createWindow({label, parent:'main', modal:true, url:'/#/route'})`. Send results back via `emitTo('main','event',payload)` then `closeWindow()`.

## SharedBuffer
```cpp
auto buf = anyar::SharedBuffer::create("frame", w*h*4);
std::memcpy(buf->data(), pixels, buf->size());
app.emit("buffer:ready", {{"name","frame"},{"url","anyar-shm://frame"}});
```
JS: `createBufferRenderer({canvas:'#viewport', width, height, format:'rgba', pool:'frame'})`.
