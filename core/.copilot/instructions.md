# core/ — LibAnyar C++ Core

Static C++17 lib `anyar_core`: `anyar::App`, window manager, IPC router, command registry, event bus, shared memory, built-in plugins (fs, dialog, shell, clipboard, db).

## Layout
- `core/include/anyar/` — public headers (no platform includes): `app.h, app_config.h (AppConfig/WindowConfig/WindowCreateOptions/FileResolver), window.h, window_manager.h, ipc_router.h, command_registry.h, event_bus.h, shared_buffer.h, pinhole.h, pixel_format.h, frame_mailbox.h, task.h, http_file.h, path.h (UTF-8 ⇄ fs::path, is_path_within), embed.h, plugin.h, types.h, main_thread.h`, `gtk_dispatch.h` (deprecated shim → `main_thread.h`), `plugins/`
- `core/src/` — impl + platform splits: `frame_mailbox.cpp` (+ pixel_format helpers), `http_file.cpp`, `shared_buffer.cpp` (factory/registry/pool; mapping in `shared_buffer_<os>.cpp`), `platform.h` (PRIVATE per-OS hooks, ADR-010) → `platform_<os>.cpp` + `main_thread_<os>.cpp`, `*_linux.cpp` (incl. `pinhole_linux.cpp`), `*_win32.cpp` + `win32_util.h` (UTF-8↔UTF-16), `pinhole_stub.cpp` (non-Linux), `plugins/{dialog,clipboard,shell}_{linux,win32}.cpp`, `db_plugin.cpp`, `fs_plugin.cpp`. `window.cpp` is one file with `#ifdef __linux__` / `_WIN32` Impl sections.

## App API (beyond command/emit/on/use)
`http_get/http_post(path, fn)` — pre-`run()` deferred, always beat `serve_static`. `allow_file_access(dir)` → `anyar-file://`. `on_ready(fn)` — after server + plugin init. `on_window_ready(fn(Window&))` — main thread, main window exists, GTK loop not yet running: the place for `create_pinhole()`. `server()`/`port()`/`service()`. `set_on_server_ready` is deprecated. Relative `AppConfig::dist_path` resolves cwd first, then next to the executable (`resolve_dist_path()` in `app_config.h`); `ANYAR_EMBED_FRONTEND` builds ignore it.

## LibAsyik APIs
```cpp
auto as = asyik::make_service();
as->execute([]{/*fiber*/}); as->async([]{/*worker*/});
auto srv = asyik::make_http_server(as, "127.0.0.1", port);
srv->on_http_request("/p", "POST", h, /*insert_front=*/true); // 1.6.1+
srv->serve_static("/", dist_abs, asyik::static_file_config{});
srv->on_websocket("/ws", [](auto ws, auto){...});
auto pool = asyik::make_sql_pool(asyik::sql_backend_sqlite3, "db", 4);
```
`http_server` is a template: use `asyik::http_server_ptr<asyik::http_stream_type>`. Routes accept `<int>`/`<string>` only; regex via `on_http_request_regex`. `find_package(SOCI QUIET)`. `insert_front=true` to beat `serve_static` catch-all.

## Threading (CRITICAL)
Main thread = platform UI loop (`gtk_main()` Linux, Win32 message loop Windows — dispatch window bound by `platform::attach_main_thread()` at the top of `App::run()`). Service thread = `asyik::service::run()` (fibers, HTTP, WS, DB) — ONE thread for every command/route/event. Worker pool = `as->async()`. Webview calls MUST run on main thread; LibAsyik in fibers. Cross-thread: `run_on_main_thread(fn)` from `<anyar/main_thread.h>`, `service_->execute()`.

## Background Work (`<anyar/task.h>`, ADR-009)
- `anyar::run_blocking(service, fn)` — run blocking work (codecs, file I/O, CPU) on the worker pool; suspends only the calling fiber, rethrows exceptions. Never block the service thread directly.
- `anyar::BackgroundTask` — `start(service, [](StopToken st){ while(!st.stop_requested()) … })`, `request_stop()`, `join()`, `join_for(ms)`, `stop(timeout=5s)` (request + join), `running()`. Joinable from fibers and plain threads (plugin `shutdown()`); restartable after it finishes; `start()` while running throws `std::logic_error`. Don't join from inside the body.
- `CommandRegistry::add_async(name, [](const json&, CommandReply reply){…})` — `reply(data, error)` may be called later from any fiber/thread; caller waits. All copies of `reply` dropped uncalled → "did not complete" error; later calls ignored.
- Fibers alive at `service_->stop()` can make process exit spin — join them first (tests too).

## Shutdown (ADR-007 + docs/graceful-shutdown.md — read before changing App::run / Window dtor)
`App::run()` owns the order after `main_win->run()` returns:
1. Drain GTK idle bounded — 200 iters max, never `while(g_main_context_pending())`
2. `plugin->shutdown()` for all plugins (once; service still alive so fibers can see stop flags). Also called on main-window-create failure.
3. `server_->close()` BEFORE `service_->stop()` (cancels accept-loop fiber)
4. `service_->stop() + thread.join()` — 1s drain warning is expected
5. Remove EventBus sinks → `window_mgr_.close_all()`
6. Per window: `pin->notify_window_destroyed()` on every pinhole (drops GtkGLArea/overlay/eval ptrs) → `webview_terminate()` if this window owns the run loop → set `destroyed=true` → bounded drain → `webview_destroy()`. Native GTK "destroy" signal path does the same notify + terminate.
- Plugin contract: long-lived loops → `anyar::BackgroundTask`, `task.stop()` (request + join) in `shutdown()`; any back-pressure wait needs a cancel path (`SharedBufferPool::close()` → blocked `acquire_write()` throws `SharedBufferPoolClosed`) or should be non-blocking (`try_acquire_write()`). Never call `service_->stop()` from plugins or window-close handlers (window-closed callback only does `close_all()`).
- Headless (`has_window_==false`) path just joins the service thread — plugin `shutdown()` is not called there.
- `GBytes` over mmap memory: `g_bytes_new_with_free_func()` capturing `shared_ptr` — never `g_bytes_new_static()` with stack-local
- `webview_terminate()` is thread-safe; never wrap in `post_to_main_thread()` from a fiber (deadlock)
- Regression: `tests/window_close/` (native close → `app.run()` must return within 5s)

## Pinhole (native overlay, ADR-008, docs/pinhole-rendering.md) — Linux only; stub elsewhere
`auto pin = window.create_pinhole(id, PinholeOptions{format, continuous, show_during_scroll, force_fallback});` (call in `on_window_ready`); `window.find_pinhole(id)`. DOM: `<div data-anyar-pinhole="id">`; tracking JS injected once per window via `webview_init` → IPC `pinhole:update_rect|set_visible|get_metrics|dom_detached`.
- `pin->on_render([](PinholeRenderContext& ctx){ ctx.clear(r,g,b,a); ctx.draw_image(ptr, bytes, w, h, pixel_format::yuv420); })` — `size_px()`, `dpr()`; formats rgba/rgb/bgra/grayscale/yuv420/nv12/nv21 (BT.601 full-range). 4:2:0 layouts use ⌈w/2⌉×⌈h/2⌉ chroma; size via `pixel_format_byte_size(fmt,w,h)` (`<anyar/pixel_format.h>`, also `pixel_format_name/from_name`).
- Frames from another thread: `anyar::FrameMailbox` (`<anyar/frame_mailbox.h>`) — producer `auto f = mb->acquire(bytes)` → fill `f->data`/`width`/`height`/`format`/`pts` → `mb->publish(f)` → `pin->request_redraw()`; `on_render` captures the mailbox `shared_ptr` and does `if (auto f = mb->latest()) ctx.draw_frame(*f);` (letterboxed; `draw_frame(f, false)` stretches). Never hand raw pool/buffer pointers to `on_render`.
- `request_redraw()` (push, coalesced), `set_continuous(bool)` (vsync), `set_rect(x,y,w,h)` (CSS px), `set_visible`, `set_z_index`, `on_resize`, `on_visibility`, `on_dom_detached`.
- GtkWindow → outer GtkOverlay{ main: inner GtkOverlay (GtkEventBox + per-pinhole GtkGLArea, GL 3.3 core via libepoxy), overlay child: transparent WebKitWebView on top }. Anything above the placeholder must be transparent.
- `is_native()==false` → GL failed (or `force_fallback`): never throws; CPU render ctx → SharedBuffer → injected canvas-2D via eval. `set_continuous(true)` is a no-op there.
- Threading: `on_render` runs on GTK main thread (NOT a fiber), GL current; exceptions caught+logged, frame dropped. `request_redraw/set_rect/set_visible/set_z_index` thread-safe. Fibers spawned from `on_render` must be cancelled before service stop.
- Lifetime: `Window` holds pinholes in a map; teardown calls `notify_window_destroyed()` before `webview_destroy()` so later `~Pinhole` skips GL/GTK work. Unsupported CSS: border-radius, transforms, opacity/filter/blend, sticky.

## IPC Protocol
Native (~0.01ms): `webview_bind("__anyar_ipc__")` + `webview_return`. JS (GTK thread) → `service_->execute()` → fiber → `window_->dispatch()` → `return_result()`. HTTP fallback `POST /__anyar__/invoke` `{cmd,args,id}` → `{id,data,error}`. WS fallback `/__anyar_ws__`. Routes registered before `app.run()` are deferred and inserted before `serve_static`; after `run()` use `insert_front=true`.

## Shared Memory (Linux; Windows: see above)
`SharedBuffer::create(name,size)` → `shm_open` + `mmap`; names are process-global (duplicate → throws). URI `anyar-shm://<name>` via `webkit_web_context_register_uri_scheme()`. HTTP fallback `GET /__anyar__/buffer/<name>`. `anyar-file://<path>` via `app.allow_file_access(dir)` — path traversal validated against canonical roots; streamed from disk (`GFileInputStream`, no Range).
`SharedBufferPool(base, size, n)` → buffers `base_0..n-1`; slot states FREE→WRITING (`acquire_write()` blocking / `try_acquire_write()` → nullptr if full) →READY (`release_write`, only from WRITING) →FREE (`release_read`, only from READY/READING; stale releases can't free a WRITING slot). `release_unpublished(buf)` WRITING→FREE. `buffer(name)` → `shared_ptr` that outlives the pool. `close()` → acquires throw `SharedBufferPoolClosed`. Recreating a base name requires the old pool destroyed — prefer per-session names.

## HTTP File Serving (`<anyar/http_file.h>`)
`anyar::serve_file(weak_server.lock(), req, path, FileServeOptions{content_type, cache_control="no-store", cors_any_origin, chunk_bytes=256KiB})` — streaming: takes over the connection (direct response, `Connection: close`), body written in chunks read on the worker pool; use for `<audio>`/`<video>` and large files. `serve_file(req, path, opts)` — buffered variant (whole range in memory). Both: 200 full / 206 Range / 416 / 404, `Accept-Ranges: bytes`. ALWAYS answer the full requested range — WebKitGTK media never requests the rest of a shortened 206 (playback stalls, later seeks fail with MEDIA_ERR_DECODE). Capture the server as `weak_ptr` in routes (the route is owned by the server). Helpers `parse_range_header()`, `mime_type_for_path()`, `percent_decode()` (route args are NOT decoded by LibAsyik — decode, then reject `..`). `/__anyar__/file/<path>` uses it.

## Platform Split
Public headers MUST NOT include GTK/Win32/Cocoa. Platform code: `*_linux.cpp` / `*_win32.cpp` / `*_macos.mm`; shared seams go in private `core/src/platform.h` (not `#ifdef`s in `app.cpp`). CMake selects via `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` / `elseif(WIN32)`. Don't set `WEBVIEW_GTK=1` — auto-detected.

## Windows (ADR-010) — MSVC + vcpkg, LibAsyik 1.8.1+
- Deps: vcpkg Boost/OpenSSL/SOCI/nlohmann-json/`webview2` (header only — webview's built-in loader). `scripts/setup-windows.ps1` installs them + LibAsyik into `build-deps/libasyik` (`CMAKE_PREFIX_PATH`). LibAsyik exports `NOGDI WIN32_LEAN_AND_MEAN NOMINMAX` — a TU needing GDI (`dialog_win32.cpp`, `<commctrl.h>`) must `#undef NOGDI` before any include.
- `Window::terminate()` → `platform::request_quit()` (webview's is caller-thread `PostQuitMessage`). Quit is sticky: every `webview_destroy()` we call is followed by `repost_quit_if_requested()` (webview's nested loops swallow `WM_QUIT`); `Window::run()` clears it after the loop.
- Engine HWND subclassed (prop `anyar.window.impl`; webview owns `GWLP_USERDATA`): `WM_ACTIVATE` focus, `WM_CLOSE` closable/close-requested, `WM_SIZE` minimize → pinholes, `WM_DESTROY` destroy handler. Natively destroyed windows keep `wv` until `~Impl` → `webview_destroy()` (frees WebView2 COM). Run-loop owner never posts `on_close` (would delete it inside `run()`).
- First navigation deferred until setup is done (`show()` if loop running, else `run()`): WebView2 applies bind/init scripts only to navigations issued after them. Add scripts before `run()`.
- SharedBuffer = file mapping, no `anyar-shm://`/`anyar-file://` (no-op registration); page gets `window.__LIBANYAR_SHM_SCHEME__ = false` → JS `fetchBuffer()` uses HTTP. Pinhole = stub. CLI + key-storage/video-player/wifi-analyzer: Linux-only.
- Paths are UTF-8 everywhere (IPC/JSON, `AppConfig::dist_path`, `allow_file_access`, `serve_file`). NEVER `fs::path(std::string)` / `path.string()` (ANSI on Windows) — use `<anyar/path.h>` `path_from_utf8()` / `path_to_utf8()`; root checks via `is_path_within()` (component-wise, not string prefix). Win32 API strings: `win32::widen/narrow`.

## Adding a Plugin
1. `core/include/anyar/plugins/my_plugin.h` (impl `IAnyarPlugin`)
2. `core/src/plugins/my_plugin.cpp` — register via `PluginContext`
3. Add to `core/CMakeLists.txt`; auto-register in `App::App()`
4. JS module under `js-bridge/src/modules/`; Catch2 tests under `tests/`
