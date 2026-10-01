# LibAnyar — Architecture Decision Records

> Log of significant technical decisions. Newest first.

---

## ADR-012: Pinhole on Windows — DirectComposition Below a Transparent Windowed WebView2

**Date**: 2026-10-01
**Status**: Accepted (supersedes the Windows part of ADR-008)

**Context**: ADR-008 planned the Windows Pinhole port as a switch to `CoreWebView2CompositionController` (visual hosting), with a DComp tree holding both the webview visual and the swap-chain visuals. That is a major-version breaking change: webview/webview creates a windowed controller, and visual hosting would mean taking over its input routing and accessibility. The Linux implementation actually layers the native surfaces **below** a transparent webview (HTML on top), not above it.

**Decision**: Keep webview/webview's windowed WebView2 and reproduce the Linux layering:
- **Transparency:** on the first `create_pinhole()`, set `ICoreWebView2Controller2::put_DefaultBackgroundColor` to fully transparent.
- **Composition:** one DirectComposition target per window, `CreateTargetForHwnd(widget_hwnd, topmost = FALSE)` on webview's host ("widget") HWND, which places its visuals **below** that HWND's child windows, i.e. below the WebView2 window. Each pinhole is a visual whose content is a `CreateSwapChainForComposition` swap chain (BGRA, premultiplied, flip-sequential), offset to the CSS rect × window DPI and re-stacked by `z_index`. (`AddVisual(v, FALSE, nullptr)` adds on top.)
- **Rendering:** one D3D11 device per window (hardware, then WARP). The YUV/gray/RGBA conversion is a runtime-compiled HLSL shader mirroring the GLSL; `rgb` is CPU-expanded. `Present(0, 0)` never blocks the UI thread on vsync; continuous mode is a 16 ms UI timer.
- **Threading:** every D3D/DComp call runs on the UI thread. Posted work captures a `weak_ptr<PinholeState>` — the Win32 equivalent of Linux's `ImplGuard`.
- **Fallback:** the canvas-2D fallback (no D3D11/DComp, or `force_fallback`) is shared logic, fetching over HTTP. `pinhole_cpu.cpp` (CPU converters) and `pinhole_tracking.cpp` (DOM tracking JS) are now platform-neutral.

**Rationale**: A standalone prototype proved DWM composes non-topmost DComp content of the host window underneath a transparent windowed WebView2, with HTML on top. That gives the full feature with no API, hosting, input or accessibility change, and no major version bump.

**Consequence**:
- Once a window has a pinhole, its webview background is transparent. Pages must paint their own background, except over pinholes — as on Linux.
- `tests/pinhole_win32` checks composed pixels via `PrintWindow(PW_RENDERFULLCONTENT)`: position × DPI, z-order and re-stack, visibility, move, canvas fallback, shutdown with live pinholes. It needs a desktop (`display` label).
- `Window` holds every pinhole until the window closes. There is still no remove API (same as Linux).
- video-player defaults to `--mode=pinhole` on Windows too.

---

## ADR-011: Zero-Copy SharedBuffers on WebView2

**Date**: 2026-10-01
**Status**: Accepted

**Context**: ADR-010 shipped Windows SharedBuffers as file mappings read over HTTP, which costs ~60 ms per 8 MiB (1080p RGBA) frame. `anyar-shm://` is not an option on WebView2, because custom schemes must be registered when webview/webview creates the environment. A `WebResourceRequested` handler would still copy every byte across processes, as WebKitGTK's URI-scheme path does on Linux.

**Decision**:
- **Memory:** on Windows, a SharedBuffer's memory *is* a WebView2 shared buffer (`ICoreWebView2Environment12::CreateSharedBuffer`), so `data()` points into memory the page can map. The environment comes from the first window's controller (`webview_get_native_handle(…BROWSER_CONTROLLER)` → `ICoreWebView2_2::get_Environment`), so webview needs no hook. Buffers created before any window exists, runtimes older than 1.0.1661, and a UI thread that doesn't answer within 2 s all fall back to a file mapping and HTTP.
- **Delivery:** the page pulls a buffer lazily with `buffer:attach {name, have}`. C++ posts it into the caller's page, read-only, via `ICoreWebView2_17::PostSharedBufferToScript`, tagged `{name, id}` — unless `have` already equals the buffer's generation `id()`. The page caches the `ArrayBuffer` from `sharedbufferreceived` per name and generation, and releases the previous one when a name is recreated. Pulling instead of pushing covers reloads, late-joining windows and recreated names without C++ tracking page state.
- **JS API:** `fetchBuffer(nameOrUrl, { copy?, id? })`:
  - `copy` defaults to `true`: a snapshot (one in-renderer `slice`), keeping the old semantics.
  - `copy: false` returns the live memory, for consumers that read immediately and then release the pool slot (`createBufferRenderer`, video-player).
  - `id` from a `buffer:ready` payload skips the attach round trip, so steady-state frames cost no IPC at all.
- **COM threading:** STA rules apply. Buffers are created, posted and released on the UI thread; callers on fibers or other threads hop there with `post_to_main_thread` and a bounded boost-fiber future.
- `Window::browser_controller()` (opaque) and `SharedBuffer::id()`, `is_webview_shared()`, `native_handle()` are added. Every core buffer payload now carries `id`.

**Rationale**: This is the only path where the producer's memory is what the page reads, with no per-frame copy on either side. Measured on WebView2 per 8 MiB: HTTP ~60 ms; attach round trip ~2 ms; `slice` ~4–6 ms; live view ~0.01 ms. In video-player, ~90 frames took 4 `buffer:attach` calls (one per pool slot) and 0 HTTP fetches.

**Consequence**:
- A live view changes when the producer writes again. The pool protocol (READY → consumer → `release_read`) is what makes `copy:false` safe, so standalone buffers should stay on the default snapshot.
- A buffer is attachable only by windows that share the allocating environment. Other windows get `attached:false` and use HTTP.
- Linux is unchanged (`anyar-shm://`). The WebKitGTK zero-copy work (the webext prototype) is separate.

---

## ADR-010: Windows Port — Win32 Platform Layer on webview/WebView2

**Date**: 2026-09-30
**Status**: Accepted

**Context**: Phase 7 begins with Windows. LibAsyik 1.8.1 is the first release that builds with MSVC (vcpkg Boost 1.90, OpenSSL 3, SOCI 4.0.3). It exports `_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX NOGDI` and `/bigobj /Zc:__cplusplus /utf-8` to every target that links it. The vendored webview/webview 0.12 already has a WebView2 backend with a built-in loader. Four behaviours of that backend differ from GTK in ways the core relied on:
1. `webview_terminate()` is a bare `PostQuitMessage(0)`, which quits the *calling* thread's loop. `window:close-all` calls it from a service-thread fiber.
2. webview's nested loops (`deplete_run_loop_event_queue()` inside `webview_destroy()`, script registration) exit on `WM_QUIT` and consume it, so a pending quit can be lost.
3. Bindings and init scripts use `AddScriptToExecuteOnDocumentCreated`, which only affects navigations issued after the call. webview pumps the message loop while adding each script. A navigation issued in `Window`'s constructor therefore commits before `window.__anyar_ipc__` exists.
4. WebView2 custom schemes must be registered when the environment is created, which webview/webview does internally, so `anyar-shm://` / `anyar-file://` cannot be served yet.

**Decision**:
- **Private platform hooks** (`core/src/platform.h`): `init_process`, `executable_path`, `attach_main_thread`, `drain_main_thread`, `has_shm_uri_scheme`, plus Win32 `request_quit` / `repost_quit_if_requested` / `clear_quit_request`. Implemented in `platform_<os>.cpp` and `main_thread_<os>.cpp`, so `app.cpp` has no platform `#ifdef`s. The public headers are unchanged.
- **Main-thread dispatch**: a message-only window created by `App::run()` on the UI thread. `post_to_main_thread()` posts to it (modal loops dispatch it too, like `g_idle_add`), and posts made before attach are queued.
- **Quit** is sticky: `request_quit()` sets a flag and posts `WM_QUIT` to the UI thread. Every `webview_destroy()` we call re-posts it, and `Window::run()` clears it once the loop has returned.
- **Window**: the engine HWND is subclassed (`Impl*` kept in a window property, because webview owns `GWLP_USERDATA`). The mapping to the GTK signals: `WM_ACTIVATE`→focus, `WM_CLOSE`→closable/close-requested, `WM_SIZE`→pinhole pause, `WM_DESTROY`→destroy handler. Owned windows (`GWLP_HWNDPARENT`) replace transient-for, and `EnableWindow(parent, FALSE)` provides modality. A natively destroyed window keeps its engine until `~Impl` calls `webview_destroy()`, which releases the WebView2 COM objects. The run-loop owner never posts `on_close`: posted messages come before `WM_QUIT` and would delete the main `Window` inside `run()`. `App::run()` tears it down instead.
- **Initial navigation is deferred** on Win32 until setup is done: in `show()` if the UI loop is running (child windows), otherwise at the start of `Window::run()`, which also covers `on_window_ready` scripts.
- **SharedBuffer** uses a pagefile-backed file mapping. The C++ side injects `window.__LIBANYAR_SHM_SCHEME__ = false`, and `fetchBuffer()` then uses `GET /__anyar__/buffer/<name>`. Other buffer IPC is unchanged.
- **Plugins**: `IFileOpenDialog`/`IFileSaveDialog`, `TaskDialogIndirect` (Common Controls v6 via a `#pragma` manifest dependency, `MessageBoxW` fallback), `CF_UNICODETEXT` clipboard, `CreateProcessW` + pipes for `shell:execute` (code 127 when the program cannot start), `ShellExecuteW` for `openUrl`/`openPath`.
- **Ports**: `App` (and the tests) ask the OS for a free ephemeral port instead of picking one at random. Windows reserves blocks of 49152–65535 for Hyper-V/WSL, and system RPC services listen there. A failed `bind()` inside a test fiber skipped `svc->stop()`, and `run()` hung (~13% of runs).
- **Deferred**: Pinhole stays a stub (DComp port per ADR-008); `anyar` CLI and the key-storage / video-player / wifi-analyzer examples are Linux-only.

**Rationale**: The public API stays platform-neutral, as the Phase 4e principle requires. Each Win32 divergence is handled where it originates: thread affinity in dispatch/quit, script timing in navigation. Callers need no per-platform code. HTTP for buffers costs one copy and loopback TCP, but it works today and the JS fallback already existed.

**Consequence**:
- `Window::terminate()` is thread-safe on both platforms.
- On Windows the first page load starts only when `run()` is entered, or at `show()` for windows created while the loop runs. A child window created before the main loop starts loads nothing until it is navigated.
- `std::filesystem::path(std::string)` uses the ANSI code page on Windows. All core paths are therefore UTF-8 and converted through `<anyar/path.h>` (`path_from_utf8` / `path_to_utf8`), a contract that plugins must follow too (added 2026-09-30).
- A zero-copy WebView2 path (`CreateSharedBuffer` / `PostSharedBufferToScript`, or a custom scheme) needs a hook into webview's environment creation.

---

## ADR-009: Background Work, Async Commands and Cross-Thread Frame Handoff

**Date**: 2026-09-24
**Status**: Accepted

**Context**: A review of `examples/video-player` traced its crashes, stalls and seek stutter to gaps in the platform, not only in the example:
- Every command, HTTP route, event and plugin loop shares ONE fiber thread. Blocking calls (FFmpeg, `ifstream`) froze all IPC for seconds. `add_async` returned an error unless the handler replied before returning, so there was no real offload path.
- `service_->execute()` loops could be flagged to stop but not joined. Stopping one session and starting the next could overlap them, and the old loop's teardown clobbered the new one.
- `SharedBufferPool::acquire_write()` hands out a raw `SharedBuffer&`. Pinhole's `on_render` (GTK thread) read slots the producer fiber was recycling or unmapping.
- No Range-capable file serving existed. `anyar-file://` and `/__anyar__/file` loaded whole files into memory.

**Decision**: Four framework primitives, used by the example and recommended for all plugins:
1. `anyar::run_blocking(service, fn)` (`<anyar/task.h>`) runs `fn` on LibAsyik's worker pool (`service::async`) and suspends only the calling fiber.
2. `anyar::BackgroundTask` (`<anyar/task.h>`): `start(service, body(StopToken))`, `request_stop()`, `join()` / `join_for()` / `stop(timeout)`. Joinable from a fiber or a plain thread (plugin `shutdown()`), and restartable.
3. `CommandRegistry::add_async` resolves when `reply` is called, from any fiber or thread, at any time. Dropping every copy of `reply` without calling it yields a "did not complete" error.
4. `anyar::FrameMailbox` + `anyar::Frame` (`<anyar/frame_mailbox.h>`) form a non-blocking, ref-counted "latest frame" handoff. A consumer's `shared_ptr` pins the frame; the producer recycles only unreferenced frames. `PinholeRenderContext::draw_frame(frame, preserve_aspect)` draws one letterboxed.

Plus: `anyar::serve_file()` (`<anyar/http_file.h>`), with Range/206/416 support. It always answers the full requested range; the streaming overload writes it in chunks read on the worker pool. An early version shortened open-ended ranges to 4 MB, which WebKitGTK media does not handle: `<audio>` stalled after one chunk and later seeks failed with MEDIA_ERR_DECODE. `anyar-file://` now streams from a `GFileInputStream`. `SharedBufferPool` gains `try_acquire_write()`, `release_unpublished()` and `buffer(name)` (shared ownership), and state transitions are now validated.

**Rationale**:
- Keeps the single-service-thread model (no data races between command fibers) and moves only the blocking work off it.
- Ownership (`shared_ptr`) is the only handoff that stays safe when producer and consumer live on different threads with different lifetimes. Locks alone cannot stop a consumer from reading memory the producer frees afterwards.
- Frame pacing belongs to the producer. A live producer drops frames (`try_acquire_write`) instead of blocking on a slow consumer.

**Consequence**:
- Plugins should wrap long-lived loops in `BackgroundTask` and call `stop()` in `shutdown()`, and wrap blocking calls in `run_blocking()`.
- Fibers still alive at `service_->stop()` can make process exit spin (LibAsyik). `BackgroundTask::stop()` before shutdown avoids this.
- `SharedBufferPool::release_write()` now requires the slot to be WRITING. Callers relying on the old unconditional transition must acquire first.
- `draw_image()` validates 4:2:0 sizes with ⌈w/2⌉×⌈h/2⌉ chroma (`pixel_format_byte_size`). Undersized odd-dimension buffers are now rejected instead of over-read.

---

## ADR-008: Pinhole (Native Overlay) Rendering Architecture

**Date**: 2026-04-28
**Status**: Accepted — Windows hosting superseded by [ADR-012](#adr-012-pinhole-on-windows--directcomposition-below-a-transparent-windowed-webview2) (no visual-hosting migration, not breaking)

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
