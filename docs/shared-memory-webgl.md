# Shared Memory IPC & WebGL Canvas — Developer Guide

> Zero-copy binary data transfer between C++ and the webview frontend.

## Overview

LibAnyar provides two complementary modules for high-throughput binary data:

| Module | Purpose |
|---|---|
| `@libanyar/api/buffer` | Create, write, fetch, and manage shared memory buffers |
| `@libanyar/api/canvas` | WebGL frame renderer consuming shared buffers (RGBA & YUV420) |

**Use cases:** Video frame streaming, image processing pipelines, LiDAR/point cloud visualization, real-time sensor data, screen capture overlays.

**How it works:**
1. C++ allocates a named shared memory region (`SharedBuffer`)
2. C++ writes binary data directly to the mmap'd memory
3. C++ emits a `buffer:ready` event to notify the frontend
4. JS fetches the data via `fetch('anyar-shm://<name>')` — zero-copy on Linux
5. JS renders the data using WebGL (`FrameRenderer`) or processes the `ArrayBuffer` directly

---

## C++ API

### SharedBuffer

A named shared memory region backed by POSIX `shm_open()` + `mmap()`.

```cpp
#include <anyar/shared_buffer.h>

// Create a 1920×1080 RGBA buffer (≈8MB)
auto buf = anyar::SharedBuffer::create("camera-frame", 1920 * 1080 * 4);

// Write data
std::memcpy(buf->data(), pixels, buf->size());

// Access properties
buf->name();    // "camera-frame"
buf->size();    // 8294400
buf->data();    // void* — raw pointer to mmap'd memory
buf->shm_url(); // "anyar-shm://camera-frame"

// Notify frontend that new data is available
app.emit("buffer:ready", {
    {"name", buf->name()},
    {"url", buf->shm_url()},
    {"width", 1920},
    {"height", 1080}
});

// Cleanup (automatic on destruction, or explicit)
buf.reset();  // calls munmap() + shm_unlink()
```

### SharedBufferRegistry

Singleton that tracks all active buffers. Used internally by IPC commands.

```cpp
auto& registry = anyar::SharedBufferRegistry::instance();

// Create via registry (same as SharedBuffer::create, but tracked)
auto buf = registry.create("frame", size);

// Lookup by name
auto found = registry.get("frame");

// List all active buffers
auto names = registry.list();  // std::vector<std::string>

// Destroy by name
registry.destroy("frame");     // munmap + shm_unlink + remove from registry
```

### SharedBufferPool

Lock-free ring buffer pool for streaming scenarios (e.g., 30fps video).

```cpp
// Create a pool of 3 slots, each 8MB (triple buffering)
anyar::SharedBufferPool pool("video", 3, 1920 * 1080 * 4);

// Producer side (C++ thread or fiber)
int slot = pool.acquire_write();   // Returns slot index, or -1 if none free
if (slot >= 0) {
    auto& buf = pool.slot_buffer(slot);
    std::memcpy(buf.data(), frame_data, buf.size());
    pool.release_write(slot);      // Transitions slot: WRITING → READY
}

// Consumer side (triggered by buffer:ready event in JS)
int slot = pool.acquire_read();    // Returns slot with READY state, or -1
if (slot >= 0) {
    // JS fetches anyar-shm://video_0, video_1, or video_2
    pool.release_read(slot);       // Transitions slot: READING → FREE
}
```

**Slot state machine:**
```
FREE ──acquire_write()──► WRITING ──release_write()──► READY
  ▲                                                      │
  │                                                      │
  └───release_read()──── READING ◄──acquire_read()───────┘
```

All transitions use `std::atomic::compare_exchange_strong` — no mutexes in the hot path.

---

## JavaScript API — `@libanyar/api/buffer`

### Import

```ts
import {
  createBuffer, writeBuffer, destroyBuffer, listBuffers,
  fetchBuffer, notifyBuffer, onBufferReady,
  createPool, destroyPool, poolAcquire, poolReleaseWrite, poolReleaseRead,
} from '@libanyar/api/buffer';
```

### Buffer Operations

#### `createBuffer(name, size)`
Create a named shared memory buffer on the C++ side.

```ts
await createBuffer('my-frame', 1920 * 1080 * 4);
```

**Parameters:**
| Param | Type | Description |
|---|---|---|
| `name` | `string` | Unique buffer name |
| `size` | `number` | Size in bytes |

**Returns:** `Promise<{ name: string, size: number, url: string }>`

---

#### `writeBuffer(name, data)`
Write binary data to an existing buffer (base64-encoded transfer via IPC).

```ts
const pixels = new Uint8Array(width * height * 4);
// ... fill pixels ...
await writeBuffer('my-frame', pixels);
```

> **Note:** For high-throughput scenarios, prefer writing from C++ directly via `memcpy` to avoid base64 encoding overhead.

---

#### `fetchBuffer(nameOrUrl)`
Fetch buffer contents via the `anyar-shm://` URI scheme. Returns raw binary data.

```ts
const data: ArrayBuffer = await fetchBuffer('my-frame');
// or with explicit URL:
const data = await fetchBuffer('anyar-shm://my-frame');

// Access as typed array
const pixels = new Uint8Array(data);
```

**Returns:** `Promise<ArrayBuffer>`

This uses `fetch('anyar-shm://<name>')` internally — zero-copy on Linux (WebKitGTK reads the mmap'd region directly via `g_bytes_new_static()`).

---

#### `destroyBuffer(name)`
Unmap and unlink a shared memory buffer.

```ts
await destroyBuffer('my-frame');
```

---

#### `listBuffers()`
List all active buffer names.

```ts
const buffers = await listBuffers();
// ["camera-frame", "depth-map", ...]
```

**Returns:** `Promise<string[]>`

---

#### `notifyBuffer(name, metadata?)`
Emit a `buffer:ready` event for a buffer (with optional metadata).

```ts
await notifyBuffer('my-frame', { width: 1920, height: 1080, format: 'rgba' });
```

---

#### `onBufferReady(handler)`
Listen for `buffer:ready` events.

```ts
const unlisten = onBufferReady((event) => {
  console.log(event.name);   // "my-frame"
  console.log(event.url);    // "anyar-shm://my-frame"
  console.log(event.width);  // 1920 (if provided by notifier)
});

// Stop listening
unlisten();
```

---

### Pool Operations

#### `createPool(name, slotCount, slotSize)`
Create a ring buffer pool for streaming.

```ts
await createPool('video', 3, 1920 * 1080 * 4);
```

---

#### `poolAcquire(poolName)`
Acquire a free slot for writing.

```ts
const result = await poolAcquire('video');
// { slot: 0, name: "video_0", url: "anyar-shm://video_0" }
```

**Returns:** `Promise<{ slot: number, name: string, url: string }>`

---

#### `poolReleaseWrite(poolName, slot)`
Release a slot after writing (transitions WRITING → READY, emits `buffer:ready`).

```ts
await poolReleaseWrite('video', 0);
```

---

#### `poolReleaseRead(poolName, slot)`
Release a slot after reading (transitions READING → FREE).

```ts
await poolReleaseRead('video', 0);
```

---

#### `destroyPool(poolName)`
Destroy a pool and all its shared memory slots.

```ts
await destroyPool('video');
```

---

## JavaScript API — `@libanyar/api/canvas`

### Import

```ts
import {
  FrameRenderer,
  createFrameRenderer,
  createBufferRenderer,
} from '@libanyar/api/canvas';
```

### `createFrameRenderer(options)`

Create a WebGL renderer attached to a canvas element.

```ts
const renderer = createFrameRenderer({
  canvas: document.getElementById('viewport'),  // or CSS selector '#viewport'
  width: 1920,
  height: 1080,
  format: 'rgba',  // 'rgba' or 'yuv420'
});

// Draw a frame from an ArrayBuffer
renderer.drawFrame(frameData);

// Resize (e.g., on window resize)
renderer.resize(1280, 720);

// Cleanup
renderer.destroy();
```

**Options:**

| Option | Type | Default | Description |
|---|---|---|---|
| `canvas` | `HTMLCanvasElement \| string` | — | Target canvas element or CSS selector |
| `width` | `number` | — | Frame width in pixels |
| `height` | `number` | — | Frame height in pixels |
| `format` | `PixelFormat` | `'rgba'` | Pixel format of input data |

**Supported formats:**

| Format | Bytes/pixel | Description |
|---|---|---|
| `'rgba'` | 4 | Standard RGBA — 4 channels, 8 bits each |
| `'rgb'` | 3 | RGB without alpha channel |
| `'bgra'` | 4 | BGRA byte order (common on Windows / Direct3D) — channel-swapped in shader |
| `'grayscale'` | 1 | Single luminance channel (depth maps, thermal) |
| `'yuv420'` | 1.5 | YUV 4:2:0 planar (Y + U + V planes) — BT.601 conversion in shader |
| `'nv12'` | 1.5 | YUV 4:2:0 semi-planar, UV interleaved (hardware decoders, VA-API, NVDEC) |
| `'nv21'` | 1.5 | YUV 4:2:0 semi-planar, VU interleaved (Android cameras) |

### `FrameRenderer` Methods

| Method | Description |
|---|---|
| `drawFrame(data: ArrayBuffer)` | Upload and render a frame |
| `resize(width, height)` | Update dimensions and viewport |
| `destroy()` | Release WebGL resources |

---

### `createBufferRenderer(options)`

Convenience function that combines shared memory fetching with WebGL rendering. Automatically listens for `buffer:ready` events and renders each new frame.

```ts
const { renderer, destroy } = createBufferRenderer({
  canvas: '#viewport',
  width: 1920,
  height: 1080,
  format: 'rgba',
  pool: 'video-frames',  // Optional: pool name for auto-release
});

// Rendering happens automatically on buffer:ready events
// Call destroy() to stop
destroy();
```

**Additional options:**

| Option | Type | Description |
|---|---|---|
| `pool` | `string` | If set, auto-calls `poolReleaseRead()` after each frame |
| `bufferName` | `string` | Filter events to only this buffer name |

---

## Complete Example — Video Frame Streaming

### C++ Side (Producer)

```cpp
#include <anyar/app.h>
#include <anyar/shared_buffer.h>
#include <cstring>

int main() {
    anyar::App app({.title = "Video Viewer", .width = 1280, .height = 720});

    // Create a triple-buffer pool
    anyar::SharedBufferPool pool("video", 3, 1280 * 720 * 4);

    // Simulate a frame producer (in practice: FFmpeg, camera, etc.)
    app.command("video:start", [&](const json& args) -> json {
        auto svc = app.service();
        svc->execute([&pool, &app]() {
            for (int frame = 0; frame < 300; ++frame) {  // 10s at 30fps
                int slot = pool.acquire_write();
                if (slot < 0) continue;  // All slots busy, drop frame

                auto& buf = pool.slot_buffer(slot);
                generate_frame(buf.data(), buf.size(), frame);  // Your renderer

                pool.release_write(slot);

                // Notify frontend
                app.emit("buffer:ready", {
                    {"name", pool.slot_name(slot)},
                    {"url", pool.slot_url(slot)},
                    {"slot", slot},
                    {"pool", "video"},
                    {"width", 1280}, {"height", 720}
                });

                // ~33ms per frame
                asyik::sleep_for(std::chrono::milliseconds(33));
            }
        });
        return {{"status", "started"}};
    });

    return app.run();
}
```

### JavaScript Side (Consumer)

```html
<canvas id="viewport" width="1280" height="720"></canvas>

<script type="module">
import { createBufferRenderer } from '@libanyar/api/canvas';
import { invoke } from '@libanyar/api';

// Start rendering — auto-fetches and displays each frame
const { destroy } = createBufferRenderer({
  canvas: '#viewport',
  width: 1280,
  height: 720,
  format: 'rgba',
  pool: 'video',
});

// Tell C++ to start producing frames
await invoke('video:start');
</script>
```

---

## Performance Notes

| Metric | Value | Notes |
|---|---|---|
| Buffer creation | ~0.1ms | One-time `shm_open()` + `mmap()` |
| Frame write (C++) | ~0.3ms (1080p RGBA) | `memcpy` to mmap'd memory |
| Frame fetch (JS) | ~0.05ms | `anyar-shm://` URI scheme, zero-copy via `g_bytes_new_static()` |
| Event notification | ~0.01ms | Native IPC push via `webview_eval()` |
| WebGL upload + render | ~0.5ms (1080p) | `texImage2D` + draw call |
| **Total pipeline** | **~1ms per frame** | C++ write → JS fetch → WebGL render |

### Comparison with Alternatives

| Approach | Latency | Copy Count | Max Throughput |
|---|---|---|---|
| Base64 via JSON IPC | ~15ms (1080p) | 3 copies | ~10fps |
| HTTP POST binary | ~5ms (1080p) | 2 copies | ~30fps |
| WebSocket binary | ~3ms (1080p) | 2 copies | ~60fps |
| **Shared Memory (LibAnyar)** | **~1ms (1080p)** | **0-1 copies** | **120fps+** |

---

## Platform Support

| Platform | Backend | Status |
|---|---|---|
| Linux | POSIX `shm_open()` + WebKitGTK `anyar-shm://` | ✅ Implemented |
| Windows | `CreateFileMapping` + WebView2 virtual host | 🔲 Planned |
| macOS | `shm_open()` + WKWebView URL scheme | 🔲 Planned |

---

## Testing

The shared memory IPC has comprehensive test coverage:

- **Unit tests** (`test_shared_buffer`) — 17 Catch2 test cases, 552 assertions covering SharedBuffer, Registry, and Pool
- **WebGL E2E test** (`test_webgl_canvas`) — Automated in-webview pixel verification: creates a 4×4 RGBA buffer with known pattern, renders via WebGL, reads back pixels with `gl.readPixels()`, verifies all 16 pixels match (±2 tolerance)

Run all tests:
```bash
cd build && ctest --output-on-failure
```

Run only shared memory tests:
```bash
ctest -R "shared_buffer|webgl"
```
