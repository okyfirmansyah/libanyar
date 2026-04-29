# core/ — LibAnyar C++ Core

Static C++17 lib `anyar_core`: `anyar::App`, window manager, IPC router, command registry, event bus, shared memory, built-in plugins (fs, dialog, shell, clipboard, db).

## Layout
- `core/include/anyar/` — public headers (no platform includes): `app.h, window.h, window_manager.h, ipc_router.h, command_registry.h, event_bus.h, shared_buffer.h, embed.h, plugin.h, types.h, main_thread.h`, `plugins/`
- `core/src/` — impl + platform splits: `*_linux.cpp`, `plugins/{dialog,clipboard,shell}_linux.cpp`, `db_plugin.cpp`, `fs_plugin.cpp`

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
Main thread = platform UI loop (`gtk_main()` Linux). Service thread = `asyik::service::run()` (fibers, HTTP, WS, DB). Worker pool = `as->async()`. Webview calls MUST run on main thread; LibAsyik in fibers. Cross-thread: `run_on_main_thread(fn)` from `<anyar/main_thread.h>`, `service_->execute()`.

## Shutdown (ADR-007 — read before changing App::run / Window dtor)
1. Drain GTK idle bounded — 200 iters max, never `while(g_main_context_pending())`
2. `server_->close()` BEFORE `service_->stop()` (cancels accept-loop fiber)
3. `service_->stop() + thread.join()` — 1s drain warning is expected
4. Remove EventBus sinks → close all windows
5. Set `destroyed=true` BEFORE `webview_destroy()` — stale IPC callbacks must bail
6. `GBytes` over mmap memory: `g_bytes_new_with_free_func()` capturing `shared_ptr` — never `g_bytes_new_static()` with stack-local
7. `webview_terminate()` is thread-safe; never wrap in `post_to_main_thread()` from a fiber (deadlock)

## IPC Protocol
Native (~0.01ms): `webview_bind("__anyar_ipc__")` + `webview_return`. JS (GTK thread) → `service_->execute()` → fiber → `window_->dispatch()` → `return_result()`. HTTP fallback `POST /__anyar__/invoke` `{cmd,args,id}` → `{id,data,error}`. WS fallback `/__anyar_ws__`. Routes registered before `app.run()` are deferred and inserted before `serve_static`; after `run()` use `insert_front=true`.

## Shared Memory (Linux)
`SharedBuffer::create(name,size)` → `shm_open` + `mmap`. URI `anyar-shm://<name>` via `webkit_web_context_register_uri_scheme()`. HTTP fallback `GET /__anyar__/buffer/<name>`. `anyar-file://<path>` via `app.allow_file_access(dir)` — path traversal validated against canonical roots.

## Platform Split
Public headers MUST NOT include GTK/Win32/Cocoa. Platform code: `*_linux.cpp` / `*_win32.cpp` / `*_macos.mm`. CMake selects via `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`. Don't set `WEBVIEW_GTK=1` — auto-detected.

## Adding a Plugin
1. `core/include/anyar/plugins/my_plugin.h` (impl `IAnyarPlugin`)
2. `core/src/plugins/my_plugin.cpp` — register via `PluginContext`
3. Add to `core/CMakeLists.txt`; auto-register in `App::App()`
4. JS module under `js-bridge/src/modules/`; Catch2 tests under `tests/`
