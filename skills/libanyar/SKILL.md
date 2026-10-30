---
name: libanyar
description: Build, extend, debug and ship desktop apps with LibAnyar — the Tauri-style C++17 framework where a web frontend (Vite + Svelte/React/vanilla) runs in the OS webview (WebKitGTK, WebView2) and a C++ backend (anyar::App on LibAsyik fibers) serves IPC commands, events and binary data. Use this skill whenever the user is working in a LibAnyar project or mentions LibAnyar, the `anyar` CLI (init/dev/build/package), `@libanyar/api`, `anyar::App`, `app.command`, IAnyarPlugin, SharedBuffer, Pinhole, FrameMailbox, run_blocking, BackgroundTask, or wants a C++ desktop app with an HTML/JS UI — including adding commands or plugins, streaming video/3D/binary data to the UI, packaging installers, fixing crashes on exit, or testing the UI in the real webview — even if they don't name the framework explicitly.
---

# LibAnyar app development

LibAnyar apps are two programs in one process: a **C++ backend** (`anyar::App`) and a **web frontend** shown in the operating system's webview. The frontend talks to the backend through `@libanyar/api` (`invoke` commands, `listen` to events, fetch binary buffers). Most mistakes come from three things: blocking the wrong thread, sending big binary data as JSON, and tearing things down in the wrong order. The rules below exist for those reasons.

Reference files, read when the task needs them:

| File | Read it when… |
|---|---|
| `references/cpp-backend.md` | writing `main.cpp`, commands, events, plugins, HTTP routes, windows, file access |
| `references/frontend.md` | calling the backend from TS/JS, windows/dialogs/fs/db from the UI, React hooks |
| `references/binary-data-and-rendering.md` | sending images/video/meshes/large arrays, WebGL, native overlays (Pinhole) |
| `references/debugging-and-testing.md` | a UI bug, a crash, a hang on exit, or writing an automated UI test |
| `references/build-and-ship.md` | project setup, CMake, `anyar build`, embedding, DEB/AppImage/MSI/NSIS, Windows specifics, third-party assets |

The project's own docs (`docs/` in the LibAnyar repo: getting-started, writing-plugins, graceful-shutdown, shared-memory-webgl, pinhole-rendering, multi-window, debugging, packaging) are the source of truth if anything here looks out of date — check the installed headers (`core/include/anyar/*.h`) before relying on a signature you are unsure of.

## The threading model (get this right first)

| Thread | Runs | Never do here |
|---|---|---|
| **Main / UI thread** | the GTK or Win32 message loop, every webview call, `on_window_ready`, Pinhole `on_render`, plugin `shutdown()` | block; wait for a fiber that needs the UI thread |
| **Service thread** (one) | every command handler, HTTP route and event — as LibAsyik **fibers** | blocking I/O, codecs, heavy CPU, `sleep` (it freezes *all* IPC) |
| **Worker pool** | `anyar::run_blocking(service, fn)` jobs | touch fiber-only state or webview APIs |

- Blocking or CPU-heavy work in a command → wrap it: `auto r = anyar::run_blocking(service_, [=] { return decode(path); });` Only the calling fiber waits; other commands keep flowing.
- A long-lived loop (polling a device, a decode loop) → `anyar::BackgroundTask` (`<anyar/task.h>`): `task.start(service, [](anyar::StopToken st){ while (!st.stop_requested()) … })`, and `task.stop()` (request + join) in `shutdown()`.
- Something that must run on the UI thread from a fiber → `anyar::run_on_main_thread(fn)` (`<anyar/main_thread.h>`).
- Frames produced on another thread for a native overlay → `anyar::FrameMailbox`, never raw pointers.

## Core pattern

```cpp
#include <anyar/app.h>
#include <anyar/task.h>
#ifdef ANYAR_EMBED_FRONTEND
#include <anyar/embed.h>
#endif

int main() {
    anyar::AppConfig config;
    config.port = 0;                 // pick a free port
    config.dist_path = "./dist";     // Vite build output (cwd, then next to the exe)
    anyar::App app(config);

    app.command("greet", [](const anyar::json& args) -> anyar::json {
        return {{"message", "Hello, " + args.value("name", "world")}};
    });

    anyar::WindowConfig win;
    win.title = "My App"; win.width = 1100; win.height = 750;
    app.create_window(win);
#ifdef ANYAR_EMBED_FRONTEND
    app.set_frontend_resolver(anyar::make_embedded_resolver());
#endif
    return app.run();                // returns the exit code
}
```

```ts
import { invoke, listen } from '@libanyar/api';
const { message } = await invoke<{ message: string }>('greet', { name: 'Ada' });
const stop = listen<{ pct: number }>('job:progress', (p) => bar.value = p.pct);
```

Command errors: throw from the handler (`throw std::invalid_argument("…")`) — the frontend's `invoke` rejects with that message. Reserve crashes for the truly unrecoverable.

Anything bigger than a page of state belongs in a **plugin** (`IAnyarPlugin`, registered with `app.use(std::make_shared<MyPlugin>())`), which owns its state, registers `ns:verb` commands in `initialize(PluginContext&)`, and cleans up in `shutdown()` — see `references/cpp-backend.md`.

## Rules that prevent the common failures

1. **Binary data never goes through JSON.** Images, frames, audio, meshes, point clouds → a `SharedBuffer` (one-off) or `SharedBufferPool` (streaming), fetched with `buffer.fetchBuffer()` — or a Pinhole native overlay for video-rate pixels. A 5 MB mesh as JSON is ~20× slower and stalls IPC. Return only metadata (offsets, sizes, ids) from the command. Details: `references/binary-data-and-rendering.md`.
2. **Shutdown is ordered for you — cooperate with it.** `App::run()` calls every plugin's `shutdown()` **on the main thread while the service thread is still running commands**, then stops the service. So: stop your BackgroundTasks there, unblock any waits (`SharedBufferPool::close()`), **lock state that commands also touch** (a late command from the page can race your cleanup and corrupt the heap) — briefly, never while joining a task, or a command waiting on the lock freezes the service thread and the join never returns, and make sure commands waiting on `run_blocking` have finished. Never call `service_->stop()` yourself.
3. **Test exit while busy.** Shutdown bugs only show when the app closes mid-work. Use a test script that exits at a random moment and run it 20–30 times (`references/debugging-and-testing.md`).
4. **Paths are UTF-8 strings over IPC.** Convert with `anyar::path_from_utf8()` / `path_to_utf8()` (`<anyar/path.h>`), never `fs::path(std::string)` — it breaks non-ASCII paths on Windows. Check containment with `anyar::is_path_within()`.
5. **Media files**: serve them with `anyar::serve_file()` from an `http_get` route (Range support), not by reading them into a command result.
6. **Portable C++**: C++17, no GCC-only extensions (`a ?: b`), no `unistd.h`/`getpid()` in shared code, platform code in `*_linux.cpp` / `*_win32.cpp`. LibAnyar targets Linux and Windows today, macOS later.
7. **Don't ship debug automation.** `ANYAR_DEBUG_TOOLS` / `debug_tools.automation` lets page code screenshot and synthesize input; keep it to dev/test.

## Workflow for a feature

1. **Shape the boundary first**: which commands (`ns:verb`, JSON in/out), which events (backend → UI pushes), which buffers. Keep JSON small; push progress as events.
2. **Backend**: plugin or `app.command`; blocking work through `run_blocking`; long loops as BackgroundTask; teardown in `shutdown()`.
3. **Frontend**: typed wrappers around `invoke` in one module (`api.ts`), not scattered string commands; unsubscribe listeners on unmount.
4. **Verify in the real webview**, not only in a browser: `ANYAR_CONSOLE=1` to see page errors in the terminal, and an `ANYAR_TEST_SCRIPT` smoke test that drives the UI and exits with a code (works headless under `xvfb-run` on Linux). Also close the app mid-operation a few times.
5. **Unit-test the C++ logic** that has no UI dependency (Catch2 is bundled with LibAsyik), and keep the plugin's pure logic in a separate file so it can be tested without a window.

## Debugging quick reference

| Symptom | First move |
|---|---|
| UI misbehaves, no visible error | run with `ANYAR_CONSOLE=1` — page console + uncaught errors appear in the terminal as `[js:main] …` |
| Need to inspect DOM/CSS | `config.debug = true` → right-click → Inspect (WebKitGTK) / F12 (WebView2) |
| Works in Chrome, broken in the app (or vice versa) | engine difference: open the same running app at `http://127.0.0.1:<port>` in Chrome to split *your code* from *the engine* |
| All IPC freezes for a moment | something blocks the service thread — wrap it in `run_blocking` |
| Crash / hang on exit | the shutdown rules above; reproduce with the random-exit loop |
| Need a regression test for a UI flow | `ANYAR_TEST_SCRIPT=smoke.js` with `anyarTest.click/type/key/waitFor/screenshot/done` |

Full details and templates: `references/debugging-and-testing.md`.
