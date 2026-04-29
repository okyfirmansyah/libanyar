# LibAnyar — Pinhole Native Overlay Rendering

> **Status (Phase 4g, complete on Linux):** native GtkGLArea overlay,
> JS-driven DOM rect tracking, full pixel-format matrix, multi-pinhole
> lifecycle, transparent canvas-2D fallback. Windows / macOS land in Phase 7.

A **Pinhole** reserves a DOM rectangle as a transparent placeholder and renders
a native GPU surface positioned to match that element, below the transparent
webview compositor output, **inside the same OS window**. C++ draws directly
to the native GL/Metal/D3D surface — no JS in the hot path, no SharedBuffer
copy, no `texImage2D` upload.

For an end-to-end working example see
[examples/video-player/](../examples/video-player/) (toggle with
`--mode=pinhole|webgl`) and the minimal
[examples/pinhole-hello/](../examples/pinhole-hello/).

## When to use which renderer

LibAnyar offers two parallel render paths. Pick the one that matches your
constraints:

| | **Pinhole** | **SharedBuffer + FrameRenderer** |
|---|---|---|
| Target surface | Native GL/Metal/D3D layer | DOM `<canvas>` via WebGL |
| JS in hot path | None | `fetch(anyar-shm://…)` + `texImage2D` |
| Typical latency, 1080p/60 | ~0.2 ms | ~1 ms |
| 4K real-time | OK | GPU-bound |
| `border-radius`, `transform`, `filter`, `opacity` | Not honored | Full DOM styling |
| Scroll / swim | Hides during scroll (configurable) | Always smooth |
| Platforms | Linux today; Win / macOS Phase 7 | All platforms |

Rule of thumb: pinhole for video / camera / chart streams where every ms
matters; SharedBuffer + FrameRenderer when the surface needs DOM CSS
or you target Windows / macOS today.

## Architecture

```
GtkWindow
  └── GtkOverlay                ← inserted by Window during webview create
        ├── GtkOverlay          ← pinhole layer
        │     ├── GtkEventBox   ← transparent filler/main child
        │     └── GtkGLArea     ← per-pinhole child, positioned from
        │                         [data-anyar-pinhole] DOM rect tracking
        └── WebKitWebView       ← transparent, drawn on top
```

The `GtkGLArea` uses OpenGL 3.3 core via **libepoxy** (a transitive
dependency of WebKitGTK — no extra packages required).

See [ADR-008](decisions.md#adr-008-pinhole-native-overlay-rendering-architecture)
for the full decision record, and
[ADR-007](decisions.md#adr-007-shutdown-sequence) §5b for the shutdown
ordering invariant.

## API quick start

```cpp
#include <anyar/pinhole.h>
#include <anyar/window.h>

// 1. Create a pinhole on a Window.  May be called any time after the
//    window is constructed; the JS tracking script is injected by Window.
anyar::PinholeOptions opts;
opts.format             = anyar::pixel_format::yuv420;
opts.continuous         = false;     // render only on request_redraw()
opts.show_during_scroll = false;     // default: hide during scroll
// opts.force_fallback  = true;      // testing only — forces canvas-2D path
auto pin = window->create_pinhole("video", opts);

// 2. Detect whether GL came up.  False → framework's built-in canvas-2D
//    fallback (SharedBuffer + injected <canvas>) takes over transparently.
if (!pin->is_native()) {
    std::cerr << "Pinhole using canvas-2D fallback (perf reduced)\n";
}

// 3. Register a render callback.  Runs on the GTK main thread (NOT a
//    LibAsyik fiber) with the GL context current.
pin->on_render([&](anyar::PinholeRenderContext& ctx) {
    ctx.clear(0.0f, 0.0f, 0.0f, 1.0f);
    ctx.draw_image(yuv_bytes, byte_size, width, height,
                   anyar::pixel_format::yuv420);
});

// 4. Push frames as they arrive (typical video pipeline).
//    Multiple calls coalesce — at most one redraw is in flight per pinhole.
pin->request_redraw();
```

The HTML side just needs a placeholder element whose `data-anyar-pinhole`
matches the C++ id:

```html
<div data-anyar-pinhole="video" style="width:100%;height:100%"></div>
```

The framework injects a tracking script that mirrors the element's
`getBoundingClientRect()` and `devicePixelRatio` to C++ via IPC; the
`GtkGLArea` is repositioned automatically.

The placeholder can sit inside a styled page, but anything covering that
rectangle must remain transparent or the webview will occlude the native
surface below it.

## Pixel formats

`PinholeRenderContext::draw_image()` accepts the same enum as
`@libanyar/api/canvas`'s `FrameRenderer`, which means a webcam / decoder
pipeline can target either renderer without re-encoding:

- `rgba`, `rgb`, `bgra`, `grayscale` — single-plane uploads
- `yuv420` (I420) — 3 planes, BT.601 full-range YUV→RGB in shader
- `nv12`, `nv21` — 2 planes (Y + interleaved UV / VU)

In native GL mode each plane is a separate texture; in canvas-2D fallback
mode the planes are CPU-converted to RGBA before being copied to a
`SharedBuffer` and drawn on the injected canvas.

## Fallback mode

`create_pinhole()` never throws. If the platform cannot bring up a GL
context (headless, sandboxed, missing libepoxy) `Pinhole::is_native()`
returns `false` and the framework activates a canvas-2D fallback:

1. A `SharedBuffer` is allocated for the surface, sized to CSS px × DPR.
2. A tiny JS IIFE is injected into the placeholder div — it creates a
   `<canvas id="__anyar_fb_<id>">` and a `window.__anyar_pb_frame(id)`
   callback that re-fetches `anyar-shm://…` and `putImageData`s it.
3. `request_redraw()` runs the user's `on_render` against a CPU
   `PinholeRenderContext` (whose `clear`/`draw_image` write into the
   SharedBuffer), then evals `__anyar_pb_frame('<id>')`.
4. Behavior parity: same RenderFn, same pixel formats, same `set_rect`
   tracking. Only difference is: `set_continuous(true)` is a no-op (logged
   warning) and CPU pixel-format conversion is on the framework side
   instead of the GPU.

For full-throughput CI tests of the fallback path, set
`PinholeOptions::force_fallback = true`.

## CSS interactions

Properties on the placeholder div (or any ancestor) that do **not** affect
the native overlay:

- `border-radius`
- `transform: rotate / scale / 3D` (logs a console warning)
- `opacity`, `mix-blend-mode`, `filter`, `backdrop-filter`
- `position: sticky`

If your design requires those, use SharedBuffer + FrameRenderer instead.

## Threading & lifetime invariants

- `on_render` is invoked **on the GTK main thread**, not in a LibAsyik
  fiber. If you spawn fibers from inside the callback you must cancel /
  join them before `App::stop()`. Exceptions thrown from the callback
  are caught + logged + the frame is skipped.
- `request_redraw()`, `set_rect()`, `set_visible()`, `set_z_index()` are
  thread-safe and dispatch to the main thread internally.
- All pinholes belonging to a window are destroyed **before** that
  window's webview ([ADR-007 §5b](decisions.md#adr-007-shutdown-sequence)).
  In practice this is automatic: just hold the `shared_ptr<Pinhole>` no
  longer than the `shared_ptr<Window>`.

## Worked example: video-player

The [video-player example](../examples/video-player/) ships with both
renderers behind a CLI flag:

```bash
./video_player                # default: --mode=pinhole
./video_player --mode=webgl   # legacy SharedBuffer + WebGL renderer
```

In pinhole mode the FFmpeg decode loop calls `pin->request_redraw()`
after each frame instead of emitting the `buffer:ready` event; the
`on_render` callback reads the latest `SharedBuffer` directly via
`buf.data()` and forwards to `ctx.draw_image()`. No JS is involved in
rendering, only in transport-bar updates.

## Roadmap

| Phase | Status | Description |
|---|---|---|
| 4g.1 | ✅ | GtkOverlay + GtkGLArea, public API |
| 4g.2 | ✅ | JS rect tracking via ResizeObserver + IPC |
| 4g.3 | ✅ | RGBA / RGB / BGRA / Grayscale / YUV420 / NV12 / NV21 |
| 4g.4 | ✅ | Multi-pinhole lifecycle, z-index, window minimize/restore |
| 4g.5 | ✅ | Transparent canvas-2D fallback |
| 4g.6 | ✅ | ADR-007 shutdown integration, ASAN-clean destroy |
| 4g.7 | ✅ | video-player integration + this guide |
| Phase 7 | ⏳ | Windows (DComp / D3D11), macOS (CAMetalLayer) |
