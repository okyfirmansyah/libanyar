# examples/ — Example LibAnyar Apps

Reference implementations. Each is a standalone CMake project consumed by the root via `add_subdirectory`.

## Examples
| Dir | Demonstrates |
|---|---|
| `hello-world/` | Minimal — IPC commands, events, fs/dialog/shell |
| `pinhole-hello/` | Minimal Pinhole: `on_window_ready` → `create_pinhole` + `set_rect` + `set_continuous` render of `clear()`/`draw_image()`. Plain `frontend/index.html` (no Vite, no README) |
| `key-storage/` | SQLite + Svelte + AES-256-GCM + multi-window modal + custom plugin |
| `video-player/` | FFmpeg decode on the worker pool (`run_blocking`), decode loop = `BackgroundTask` session per file, audio-clock sync with acknowledged seeks (`video:seeked`), Range streaming via `serve_file` of an audio-only remux (the `<audio>` element never sees the video track), fresh `<audio>` pipeline per timeline seek + stall watchdog (WebKitGTK wedges on flushing seeks with high-latency audio devices), `VIDEO_PLAYER_DEBUG=1` trace, real-UI stress test `test/run_seek_stress.sh`. Default `--mode=pinhole` (`FrameMailbox` → `draw_frame`); `--mode=webgl` = per-session SharedBufferPool + `buffer:ready` + WebGL canvas |
| `wifi-analyzer/` | libnl Wi-Fi scan + WebGL real-time heatmap |

## Standard Layout (pinhole-hello is the deliberate exception)
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
Streaming pools: one pool per session with a unique base name, `try_acquire_write()` → fill → `release_write()` → emit `buffer:ready`; JS releases in `finally`.

## Pinhole (native overlay)
```cpp
auto mailbox = std::make_shared<anyar::FrameMailbox>();   // <anyar/frame_mailbox.h>
app.on_window_ready([&](anyar::Window& win){           // main thread, before GTK loop
    anyar::PinholeOptions o; o.format = anyar::pixel_format::yuv420;
    pin = win.create_pinhole("video", o);
    pin->on_render([mailbox](anyar::PinholeRenderContext& ctx){
        ctx.clear(0, 0, 0, 1);
        if (auto f = mailbox->latest()) ctx.draw_frame(*f);   // letterboxed; frame pinned while drawn
    });
});
// producer (any thread): auto f = mailbox->acquire(bytes); fill; mailbox->publish(f); pin->request_redraw();
// HTML: <div data-anyar-pinhole="video"></div> (keep content above it transparent)
```
Check `pin->is_native()` — false = canvas-2D fallback (slower). See docs/pinhole-rendering.md.

## Background Work & Shutdown
Blocking work (codecs, file I/O) → `anyar::run_blocking(service_, fn)`; long-lived loops → `anyar::BackgroundTask` (`<anyar/task.h>`), `task.stop()` in `shutdown()` and before restarting (e.g. on re-open) so sessions never overlap. Live producers use `SharedBufferPool::try_acquire_write()` (drop, don't stall). Serve media with streaming `anyar::serve_file(weak_server.lock(), req, path)` (full ranges, chunked). Never call `service_->stop()` yourself. Test by closing the window while busy. Refs: `VideoPlugin` (`start_playback`/`stop_playback`), `WifiPlugin::shutdown()`, docs/graceful-shutdown.md, ADR-009.
