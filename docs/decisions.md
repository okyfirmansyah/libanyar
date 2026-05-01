# LibAnyar — Architecture Decision Records

> Log of significant technical decisions. Newest first.

---

## ADR-008: Pinhole (Native Overlay) Rendering Architecture

**Date**: 2026-04-28
**Status**: Accepted

**Context**: Phase 4f delivers zero-copy shared memory (SharedBuffer) + WebGL frame rendering (~1ms / 1080p). The remaining bottleneck is two steps: (a) JS must `fetch("anyar-shm://")` which still involves a WebKit URI scheme handler callback and an IPC event, and (b) `texImage2D` requires a GPU upload from the CPU-mapped memory. For 4K/8K at 60fps or camera-class latency budgets, a more direct path is needed.

**Options considered**:

| Option | Description | Why rejected / accepted |
|---|---|---|
| **Chroma-key punch-through** | Render a known color in DOM; OS compositor replaces with overlay | Requires compositor cooperation not available on stock WebKitGTK/WebView2/WKWebView |
| **Child window (sibling OS window)** | Create a second OS window and layer it manually | Z-ordering unreliable across WMs; IME/focus breaks; window chrome visible |
| **wl_subsurface** | Wayland protocol, sibling surface with compositor-pinned transform | Not available through standard GTK3/WebKitGTK APIs; per-compositor support |
| **GtkOverlay + GtkGLArea** (Linux) | GtkOverlay wraps WebKitWebView; GtkGLArea placed as overlay child | ✅ First-class GTK primitive, GL context integrated, HiDPI-aware |
| **CoreWebView2CompositionController** (Windows) | Visual hosting via DirectComposition | ✅ First-class WebView2 API; requires host migration (breaking) |
| **CAMetalLayer sibling** (macOS) | Metal layer in same NSView hierarchy as WKWebView | ✅ Standard Cocoa pattern; no additional SDK required |

**Decision**: Implement the Pinhole API using:
- **Linux**: `GtkOverlay` containing the `WebKitWebView` + `GtkGLArea` as overlay child; OpenGL 3.3 core via libepoxy.
- **Windows** (Phase 7): Switch WebView2 host to `CoreWebView2CompositionController` (visual hosting); D3D11 swap-chain visual in DComp tree. This is a **major version breaking change** — no runtime fallback, documented migration note.
- **macOS** (Phase 7): Sibling `CAMetalLayer` in the WKWebView's hosting `NSView`.

**Coexistence**: `@libanyar/api/canvas` (existing — WebGL + SharedBuffer, in-DOM `<canvas>`) and `@libanyar/api/pinhole` (new — native overlay surface) are **parallel APIs**. They have different DOM models (canvas element vs transparent placeholder div) and different tradeoff profiles. Users pick the right one for the job.

**Design decisions locked in**:
1. **Scroll behavior**: hide overlay on any `scroll` event in ancestor chain; reshow on `scrollend` + RAF idle. Documents as "scroll-time flicker intentional — avoids visual swim."
2. **Windows hosting migration**: major version bump, not a runtime detect.
3. **Tier-2 escape hatch** (raw `GLuint` / `id<MTLTexture>` / `ID3D11Texture2D*`): deferred to a later phase; Phase 1 ships Tier-1 only (`pixel_format` enum).
4. **Naming**: `anyar::Pinhole`, `@libanyar/api/pinhole`, DOM attribute `data-anyar-pinhole`.
5. **Fallback**: `create_pinhole()` success even if GL init fails; internally routes to SharedBuffer + FrameRenderer driving an injected `<canvas>`. Same `on_render` signature. `Pinhole::is_native()` returns false.
6. **Coexistence**: parallel APIs, not unified; see above.

**Documented limitations** (CSS that does NOT affect the native overlay):
- `border-radius` (no GL stencil approximation in Phase 1)
- `transform: rotate / scale / 3D` on placeholder or any ancestor → console warning + fallback engaged
- `opacity`, `mix-blend-mode`, `filter`, `backdrop-filter`
- Higher-`z-index` DOM siblings: JS detects intersection and signals C++ to hide overlay
- `position: sticky` ancestors: not officially supported

**GtkOverlay widget hierarchy** (established from webview/webview source reading):
```
webview_create() produces:
  GtkWindow
    └── WebKitWebView   ← direct child (via gtk_container_add / gtk_window_set_child)

After Pinhole init restructure:
  GtkWindow
    └── GtkOverlay      ← new intermediate container
          ├── WebKitWebView   ← main child (fills overlay)
          └── GtkGLArea       ← overlay child, positioned to match placeholder div
```

**Shutdown ordering** (extends ADR-007):
- Plugin-owned background work must be stopped from `shutdown()` before `server_->close()` / `service_->stop()`.
- Step 5b added: destroy all `Pinhole` objects belonging to a window BEFORE calling `webview_destroy()`.
- GL context cleanup dispatched via `run_on_main_thread()`.
- Any back-pressure wait used by plugin-owned producers must have a close/cancel path so shutdown cannot strand a fiber.

**Platform dependencies added (Linux)**:
- `libepoxy` (already a transitive dep of WebKitGTK; adds `pkg_check_modules(EPOXY REQUIRED epoxy)`)
- No new system packages required on a standard Linux dev box.

**Consequence**: ~5–10× lower latency than WebGL+SharedBuffer for high-frame-rate workloads. Imposes a flat-rectangle constraint on the rendering region and requires scroll-time hide. Windows port requires a one-time breaking host migration in Phase 7.

---

## ADR-007: App Shutdown Sequence & WebView Teardown

**Date**: 2026-03-15
**Status**: Accepted (resolved CI-only timeout + segfault)

**Context**: The WebGL E2E test passed all assertions but segfaulted (locally) or hung with a 30s timeout (CI/xvfb) during `App::run()` shutdown. This required 4 iterations to fully resolve and uncovered multiple interacting race conditions between GTK, LibAsyik fibers, and webview's internal cleanup.

**Root Causes Found** (in discovery order):

1. **`~Impl()` destruction ordering**: `webview_destroy()` calls `deplete_run_loop_event_queue()` which drains **all** pending `g_idle_add` callbacks. Stale IPC-return callbacks fired during this drain, calling `webview_return()` on the already-destroyed WebKitWebView → segfault. **Fix**: Set `destroyed = true` *before* `webview_destroy()`, and drain a bounded number of GTK events before calling it.

2. **SharedBuffer GBytes use-after-free**: `handle_shm_uri_request()` used `g_bytes_new_static()` with a raw pointer to mmap'd memory. The `shared_ptr<SharedBuffer>` was a stack local that died at function exit, leaving `GBytes` referencing unmapped memory. **Fix**: Use `g_bytes_new_with_free_func()` capturing the `shared_ptr` in the destroy callback.

3. **`window:close-all` fiber blocked on `post_to_main_thread()`**: This dispatched `terminate()` via `g_idle_add` and blocked the fiber waiting for a promise. On CI (slow xvfb), `service_->stop()` was called before the promise was fulfilled, causing "1 fiber(s) still active after 1s drain". **Fix**: Call `terminate()` directly — it's already thread-safe (uses `g_idle_add` internally).

4. **HTTP accept-loop fiber not exited**: `service_->stop()` sets a stopped flag but doesn't close the HTTP acceptor. The accept fiber blocks on `async_accept()` indefinitely. **Fix**: Call `server_->close()` before `service_->stop()` to cancel the pending accept.

5. **Unbounded GTK drain loops under xvfb**: `while (g_main_context_pending())` loops in Window destructor and `App::run()` ran indefinitely because WebKitGTK continuously generates events during web process teardown under xvfb. **Fix**: Cap all drain loops to 200 iterations with `for (int i = 0; i < 200 && ...; ++i)`.

**Current Shutdown Sequence** (in `App::run()` after `main_win->run()` returns):
```
1. Drain GTK idle callbacks (bounded, 200 iterations max)
   → fulfils run_on_main_thread() promises so blocked fibers can resume
2. Plugin shutdown
   → stop plugin-owned background work while the service thread is still alive
3. server_->close()
   → cancels accept-loop fiber
4. service_->stop() + service_thread_.join()
   → drains 1s for active fibers, then returns
5. Remove event sinks
   → prevents eval on dying webviews
6. window_mgr_.close_all()
   → for each window:
      6a. set destroyed=true, bounded GTK drain
      6b. pinholes.clear() — destroy all Pinhole objects (GL context still live)
          • GL objects freed via GtkGLArea unrealize → destroy_gl_objects()
          • Non-main-thread destruction dispatched via g_idle_add (safe because
            step 6b runs before webview_destroy keeps the main loop alive)
      6c. webview_destroy()
```

**Key Invariants** (must be maintained in future changes):
- `destroyed` must be set to `true` BEFORE calling `webview_destroy()`
- Never use unbounded `while(g_main_context_pending())` loops — always cap iterations
- Plugin-owned long-lived fibers must stop in `shutdown()` before `service_->stop()`
- `server_->close()` must precede `service_->stop()`
- Any back-pressure or wait loop used by plugin background work needs a close/cancel path
- `webview_terminate()` is thread-safe — never wrap it in `post_to_main_thread()` from a fiber (deadlock risk)
- Any `GBytes` wrapping shared memory must capture the `shared_ptr` via destroy callback, not hold a stack-local ref
- `pinholes.clear()` (step 5b) MUST precede `webview_destroy()` (step 5c) — GL context cleanup requires the GTK main loop to still be running
- `Pinhole::on_render` callbacks run on the GTK main thread, NOT in a fiber. Any LibAsyik fibers spawned inside `on_render` must be cancelled before `service_->stop()` or they will outlive the service thread

**Consequence**: Added a 5-second watchdog thread in the WebGL E2E test as a safety net for any remaining CI edge cases.

---

## ADR-006: Use LibAsyik `serve_static()` for Frontend Assets

**Date**: 2026-03-07  
**Status**: Accepted (supersedes earlier custom implementation)

**Context**: LibAsyik 1.5.1 (master) includes `serve_static()` with `static_file_config` — providing MIME type detection, ETag/Last-Modified caching, Range requests, and directory index serving out of the box.

**Decision**: Use `server->serve_static("/", dist_abs, cfg)` to serve frontend assets.

**Rationale**:
- LibAsyik's `serve_static()` handles: MIME types, ETags, 304 Not Modified, Range/206, Cache-Control, index files, path traversal protection
- Reduces app.cpp by ~70 lines vs the prior custom implementation
- IPC routes registered first (first match wins), so `/__anyar__/invoke` and `/__anyar_ws__` take priority over the catch-all regex generated by `serve_static`
- Earlier custom implementation was built when the installed LibAsyik was from the wrong branch (`http_digest` instead of `master`)

**Consequence**: Fewer lines of code, better caching behaviour, maintained upstream.

---

## ADR-005: Use HTTP Localhost for Frontend Assets (Not File Protocol)

**Date**: 2026-03-07  
**Status**: Accepted

**Context**: WebView can load content via `file://` protocol or via HTTP from localhost.

**Decision**: Use HTTP localhost to serve frontend via `http://127.0.0.1:<port>/`.

**Rationale**:
- `file://` restricts many Web APIs (fetch, CORS, Web Workers, SharedArrayBuffer)
- HTTP serving enables the same IPC endpoint for both commands and assets
- Hot-reload during development is trivial (just point Vite proxy)
- Same approach used by VS Code (localhost), Jupyter, Tauri (localhost mode)
- Static files served via LibAsyik `serve_static()` (see ADR-006)

**Consequence**: Need to bind to a random available port and pass it to webview.

---

## ADR-004: Dual IPC Channels (HTTP POST + WebSocket)

**Date**: 2026-03-07  
**Status**: Accepted

**Context**: Need IPC between web frontend and C++ backend. Options: webview.bind(), HTTP, WebSocket, custom protocol.

**Decision**: Use HTTP POST for request-response commands, WebSocket for bidirectional events.

**Rationale**:
- HTTP POST for commands: simple, stateless, easy to debug with curl/browser devtools, naturally request-response
- WebSocket for events: persistent connection, low latency push from backend, bidirectional stream
- Both are built into LibAsyik (no new dependencies)
- `webview.bind()` has limitations: string-only, synchronous callback on some platforms, harder to do async
- This dual-channel approach is similar to LSP (Language Server Protocol) and proven at scale

**Consequence**: JS bridge needs to manage both HTTP fetch and WebSocket connection.

---

## ADR-003: LibAsyik as Foundation (Not libuv/asio Standalone)

**Date**: 2026-03-07  
**Status**: Accepted

**Context**: Need HTTP server, WebSocket, async I/O, possibly database. Could use individual libraries or LibAsyik.

**Decision**: Use LibAsyik for HTTP, WebSocket, SQL, concurrency, and logging.

**Rationale**:
- LibAsyik wraps Boost.Asio + Beast + Fiber into an ergonomic API
- One library provides: HTTP server/client, WebSocket server/client, SQL pools, fiber scheduling, logging, rate limiting, caching
- Fiber model allows synchronous-looking code that's actually highly concurrent
- `serve_static()` (added in v1.5.1) with ETag/Range/MIME eliminates need for a separate static file server
- Maintained, MIT licensed, 32 releases, built on battle-tested Boost libraries
- Eliminates 3-4 separate dependencies (libuv, cpp-httplib, spdlog, custom thread pool)

**Consequence**: Takes a dependency on Boost (1.81+). Binary size slightly larger than minimal, but still in target range.

---

## ADR-002: webview/webview for WebView Abstraction

**Date**: 2026-03-07  
**Status**: Accepted

**Context**: Need cross-platform native webview. Options: raw platform APIs, webview/webview, CEF, Ultralight.

**Decision**: Use `webview/webview` header-only library (C API).

**Rationale**:
- Header-only (70 files), MIT license, 13.9K GitHub stars
- Uses C API: `webview_create`, `webview_run`, `webview_navigate`, `webview_eval`, `webview_bind`, etc.
- Wraps WebKitGTK (Linux), WebView2 (Windows), WKWebView (macOS)
- Zero binary size impact (uses OS-provided engines)
- Provides: create window, navigate, eval JS, bind C++ functions, resize
- Same conceptual approach as Tauri's Wry
- CEF bundles Chromium (~200MB) — defeats the purpose
- Ultralight is proprietary and not truly native

**Consequence**: UI behavior may vary slightly across platforms (WebKit vs Chromium rendering). Mitigation: test matrix.

---

## ADR-001: C++17 as Language Standard

**Date**: 2026-03-07  
**Status**: Accepted

**Context**: Choose C++ standard version for the framework.

**Decision**: C++17 minimum, with optional C++20 features where beneficial.

**Rationale**:
- C++17 provides: `std::filesystem`, `std::optional`, `std::variant`, `std::string_view`, structured bindings, if-constexpr, `[[nodiscard]]`
- Well supported by GCC 9+, Clang 10+, MSVC 2019+
- LibAsyik supports C++11+ but benefits from C++17 features
- C++20 (concepts, coroutines, ranges) is nice but not yet universally supported
- C++17 is the pragmatic sweet spot for 2026

**Consequence**: Minimum compiler requirements: GCC 9, Clang 10, MSVC 2019.
