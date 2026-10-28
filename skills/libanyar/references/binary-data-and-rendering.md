# Binary data and rendering

> Check signatures against `core/include/anyar/shared_buffer.h`, `pinhole.h` and `frame_mailbox.h`. Some older prose examples in `docs/shared-memory-webgl.md` use APIs that have since been removed (`slot_buffer`, `registry.create`, `shm_url`); the headers and the examples (`examples/video-player`, `examples/anatomy-atlas`) are current.

## Pick the transport

| Data | Use | Why |
|---|---|---|
| Small JSON (< ~100 KB) | command result / event | simplest |
| One large blob (mesh, image, table, file contents) | `SharedBuffer` + `fetchBuffer` | zero-copy on the C++ side; no JSON/base64 |
| A stream of frames to draw in WebGL | `SharedBufferPool` + `buffer:ready` + `createBufferRenderer` / your own WebGL | ring of N slots with back-pressure |
| Video-rate pixels where latency matters most | **Pinhole** native overlay + `FrameMailbox` | drawn by GL/D3D under a transparent webview region; no transfer into JS at all |
| A media file for `<video>`/`<audio>` | `anyar::serve_file` HTTP route | Range requests; streaming |

Transport per platform:
- **Linux:** `fetchBuffer` reads `anyar-shm://` (POSIX shm).
- **Windows:** a WebView2 shared buffer (zero-copy) once a window exists.
- **Plain browser (dev):** HTTP `/__anyar__/buffer/<name>`.

The JS API is the same everywhere.

## One-off buffer (pattern from the anatomy atlas)

```cpp
#include <anyar/shared_buffer.h>

// In a plugin command, after building the data on the worker pool:
const std::string name = "mesh-" + std::to_string(++seq_);       // names are process-global
auto buf = anyar::SharedBuffer::create(name, bytes);             // registers itself
std::memcpy(buf->data(), positions.data(), pos_bytes);           // … pack several arrays at known offsets
{ std::lock_guard<std::mutex> l(mu_); buffers_[name] = buf; }    // own it (see shutdown rule)
return {{"buffer", name}, {"id", buf->id()}, {"bytes", bytes},
        {"layout", {{"positions", 0}, {"indices", idx_off}}}};
```

```ts
import { invoke } from '@libanyar/api';
import { fetchBuffer } from '@libanyar/api/buffer';
const meta = await invoke<Meta>('mesh:build', { lod: 1 });
const bytes = await fetchBuffer(meta.buffer, { id: meta.id });   // ArrayBuffer (a private copy)
const positions = new Float32Array(bytes, meta.layout.positions, meta.vertices * 3);
void invoke('mesh:release', { buffer: meta.buffer });              // let C++ free it
```

The release command does: lock → `buffers_.erase(name)` → `anyar::SharedBufferRegistry::instance().remove(name)`. `shutdown()` removes whatever is left, **under the same lock**. The page can still send release calls while the app shuts down.

Align offsets to 4 bytes so typed-array views work. Keep a 16-bit/32-bit choice explicit in the metadata.

## Streaming pool (pattern from video-player)

```cpp
// One pool per session, with a unique base name (names are process-global).
pool_ = std::make_unique<anyar::SharedBufferPool>("frames-" + std::to_string(++gen_), frame_bytes, 4);

// Producer (in a BackgroundTask loop):
anyar::SharedBuffer* buf = pool_->try_acquire_write();   // nullptr = UI is behind → drop the frame
if (!buf) continue;
std::memcpy(buf->data(), frame.data(), frame_bytes);
pool_->release_write(*buf, "{}");
events_->emit("buffer:ready", {
    {"name", buf->name()}, {"pool", pool_->base_name()}, {"id", buf->id()},
    {"url", "anyar-shm://" + buf->name()}, {"size", frame_bytes},
    {"metadata", {{"width", w}, {"height", h}, {"format", "rgba"}}}});

// Consumer release command (plugin-owned pool):
ctx.commands.add("video:frame-done", [this](const anyar::json& a) -> anyar::json {
    pool_->release_read(a.at("name").get<std::string>());
    return {{"ok", true}};
});
// shutdown(): task.stop(); pool_->close();   // close() unblocks a producer stuck in acquire_write()
```

```ts
import { onBufferReady, fetchBuffer } from '@libanyar/api/buffer';
onBufferReady(async (e) => {
  try {
    renderer.drawFrame(await fetchBuffer(e.name, { copy: false, id: e.id }));  // live view where supported
  } finally {
    await invoke('video:frame-done', { name: e.name });                         // always release
  }
});
```

Prefer `try_acquire_write()` for live sources (drop rather than stall). Use the blocking `acquire_write()` only when every item must arrive, and then make sure `close()` can unblock it.

`createBufferRenderer({ canvas, width, height, format, pool })` from `@libanyar/api/canvas` wires `buffer:ready` → WebGL draw → release for app-level pools. Its formats are `rgba`, `rgb`, `bgra`, `grayscale`, `yuv420`, `nv12`, `nv21`.

## Pinhole native overlay

Pinhole puts a native GL (Linux) or D3D11/DirectComposition (Windows) surface *under* a transparent rectangle of the page. Use it for video or camera feeds at full frame rate with no copy into JS. It is not available on macOS yet.

```cpp
auto mailbox = std::make_shared<anyar::FrameMailbox>();          // <anyar/frame_mailbox.h>
std::shared_ptr<anyar::Pinhole> pin;
app.on_window_ready([&](anyar::Window& win) {                    // UI thread, before the loop
    anyar::PinholeOptions o; o.format = anyar::pixel_format::yuv420;
    pin = win.create_pinhole("video", o);
    pin->on_render([mailbox](anyar::PinholeRenderContext& ctx) {
        ctx.clear(0, 0, 0, 1);
        if (auto f = mailbox->latest()) ctx.draw_frame(*f);      // letterboxed
    });
});
// Producer, any thread:
auto f = mailbox->acquire(bytes); /* fill f->data, width, height, format, pts */
mailbox->publish(f); pin->request_redraw();
```
```html
<div data-anyar-pinhole="video" style="width:640px;height:360px"></div>
<!-- anything layered above it must be transparent -->
```

Things to know:
- `pin->is_native() == false` means GL/D3D was unavailable, and LibAnyar falls back to a slower canvas-2D path automatically.
- Not supported on the placeholder: CSS transforms, opacity/filters and border-radius.
- Debug screenshots (`debug:screenshot`) do not include Pinhole surfaces.

## WebGL tips that bit real apps

- One merged mesh per logical group plus a per-vertex id attribute scales far better than one mesh per object. For per-object state (visible/selected), use a small data texture rather than per-object materials.
- WebKitGTK and WebView2 both support WebGL2. Under Xvfb or in CI, WebGL runs on software rendering, so it is slow but correct.
- Measure FPS on the target GPU class (integrated graphics), not only on a workstation.
