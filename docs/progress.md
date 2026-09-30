# LibAnyar — Progress Tracker

> **Current Phase**: Phase 7 started — Windows core port (MSVC + WebView2, ADR-010); Linux hardening findings continue
> **Phase Status**: 🟢 Phases 1–4g complete (Linux), Phases 5–6 partial, Phase 7 Windows core working (local: 12/12 ctest); Windows CI job added, first run pending
> **Last Updated**: 2026-09-30 (ADR-010)

> **Agents**: update this file (and the matching checkbox in [roadmap.md](roadmap.md)) at the end of every task that changes status, adds a feature, or discovers a risk. Keep "Next Priorities" and "Open Risks" current; append to the Session Log.

---

## Completed Phases

| Phase | Title | Status |
|-------|-------|--------|
| 1 | Core Prototype (Linux) | ✅ Complete |
| 2 | JS Bridge NPM Package | ✅ Complete |
| 3 | Native APIs & Plugins | ✅ Complete |
| 4 | Database Integration | ✅ Complete |
| 4b | Test Suite | ✅ Complete |
| 4c | Hybrid IPC (Native + HTTP Fallback) | ✅ Complete |
| 4d | Multi-Window & Child Windows | ✅ Complete |
| 4e | Platform Abstraction Refactor | ✅ Complete |
| 4f | Shared Memory IPC & WebGL Canvas | ✅ Complete |
| 4g | Pinhole (Native Overlay) Rendering | ✅ Complete (Linux) |
| 5 | CLI Tool | 🟡 Partial (C++ watch mode open) |
| 6 | Polish & Documentation | 🟡 Partial (benchmarks, extra examples open) |
| 7 | Windows & macOS | 🟡 Windows core started (ADR-010); macOS not started |
| 8 | Plugin System & Packaging | 🔲 Not started |

See [roadmap.md](roadmap.md) for full per-task checklists and [roadmap.md — Next Steps](roadmap.md#next-steps-prioritized) for the prioritized list.

---

## CI / Test Status

| Item | Status | Notes |
|------|--------|-------|
| CircleCI pipeline | ✅ | Ubuntu 22.04, CMake 3.28.6, GCC 11 |
| C++ build | ✅ | Core lib + all examples + test binaries |
| C++ unit tests | ✅ | command_registry, event_bus, types, fs_plugin, shell_plugin, shared_buffer, integration, pinhole_linux, window_close |
| WebGL E2E test | ✅ | SharedBuffer + WebGL render + readPixels, runs under xvfb (5 s watchdog) |
| JS bridge typecheck | ✅ | Separate CI job, `tsc --noEmit` |
| JS bridge unit tests (Vitest) | ✅ | 10+ files — config, invoke, events, modules, React hooks |
| Windows build (local) | ✅ | Win 11, VS 2022 Build Tools (MSVC 19.4x), vcpkg Boost 1.90, LibAsyik 1.8.1; core + hello-world + pinhole-hello + tests |
| Windows ctest (local) | ✅ | 12/12, 5 consecutive runs — incl. `window_close` (native WM_CLOSE) and `native_ipc` (WebView2 E2E) |
| Windows CI (`build-windows`) | ⏳ | CircleCI Server 2022 job added 2026-09-30, not yet run; runs `-LE display` |

---

## Next Priorities

1. **Land the Windows port safely** — the port touched shared code (`app.cpp` platform hooks, `window.cpp` Impl restructure, `shared_buffer.cpp` split, OS-chosen ports, LibAsyik 1.8.1 on Linux CI with a fresh `cpp-deps-v5` cache) that was only compiled on Windows. Confirm Linux CI (incl. xvfb display tests), get the first `build-windows` CircleCI run green, then enable `tests/native_ipc` on Linux.
2. **Windows follow-ups (Phase 7.1)** — zero-copy buffers in WebView2 (needs a hook into webview's environment creation); `anyar` CLI port; key-storage/video-player examples; Windows packaging (7.4); Pinhole DComp port (ADR-008, breaking).
3. **Productize the SharedBuffer WebProcess extension** — prototype in `benchmarks/shm_webext/` reads 1080p in ~0.4 ms vs ~21 ms via `anyar-shm://` (root cause: WebKit's 8 KB-chunked URI-scheme IPC). Needs packaging (DEB/AppImage), `App` wiring, `@libanyar/api/buffer` API + `FrameRenderer` use.
4. **Pinhole created before `Window::show()`** — gets a null overlay forever; `create_gl_area()` re-queues itself every idle (never renders, busy main loop). Fix: wire overlay on show, or create the GL area lazily.
5. **Migrate remaining plugin loops to `BackgroundTask`** — wifi-analyzer still uses a bare `execute()` loop + flag (no join); audit built-in plugins for blocking calls that should use `run_blocking()`.
6. **`anyar dev` real HMR** (deferred — release builds embed frontend; DX-only) — CLI starts Vite but the webview never loads the Vite URL (roadmap 5.2 "HMR ✅" is not true end-to-end).
7. **macOS spike** — LibAsyik Clang/macOS build before Phase 7.2 (Windows/MSVC verified 2026-09-30).
8. **Tier 3 DX/ecosystem** — `anyar dev --watch`, Todo App (React), File Explorer, migration runner.
9. **Tier 4** — Phase 7 macOS, Pinhole ports, Phase 8 plugins.

---

## Open Risks & Known Issues

| Risk / Issue | Severity | Status / Notes |
|------|----------|-------|
| Shutdown fragility | High | Fixed repeatedly (ADR-007, 2026-03-15; plugin-shutdown reorder, 2026-05-01). Plugins must stop their own `execute()` loops and unblock back-pressure waits in `shutdown()` — contract, not enforced. See [graceful-shutdown.md](graceful-shutdown.md). |
| WebGL test hangs without watchdog | Medium | ⚠️ Mitigated by 5 s `_exit()` watchdog — can mask real teardown hangs in CI. |
| `post_to_main_thread` deadlock | Medium | ⚠️ Documented only — never call from a fiber during shutdown (ADR-007). |
| LibAsyik Windows/macOS portability | Medium (macOS) | Windows ✅ verified 2026-09-30 with LibAsyik 1.8.1 + MSVC (ADR-010). macOS still assumed. |
| Windows port changes unverified on Linux | High (until CI runs) | `app.cpp`/`window.cpp`/`shared_buffer` refactors, port probing and the LibAsyik 1.8.1 bump were built and tested only on Windows. Linux CI must confirm before relying on them. |
| ~~Windows: non-ASCII paths~~ | ✅ Fixed 2026-09-30 | `<anyar/path.h>` (`path_from_utf8`/`path_to_utf8`/`is_path_within`) used by `fs:*`, `resolve_dist_path`, `allow_file_access`, `serve_file`. Third-party plugins must use it too (`fs::path(std::string)` is ANSI on Windows). |
| ~~`/__anyar__/file/` + `anyar-file://` never decoded URLs; string-prefix root check~~ | ✅ Fixed 2026-09-30 | Files with spaces/non-ASCII names were unreachable on every platform; root `/data` admitted `/database/…`. Now `percent_decode()` then `..` check, and component-wise `is_path_within()`. |
| Windows: SharedBuffer over HTTP | Medium | No `anyar-shm://` in WebView2 yet → one copy + loopback TCP per fetch; `anyar-file://` unavailable (use `/__anyar__/file/`). |
| Windows: first page load deferred to `run()` | Low | WebView2 only applies bind/init scripts to later navigations, so a window created before the main loop starts loads nothing until `run()` (child windows created at runtime load in `show()`). |
| Windows: CLI, 3 examples, Pinhole, packaging | Medium | Not ported: `anyar` CLI (POSIX), key-storage / video-player / wifi-analyzer, Pinhole (stub), MSI/NSIS. |
| ~~Random server ports hit reserved/in-use ports~~ | ✅ Fixed 2026-09-30 | `App` and tests now take an OS-chosen port (Windows reserves blocks of 49152–65535; a failed bind inside a test fiber hung `run()` ~13% of runs). |
| Pinhole Windows port is a breaking change | Medium | Requires WebView2 visual hosting → major version bump (ADR-008). |
| Pinhole native path in CI | Medium | Unverified whether CI (xvfb/mesa) exercises native GL or only the canvas fallback. |
| Pinhole CSS/scroll limitations | Low (by design) | Flat rect; hides during scroll; z-sibling detection best-effort (ADR-008). |
| CLI has no unit tests | Low–Medium | Exercised only indirectly via examples. |
| SharedBuffer `anyar-shm://` far slower than documented | High | ~21 ms per 1080p fetch (≈370 MB/s): WebKit streams custom-scheme responses in 8 KB IPC chunks (not fixable from our side); JSC Gigacage forbids true zero-copy. Webext prototype: ~0.4 ms (one memcpy). |
| ~~Pinhole idle callbacks hold raw pointers~~ | ✅ Fixed 2026-09-24 | All `g_idle_add` tasks + GTK signal handlers go through an `ImplGuard` liveness token; ASAN regression test in `test_pinhole_linux`. |
| Pinhole created before `Window::show()` never renders | Medium | Null overlay captured at create; `create_gl_area()` spins on idle re-queue. `App::on_window_ready` path is safe (runs after show). |
| Cold startup ~2.3 s | Medium | Warm is ~330 ms to first IPC (target <500 ms ✅); ~2.1 s cold spent before window exists. |
| Pinhole draw above target | Low | 0.68 ms CPU p50 per 1080p RGBA frame vs <0.2 ms target; PBO upload is the likely fix. |
| `anyar dev` HMR not wired | Medium | Vite starts, but webview loads the backend's static `dist/`. |
| Headless `App::run()` skips plugin `shutdown()` | Medium | Reported by doc-sync review; unverified. |
| ADR-007/008 shutdown steps stale | Low | Still cite "5b `pinholes.clear()`"; code now uses `notify_window_destroyed()` (reported, unverified). |
| pinhole-hello in fallback mode | Low | Relies on `set_continuous(true)`, a no-op in fallback → ~1 frame shown. |
| ~~Single service thread, no offload path for commands~~ | ✅ Fixed 2026-09-24 | `anyar::run_blocking()` (worker pool, fiber-suspending) + real async `add_async` (reply from any thread/fiber). Model unchanged: one service thread, blocking work offloaded (ADR-009). |
| ~~No safe frame handoff from fibers to Pinhole `on_render`~~ | ✅ Fixed 2026-09-24 | `FrameMailbox` (ref-counted latest frame) + `PinholeRenderContext::draw_frame()`. |
| `SharedBufferPool` names are process-global | Low | Documented; clear error on collision. Per-session base names recommended (video-player does this). New `try_acquire_write`, `release_unpublished`, `buffer(name)`; transitions validated. |
| ~~No Range-capable media serving~~ | ✅ Fixed 2026-09-24 | `anyar::serve_file()`: 206/416, always the FULL requested range; streaming overload `serve_file(server, req, path)` writes 256 KiB chunks read on the worker pool (bounded memory). `/__anyar__/file` and video-player use it; `anyar-file://` streams from disk (still no Range — WebKit URI-scheme limitation). |
| ~~No join handle for plugin background fibers~~ | ✅ Fixed 2026-09-24 | `anyar::BackgroundTask` (stop token + join, restartable). Other examples (wifi-analyzer) still use bare `execute()` loops — migrate when touched. |
| Fibers alive at `service_->stop()` make process exit spin | Medium | LibAsyik behaviour (observed in tests). Mitigation: join all loops (`BackgroundTask::stop()`) in plugin `shutdown()`; tests must join helper fibers before `svc->stop()`. |
| `draw_image()` 4:2:0 size check under-counted odd sizes | ✅ Fixed 2026-09-24 | Was `w*h*3/2` while uploads read ⌈w/2⌉×⌈h/2⌉ chroma → over-read on odd dimensions; now `pixel_format_byte_size()`. |
| WebKitGTK `<audio>` wedges after flushing seeks on high-latency audio devices | Medium (upstream) | Reproduced with `PULSE_LATENCY_MSEC=2000` on a null sink: element stays "playing", `currentTime` frozen forever; 20 ms latency → never. Worse when the source also carries a high-bitrate video track. video-player mitigates: audio-only remux source, fresh pipeline per seek, 3 s stall watchdog. Not yet reported upstream; other apps using `<audio>`/`<video>` seeking on Linux are exposed. |
| `SharedBufferPool::release_write()` semantic change | Low | Now only WRITING→READY (was unconditional). Callers must acquire first — all in-tree callers do. |
---

## Session Log

### Sessions 1–4 (2026-03-07)
- Project creation, Phase 1 + Phase 2 implementation
- Created docs (README, ARCHITECTURE, roadmap) and AI context files
- Fixed setup-ubuntu.sh, libasyik integration, webview C API migration
- JS bridge: dual ESM/CJS build, React hooks, module APIs

### Sessions 5–N (2026-03-07 → 2026-03-14)
- Phases 3–4f implemented (native APIs, plugins, DB, test suite, hybrid IPC, multi-window, platform abstraction, SharedBuffer/WebGL)
- WiFi Analyzer example, theme redesign
- Framework maturity assessment → CI recommended as top priority

### CI & Shutdown Fix Sessions (2026-03-14/15)
- CircleCI config created and debugged (5+ iterations for build issues)
- WebGL E2E teardown fix: 3 push-debug cycles (local segfault → CI timeout #1 → CI timeout #2)
- ADR-007 documenting all 5 root causes and correct shutdown sequence
- AI context docs updated with shutdown model, progress refreshed

### JS Bridge Tests + Linux Packaging (2026-03-15)
- JS bridge unit tests: 112 tests across 10 files (Vitest + jsdom), integrated in CI
- Linux packaging: `cmd_package.cpp` — DEB + AppImage via `anyar build --package`
  - DEB: auto dependency detection (ldd → Debian pkg mapping), .desktop entry, icon, wrapper script
  - AppImage: auto-downloads linuxdeploy, creates AppDir, bundles all shared libs
  - Tested on hello-world example: DEB (4.5 MB), AppImage (75 MB)

### Embed Frontend into Binary (2026-03-15)
- cmrc (CMake Resource Compiler) integrated via `cmake/CMakeRC.cmake`
- `cmake/AnyarEmbed.cmake` helper: `anyar_embed_frontend(target, dist_dir)`
- `core/include/anyar/embed.h`: `make_embedded_resolver()` — cmrc-backed FileResolver
- `App::set_frontend_resolver(FileResolver)` added to app.h/app.cpp
- All 4 examples updated with `#ifdef ANYAR_EMBED_FRONTEND` conditional
- CLI `anyar build --embed` passes `-DANYAR_EMBED_FRONTEND=ON` to cmake
- Tested: hello-world embedded binary serves index.html from compiled-in resources

### EventBus Per-Window Sinks (2026-03-15)
- C++: `EventBus::add_window_sink(label, sink)`, `emit_to_window(label, event, payload)`, `set_global_listener(sink_id, enabled)`
- `App::emit_to(label, event, payload)` public API, `label_to_sink_` map, `global_listener_sinks_` set
- IPC commands: `anyar:emit_to_window`, `anyar:enable_global_listener`
- JS bridge: `emitTo(label, event, payload)`, `listenGlobal(event, handler)`,  `emitToWindow()` in window module
- Targeted event filtering: `listen()` skips events targeted at other windows; `listenGlobal()` receives all
- `EventMessage.target` field added to both C++ and TypeScript types
- 9 new C++ unit tests (18 total in test_event_bus), 6 new JS tests (118 total)
- Key-storage example updated to use `emitTo('main', ...)` for cross-window targeted events
- Fixed latent bug: event handler payload access (`msg.payload?.id` → `payload?.id`)

### SharedBuffer HTTP Fallback (2026-03-15)
- C++: HTTP GET `/__anyar__/buffer/<string>` endpoint in `App::start_server()` serves raw buffer bytes from `SharedBufferRegistry`
- Returns `application/octet-stream` with `Cache-Control: no-store`; 404 for unknown buffers
- JS: `fetchBuffer()` auto-detects runtime via `isNativeIpc()` — uses `anyar-shm://` in native webview, HTTP GET in browser dev mode
- `encodeURIComponent()` on buffer names for safe HTTP URL construction
- C++ integration test: 260 assertions (256 byte-level data integrity + status checks + 404 handling)
- JS tests: 4 new tests (native mode: name + URL passthrough + error; browser mode: HTTP URL + URL extraction + special chars + error)
- Total: 22 JS buffer tests, 18 C++ shared buffer test cases (812 assertions)

### Window Focused Event (2026-03-15)
- C++: `Window::FocusHandler` callback + `set_on_focus()` API, GTK `focus-in-event` signal connected in `connect_close_signals()`
- `App::run()` wires focus handler in `on_window_created` callback → emits `window:focused` with `{label}` payload
- Applies to both main window and child windows created via `window:create` IPC
- JS: `onWindowFocused(handler)` listener in window module, exported from index.ts
- 1 new JS test (123 total), window lifecycle events now complete: `created`, `closed`, `focused`
- C++ watch mode (`anyar dev --watch`) moved from Tier 2 → Tier 3 (nice-to-have)

### DX Feedback Fixes (2026-03-18 → 2026-03-29)
- Getting-started quality pass; `App::http_get()/http_post()` (always beat `serve_static`), `App::on_ready()`, `App::allow_file_access()` + `anyar-file://` scheme with path-traversal checks; `set_on_server_ready()` deprecated
- CLI templates updated to the new APIs
- video-player: supports video-only files (no audio stream)

### Phase 4g — Pinhole Native Overlay (2026-04-29)
- `core/include/anyar/pinhole.h`, `pinhole_linux.cpp` (GtkOverlay + GtkGLArea, GL 3.3), `pinhole_stub.cpp`
- JS `@libanyar/api/pinhole` rect tracking (Mutation/Resize/IntersectionObserver, scroll-hide)
- Pixel formats rgba/rgb/bgra/grayscale/yuv420/nv12/nv21; transparent canvas-2D fallback when GL unavailable
- ADR-008; `examples/pinhole-hello/`; video-player defaults to pinhole (`--mode=webgl` legacy); `docs/pinhole-rendering.md`
- Copilot/Claude AI context structure added (`.github/copilot-instructions.md`, per-module instructions, skills)

### Graceful Shutdown Fix (2026-05-01)
- Plugin `shutdown()` moved BEFORE `server_->close()` / `service_->stop()` so plugins can stop fibers while the service is alive (ADR-007 updated)
- SharedBufferPool close/cancel path for back-pressure waits; video-player plugin stops decode + closes pool
- New `tests/window_close/` regression test (close window while busy); `docs/graceful-shutdown.md`

### Docs Sync (2026-09-24)
- Fixed CLAUDE.md imports (`#import` → `@path`, Claude Code syntax); synced roadmap/progress/module instructions with code

### Performance Benchmarks (2026-09-24)
- `benchmarks/anyar_bench` (`-DANYAR_BUILD_BENCHMARKS=ON`): startup milestones, native/HTTP IPC, SharedBuffer shm/HTTP 1080p fetch, Pinhole 1080p draw, total RSS incl. WebKit processes
- Baseline (Ryzen 7 8845HS, WebKitGTK 2.50.4): first IPC ~330 ms warm / ~2.3 s cold; native IPC p50 ~0.11 ms; HTTP ~0.46 ms; shm 1080p ~21 ms ❌; Pinhole 0.68 ms CPU, 60 fps; ~620 MB RSS
- `docs/shared-memory-webgl.md` perf table flagged as unverified estimates

### Pinhole Lifetime Fix + SharedBuffer Root Cause (2026-09-24)
- Pinhole: new `ImplGuard` (shared liveness token, recursive mutex) — `post_main()` replaces every raw-pointer `g_idle_add`; realize/unrealize/render/child-position handlers connected via `g_signal_connect_data` with guard refs; `~Impl()` retires the guard (after `destroy()` on the main thread, before any access off-thread)
- Tests: 2 lifetime cases in `test_pinhole_linux` (first is heap-use-after-free under ASAN on pre-fix code); fixed stale fallback-JS assertion; `-DANYAR_ASAN=ON` CMake option (Boost.Fiber tests give `sigaltstack` false positives under ASAN)
- SharedBuffer: size sweep + XHR in bench → linear ~2.7 ms/MB; WebKit `WebKitURISchemeRequest` 8 KB read buffer confirmed; webext prototype `__anyar_shm_read` 0.35–0.40 ms / 1080p


### Video-Player Reliability Review + Platform Fixes (2026-09-24)
- Review root causes: bitrate-chart click passed chart-root x to uPlot `posToVal` (plot-relative) → seeks late by the y-axis gutter; frontend `playing` survived re-open; `/video/stream` read the whole remaining file per Range request on the service thread; audio kept running while video decoded from the keyframe; decode sessions could overlap across re-opens; Pinhole render read pool slots the decoder recycled/unmapped
- Framework (ADR-009): `<anyar/task.h>` `run_blocking` + `BackgroundTask`; `add_async` now truly async; `<anyar/frame_mailbox.h>` `FrameMailbox`/`Frame` + `PinholeRenderContext::draw_frame`; `<anyar/pixel_format.h>` (enum moved out of pinhole.h, `pixel_format_byte_size` with ceil chroma — fixes odd-size over-read in `draw_image`); `<anyar/http_file.h>` `serve_file` + `parse_range_header`; `anyar-file://` streams from disk; `SharedBufferPool` `try_acquire_write`/`release_unpublished`/`buffer()` + validated transitions
- video-player rewritten on these: RAII FFmpeg, per-file `BackgroundTask` session (stop+join on re-open), worker-pool decode/analysis, acknowledged frame-accurate seeks (`video:seeked`, keyframe back-off + byte-0 fallback for MPEG-TS), container start-time origin, swscale→YUV420P fallback, per-session WebGL pool names; frontend `{#key}` remount, seek protocol, fixed chart/cursor/waveform alignment
- Tests: new `test_task`, `test_frame_mailbox`, `test_http_file`; extended `test_command_registry`, `test_shared_buffer` — 13/13 ctest green. Scripted app stress (xvfb, HTTP IPC + WS events; 60 random re-open/seek/close cycles, MPEG-TS, odd 4:2:0/4:2:2, video-only, 300 s 720p analysis): 23/23 pinhole, 24/24 webgl, zero ASAN reports
- Follow-up (same day): user-reported stuck playback after random seeks (audio files) traced to the 4 MB open-range cap in `serve_file` — WebKitGTK media does not request the rest of a shortened 206 (audio stalls; later seek → MEDIA_ERR_DECODE). Reproduced in a real WebKitGTK harness (`<audio>`, synthetic user click for unmuted play). Fix: full ranges always; new streaming `serve_file(server, req, path)` (direct response, 256 KiB chunks via worker pool). Stress: 284 MB range streamed in 0.41 s with IPC ≤2 ms; ASAN clean. Note: rapid-seek `<audio>` freezes with 1080p-track MP4s also occur under xvfb with any server (incl. a reference Python server) — environment artifact, not seen on real desktop
- Follow-up 2: freezes persisted on the user's machine. Built a real-UI stress test (`examples/video-player/test/run_seek_stress.sh`: injected driver, real GDK clicks, `VIDEO_PLAYER_DEBUG=1` backend trace). Backend was healthy throughout (seeks 22–177 ms, frames tracked the clock); the `<audio>` element wedged. Trigger isolated to audio-device latency (user: onboard ALSA, 2 s buffer): null sink @20 ms → 0 freezes, @2000 ms → freezes; flushing seeks after sustained playback wedge WebKitGTK's pipeline permanently. Old vs new seek protocol: no difference. Fix: audio-only Matroska remux for `<audio>`, fresh pipeline per timeline seek (0 freezes where others froze 6/18), 3 s stall watchdog with pipeline rebuild. Lesson: muting the test stream at PulseAudio level feeds back into WebKit and invalidates results — use a null sink + `PULSE_LATENCY_MSEC`

### dist_path Resolution Fix (2026-09-30)
- Non-embedded apps showed "No frontend build found" when launched from any directory other than the one containing `dist/` (relative `dist_path` was resolved against the cwd only)
- New `anyar::resolve_dist_path()` (`app_config.h`): cwd first, then next to the executable (`/proc/self/exe`); error page and stderr now list every path tried and suggest `--embed`
- Test: `resolve_dist_path` case in `test_integration` (absolute, exe-dir, cwd-wins, not-found)

### CI Fixes (2026-09-30)
- Build step `make -j4` → `-j2`: CircleCI `medium` (2 vCPU / 4 GB) OOM-killed cc1plus on `test_integration`
- `test_pinhole_linux` and `test_window_close` create webviews but ran in the headless unit step (failing since 4g/shutdown-fix landed). Display tests now carry ctest label `display`; CI runs `-LE display` headless and `-L display` under xvfb. Two window-creating pinhole cases mis-tagged `[headless]` are now display-gated
- `test_window_close` then failed in CI only: `app.run()` never returned after a native close, with "1 fiber(s) still active after 1s drain". CI built LibAsyik **1.6.1**; dev machines had **1.7.1** (scheduler cancellation #32 + HTTP connection tracking #33, which terminate open keep-alive connection fibers on stop). Not reproducible locally even with CPU starvation + 1 ms close. CI and `setup-ubuntu.sh` now pin LibAsyik 1.7.1 (cache key `cpp-deps-v4`); documented minimum raised to 1.7.1. `test_window_close` watchdog now times shutdown only (6 s after close) and logs phase timestamps


### Windows Port — Phase 7 Kick-off (2026-09-30)
- Toolchain: LibAsyik **1.8.1** (first MSVC release) + vcpkg (Boost 1.90, OpenSSL 3, SOCI, nlohmann-json, `webview2` header); new `scripts/setup-windows.ps1` builds/installs LibAsyik (Release + Debug `d` postfix) into `build-deps/libasyik`. Linux CI + `setup-ubuntu.sh` also pinned to 1.8.1 (cache key `cpp-deps-v5`)
- Core (ADR-010): private `core/src/platform.h` hooks (`platform_{linux,win32}.cpp`, `main_thread_{linux,win32}.cpp`) replace the Linux `#ifdef`s in `app.cpp`; `shared_buffer.cpp` split out (factory/registry/pool) from the per-OS mapping; Win32 `Window::Impl` (HWND subclass, owned/modal windows, centering, close confirmation); `main_thread_win32.cpp` (message-only dispatch window, thread-safe sticky quit); `shared_buffer_win32.cpp` (file mapping); `plugins/{dialog,clipboard,shell}_win32.cpp`
- Three WebView2 behaviour differences found and handled: `webview_terminate()` only quits the calling thread; webview's nested loops swallow `WM_QUIT`; bind/init scripts miss navigations issued before them (`window.__anyar_ipc__` was undefined — first navigation now deferred until setup completes)
- JS: `window.__LIBANYAR_SHM_SCHEME__` (false on Windows) → `fetchBuffer()` uses the HTTP fallback; Vitest cases added (not run locally — no Node on the Windows box)
- Ports: `App::find_available_port()` + tests (`tests/test_port.h`) take an OS-chosen port — random picks landed in Windows' excluded ranges (`netsh int ipv4 show excludedportrange`) or on RPC listeners; bind threw inside the test fiber and `run()` hung
- Tests: shell tests use `cmd.exe` builtins on Windows; `window_close` ported (WM_CLOSE); new `tests/native_ipc` E2E (generated page: IPC round-trip, event push, buffer fetch, UI-thread hop, cross-thread `window:close-all`). Local: 12/12 ctest × 5 runs; shutdown 71 ms after native close
- CI: new CircleCI `build-windows` job (Server 2022, pinned vcpkg commit, `-LE display`) — not yet run
- Not ported: Pinhole (stub), `anyar` CLI, key-storage / video-player / wifi-analyzer, packaging, zero-copy buffers
