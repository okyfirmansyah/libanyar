# LibAnyar — Progress Tracker

> **Current Phase**: Post Phase 4f — CI achieved, hardening in progress
> **Phase Status**: 🟢 Phases 1–4f complete, Phases 5–6 partial, CI green
> **Last Updated**: 2026-03-15

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
| 5 | CLI Tool | 🟡 Partial |
| 6 | Polish & Documentation | 🟡 Partial |

See [PLAN.md](PLAN.md) for full per-task checklists.

---

## CI / Test Status

| Item | Status | Notes |
|------|--------|-------|
| CircleCI pipeline | ✅ | Ubuntu 22.04, CMake 3.28.6, GCC 11 |
| C++ build | ✅ | Core lib + all examples + test binaries |
| C++ unit tests (8/8) | ✅ | app, window, ipc, event, plugin, js-bridge-inject, multi-window, webgl |
| WebGL E2E test | ✅ | SharedBuffer + WebGL render + readPixels, runs under xvfb |
| JS bridge typecheck | ✅ | Separate CI job, `tsc --noEmit` |
| JS bridge unit tests (Vitest) | ✅ | 118 tests / 10 files — config, invoke, events, modules, React hooks |

---

## Recent Milestones

### WebGL E2E Teardown Fix (2026-03-14/15)
Fixed 5 root causes causing segfaults and CI timeouts. See ADR-007.

1. **`~Impl()` destruction order** — set `destroyed = true` BEFORE `webview_destroy()`
2. **GBytes use-after-free** — `g_bytes_new_with_free_func()` with shared_ptr capture
3. **App shutdown reorder** — drain GTK → close server → stop service → close_all windows
4. **Stale dispatch guard** — `is_destroyed()` check on all IPC callbacks
5. **Unbounded GTK drain loops** — capped to 200 iterations (xvfb generates infinite events)

### CircleCI Setup (2026-03-14)
- Two jobs: `build-and-test` + `js-bridge-typecheck`
- Pinned CMake 3.28.6 (avoids FindBoost removal in CMake 4.x)
- Combined cache key (apt + CMake + npm)
- WebGL test under xvfb with 5-second watchdog thread

### SharedBuffer IPC & WebGL Canvas (Phase 4f)
- Zero-copy shared memory pools with `anyar-shm://` URI scheme handler
- Pixel format support (RGBA8, RGB8, BGRA8, GRAY8, RGBAF32)
- WebGL Canvas component rendering from shared buffer
- WiFi Analyzer example demonstrating real-time heatmap visualization

### Theme/Palette Redesign
- All 4 examples (hello-world, todo-app, file-explorer, wifi-analyzer) redesigned
- Consistent dark theme, modern UI

---

## Next Priorities

From [PLAN.md — Next Steps](PLAN.md#next-steps-prioritized):

**Tier 1 — Ship-Ready Linux:**
1. ~~Fix WebGL E2E teardown segfault~~ ✅
2. ~~JS bridge unit tests (Vitest)~~ ✅ — 112 tests, 10 files
3. ~~Linux packaging (DEB + AppImage)~~ ✅ — `anyar build --package deb|appimage|all`, auto deps, linuxdeploy
4. ~~Embed frontend into binary (cmrc)~~ ✅ — `anyar build --embed`, cmrc resource compiler, FileResolver API
5. Performance benchmarks

**Tier 2 — Feature Completeness:**
6. ~~EventBus per-window sinks~~ ✅ — `add_window_sink()`, `emit_to_window()`, `set_global_listener()`, JS `emitTo()`, `listenGlobal()`
7. ~~SharedBuffer HTTP fallback~~ ✅ — HTTP GET `/__anyar__/buffer/<name>` endpoint; JS `fetchBuffer()` auto-detects native vs browser mode
8. ~~`window:focused` event~~ ✅ — GTK `focus-in-event` → `window:focused` event, JS `onWindowFocused()`

**Tier 3 — Ecosystem Growth:**
9. C++ watch mode (`anyar dev --watch`)

---

## Known Issues

| Issue | Status | Notes |
|-------|--------|-------|
| WebGL test hangs without watchdog | ⚠️ Mitigated | 5-second `_exit()` watchdog in test as safety net |
| `post_to_main_thread` deadlock risk | ⚠️ Documented | Never call from fiber during shutdown — see ADR-007 |

---

## Session Log

### Sessions 1–4 (2026-03-07)
- Project creation, Phase 1 + Phase 2 implementation
- Created docs (README, ARCHITECTURE, PLAN) and .ai/ context files
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
- `.ai/` docs updated with shutdown model, progress refreshed

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
