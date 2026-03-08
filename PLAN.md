# LibAnyar — Implementation Plan

> Last updated: 2026-03-08

## Phase Overview

| Phase | Title | Status | Est. Duration |
|-------|-------|--------|---------------|
| 1 | [Core Prototype (Linux)](#phase-1-core-prototype-linux) | ✅ Complete | 2-3 weeks |
| 2 | [JS Bridge NPM Package](#phase-2-js-bridge-npm-package) | ✅ Complete | 1-2 weeks |
| 3 | [Native APIs & Plugins](#phase-3-native-apis--plugins) | ✅ Complete | 2-3 weeks |
| 4 | [Database Integration](#phase-4-database-integration) | ✅ Complete | 1-2 weeks |
| 4b | [Test Suite](#phase-4b-test-suite) | ✅ Complete | 1 week |
| 4c | [Hybrid IPC (Native + HTTP Fallback)](#phase-4c-hybrid-ipc) | ✅ Complete | 1 day |
| 5 | [CLI Tool](#phase-5-cli-tool) | 🔲 Not Started | 2-3 weeks |
| 6 | [Polish & Documentation](#phase-6-polish--documentation) | 🔲 Not Started | Ongoing |
| 7 | [Windows & macOS Support](#phase-7-windows--macos-support) | 🔲 Not Started | 3-4 weeks |
| 8 | [Plugin System & Packaging](#phase-8-plugin-system--packaging) | 🔲 Not Started | 2-3 weeks |

---

## Phase 1: Core Prototype (Linux)

> **Goal**: A working app that shows a React frontend in a native webview, with bidirectional IPC to C++ backend, all powered by LibAsyik.

### 1.1 Project Skeleton & Build System
- [x] Create root `CMakeLists.txt` with project definition, C++17, options
- [x] Set up `core/CMakeLists.txt` for the libanyar static library
- [x] Configure `third_party/` — vendored webview/webview (70 headers), nlohmann_json via system install
- [x] Integrate LibAsyik via `find_package(libasyik)` + `LIBASYIK_SOURCE_DIR` dev mode
- [x] Create a minimal `examples/hello-world/CMakeLists.txt` that links to libanyar
- [x] Verify the whole project compiles (GCC 11.4, CMake 3.22)

### 1.2 LibAsyik HTTP Server + Static File Serving
- [x] Implement `anyar::App` constructor — creates `asyik::service`
- [x] Implement `anyar::App::run()` — starts service thread + main loop
- [x] Implement static file server — custom regex catch-all with MIME, SPA fallback, path traversal guard
- [x] Pick random available port for localhost binding
- [x] Test: HTTP server starts, serves static files from dist/

### 1.3 WebView Integration
- [x] Integrate `webview/webview` as `third_party/webview/` (70 headers, C API)
- [x] Implement `anyar::Window` — wraps webview_t handle via pimpl, RAII destroy
- [ ] Implement `anyar::WindowManager` — deferred to Phase 3 (single window via App suffices)
- [x] Navigate webview to `http://127.0.0.1:<port>/`
- [x] Handle main thread requirement — webview_run() on main, asyik on spawned thread
- [x] Implement graceful shutdown — closing window stops service
- [x] Test: Binary starts, HTTP server binds port, webview navigates (needs display for GUI)

### 1.4 IPC Router (Command Channel)
- [x] Define JSON IPC protocol types (`IpcRequest`, `IpcResponse`, `EventMessage`)
- [x] Implement `anyar::CommandRegistry` — map of command names → sync/async handlers
- [x] Implement HTTP POST handler on `/__anyar__/invoke` with CORS
- [x] Parse JSON request, dispatch to registry, return JSON response
- [x] Error handling — unknown command, handler exception, JSON parse error
- [x] Test: IPC routes registered and compile

### 1.5 Event Bus (WebSocket Channel)
- [x] Implement `anyar::EventBus` — pub/sub with mutex + atomic ID counter
- [x] Implement WebSocket endpoint `/__anyar_ws__` via LibAsyik on_websocket
- [x] Each connected frontend gets a fiber + WS sink for push
- [x] Backend → Frontend: `app.emit("event", payload)` pushes to all windows via WS sinks
- [x] Frontend → Backend: WS message parsed and dispatched to C++ subscribers
- [x] Test: Event bus compiles, WS routes registered

### 1.6 Hello World Example
- [x] Create `examples/hello-world/src-cpp/main.cpp` with greet, get_info, increment commands
- [x] Create `examples/hello-world/frontend/` with Vite+React app + standalone dist/index.html
- [x] Frontend calls `invoke("greet", {name})`, displays result
- [x] Frontend listens for events via WebSocket bridge
- [ ] Backend has a periodic fiber emitting counter events (enhancement)
- [ ] Document build & run steps in example README

### Phase 1 Deliverable
> A native Linux window displaying a React UI, with working request-response IPC and real-time events between React and C++.

---

## Phase 2: JS Bridge NPM Package

> **Goal**: A proper TypeScript NPM package that frontend developers import to communicate with LibAnyar backend.

### 2.1 Package Setup
- [x] Create `js-bridge/` with `package.json`, `tsconfig.json`, build tooling
- [x] Package name: `@libanyar/api`
- [x] Configure build: TypeScript → ESM + CJS + type declarations
- [x] Set up exports map in package.json

### 2.2 Core API
- [x] `invoke<T>(cmd: string, args?: object): Promise<T>` — HTTP POST IPC
- [x] `listen(event: string, handler: fn): UnlistenFn` — WebSocket subscription
- [x] `emit(event: string, payload?: any): void` — WebSocket send to backend
- [x] `onReady(fn): void` — called when IPC channels are established
- [x] Auto-detect port from `window.__LIBANYAR_PORT__` or meta tag

### 2.3 Module APIs (Typed Wrappers)
- [x] `@libanyar/api/fs` — `readFile`, `writeFile`, `readDir`, `exists`, `mkdir`, `remove`, `metadata`
- [x] `@libanyar/api/dialog` — `open`, `save`, `message`, `confirm`
- [x] `@libanyar/api/shell` — `openUrl`, `openPath`, `execute`
- [x] `@libanyar/api/event` — `listen`, `emit`, `once`, `onReady`
- [x] `@libanyar/api/db` — `openDatabase`, `query`, `exec`, `batch`

### 2.4 React Hooks
- [x] `useInvoke(cmd, args)` — returns `{ data, loading, error, refetch }`
- [x] `useEvent(event)` — returns latest event payload, re-renders on new events
- [x] `useEventCallback(event, handler)` — stable callback (no re-renders)

### Phase 2 Deliverable
> `@libanyar/api` can be `npm install`ed into any React/Vue/Svelte project and provides full typed access to LibAnyar backend.

---

## Phase 3: Native APIs & Plugins

> **Goal**: Platform-native features (dialogs, tray, clipboard) exposed as commands callable from frontend.

### 3.1 Plugin Infrastructure
- [x] Define `IAnyarPlugin` interface and `PluginContext`
- [x] Implement plugin registration in `anyar::App::use()`
- [x] Plugins register commands and event handlers during `initialize()`
- [x] Built-in plugins auto-registered by default

### 3.2 File System Plugin
- [x] `fs:readFile` — read text/binary file (UTF-8 + base64 encoding)
- [x] `fs:writeFile` — write text/binary file
- [x] `fs:readDir` — list directory contents (returns DirEntry[])
- [x] `fs:exists` — check if path exists
- [x] `fs:mkdir` — create directory (recursive)
- [x] `fs:remove` — remove file/directory (optional recursive)
- [x] `fs:metadata` — file size, modified time, is_directory, is_file
- [x] Path validation / security checks (validate_path, resolve helpers)

### 3.3 Dialog Plugin
- [x] `dialog:open` — file open dialog (GTK3 native — no extra dependency)
- [x] `dialog:save` — file save dialog (overwrite confirmation)
- [x] `dialog:message` — message box (info/warning/error)
- [x] `dialog:confirm` — yes/no confirmation dialog

### 3.4 Shell Plugin
- [x] `shell:openUrl` — open URL in default browser (xdg-open)
- [x] `shell:openPath` — open file/folder with OS default app (xdg-open)
- [x] `shell:execute` — run subprocess via fork/exec, return stdout/stderr + exit code

### 3.5 Clipboard Plugin
- [x] `clipboard:read` — read text from clipboard (GTK3)
- [x] `clipboard:write` — write text to clipboard (GTK3 + gtk_clipboard_store)

### Phase 3 Deliverable
> Frontend can open file dialogs, read/write files, access clipboard, and launch URLs — all via `invoke()`.

---

## Phase 4: Database Integration

> **Goal**: SQLite and PostgreSQL accessible from frontend via LibAsyik's SOCI integration.

### 4.1 Database Plugin
- [x] `db:open` — open/create SQLite database (or connect to PostgreSQL) via `make_sql_pool()`
- [x] `db:close` — close a connection pool by handle
- [x] `db:query` — execute SELECT, return rows as JSON array with column names
- [x] `db:exec` — execute INSERT/UPDATE/DELETE, return affected rows
- [x] `db:batch` — execute multiple statements in a SOCI transaction
- [x] Connection pooling via LibAsyik `make_sql_pool()` (configurable pool size)
- [x] Parameterized queries ($1, $2, ... → SOCI :p1, :p2, ... with type-aware binding)
- [x] Dynamic row → JSON conversion (handles string, int, double, long long, date, NULL)

### 4.2 JS Bridge Database API
- [x] `@libanyar/api/db` — `openDatabase`, `closeDatabase`, `query`, `exec`, `batch`
- [ ] Type-safe query builder (optional, stretch goal)

### 4.3 Migration Support (Stretch)
- [ ] Simple migration runner — SQL files in `migrations/` folder
- [ ] Auto-run on app start if enabled

### Phase 4 Deliverable
> Full-stack desktop app with SQLite persistence can be built in ~50 lines of C++ and React.

---

## Phase 4b: Test Suite

> **Goal**: Comprehensive test coverage for all unit-testable components, plus integration tests for server-dependent features. Uses Catch2 (already vendored with LibAsyik).

### Testability Tiers

| Tier | Description | Components |
|------|-------------|------------|
| 1 | **Pure unit tests** — no LibAsyik, no GTK | CommandRegistry, EventBus, IPC types, FsPlugin |
| 2 | **Unit tests with lightweight deps** — real processes, temp files | ShellPlugin (shell:execute) |
| 3 | **Integration tests** — needs LibAsyik service/fiber context | IpcRouter, DbPlugin, App headless |
| 4 | **Not testable in CI** — needs GTK/display server | Window, DialogPlugin, ClipboardPlugin, gtk_dispatch |

### 4b.1 Test Infrastructure
- [x] Create `tests/` directory with CMakeLists.txt
- [x] Use Catch2 single-header from LibAsyik vendored path
- [x] Wire into root CMake via `ANYAR_BUILD_TESTS` option + CTest
- [x] Create `run.sh`-style test runner that cleans snap env

### 4b.2 Core Unit Tests (`test_command_registry.cpp`)
- [x] Register sync handler → dispatch returns correct result
- [x] Register async handler → dispatch returns correct result
- [x] Dispatch unknown command → returns error response
- [x] `has()` returns true for registered, false for unknown
- [x] Handler that throws → dispatch returns error with message
- [x] Overwrite handler → latest handler wins

### 4b.3 Event Bus Unit Tests (`test_event_bus.cpp`)
- [x] Subscribe + emit → handler receives correct payload
- [x] Multiple subscribers → all called on emit
- [x] Unsubscribe → handler no longer called
- [x] Emit with no subscribers → no crash
- [x] WS sink: add sink → emit sends serialized JSON to sink
- [x] WS sink: remove sink → no longer receives events
- [x] `on_ws_message()` → dispatches to C++ subscribers

### 4b.4 IPC Types Unit Tests (`test_types.cpp`)
- [x] `IpcResponse::to_json()` serialization roundtrip
- [x] `EventMessage::to_json()` / `from_json()` roundtrip
- [x] `EventMessage::from_json()` with missing fields

### 4b.5 FsPlugin Unit Tests (`test_fs_plugin.cpp`)
- [x] `fs:writeFile` + `fs:readFile` roundtrip in temp directory
- [x] `fs:exists` → true for existing, false for missing
- [x] `fs:mkdir` creates nested directory
- [x] `fs:readDir` returns correct entries
- [x] `fs:remove` deletes file, recursive deletes directory
- [x] `fs:metadata` returns size, isFile, isDirectory
- [x] `fs:readFile` on non-existent file → error

### 4b.6 ShellPlugin Unit Tests (`test_shell_plugin.cpp`)
- [x] `shell:execute` runs `echo hello` → stdout = "hello\n", code = 0
- [x] `shell:execute` with args → correct output
- [x] `shell:execute` bad command → non-zero exit or error
- [x] `shell:execute` with cwd option

### 4b.7 Integration Tests (`test_integration.cpp`)
- [x] IpcRouter: HTTP POST to `/api/invoke` dispatches command + returns JSON
- [x] DbPlugin: open SQLite in-memory → exec CREATE TABLE → query → verify rows
- [x] App headless mode: start → register command → HTTP invoke → stop

### Phase 4b Deliverable
> `make test` (or `ctest`) runs all unit + integration tests in CI without a display server. Tests cover CommandRegistry, EventBus, IPC types, FsPlugin, ShellPlugin, IpcRouter, DbPlugin, and App headless.

---

## Phase 5: CLI Tool

> **Goal**: Developer-friendly CLI for scaffolding, development, and building. Linux-first — full end-to-end polishing before cross-platform.

### 5.1 `anyar init`
- [x] Interactive project scaffolding (name, template, frontend framework)
- [x] Templates: svelte-ts, react-ts, vanilla
- [x] Generate CMakeLists.txt, frontend/, src-cpp/main.cpp

### 5.2 `anyar dev`
- [x] Start frontend dev server (vite) + C++ backend simultaneously
- [x] Hot reload for frontend (Vite HMR)
- [ ] Watch mode for C++ — rebuild on changes (optional, via CMake)

### 5.3 `anyar build`
- [x] Build frontend (npm run build)
- [x] Build C++ backend (cmake --build)
- [ ] Optionally embed frontend into binary (cmrc)
- [ ] Linux packaging (DEB, AppImage)

### Phase 5 Deliverable
> Developers can `anyar init myapp && cd myapp && anyar dev` to start building immediately.

---

## Phase 6: Polish & Documentation

> **Goal**: Production-ready quality on Linux before expanding to other platforms.

### 6.1 Documentation
- [x] API reference (Doxygen for C++, TypeDoc for JS)
- [x] Guide: Getting Started
- [x] Guide: Building Your First App
- [x] Guide: Writing Plugins
- [x] Guide: Database Integration
- [x] Architecture overview for contributors

### 6.2 Examples
- [x] Hello World (greet command) — exists with README
- [ ] Todo App (SQLite CRUD + React)
- [ ] File Explorer (native dialogs + file system)
- [ ] Markdown Editor (file read/write + live preview)
- [ ] Chat App (WebSocket events demonstration)

### 6.3 Testing
- [ ] Unit tests for core components (Catch2)
- [ ] Integration tests (webview + server + IPC)
- [ ] JS bridge unit tests (Vitest)
- [ ] Linux CI validation

### 6.4 Performance
- [ ] Benchmark: startup time (target < 500ms)
- [ ] Benchmark: IPC latency (target < 1ms)
- [ ] Benchmark: memory footprint
- [ ] Profile and optimize hot paths

### Phase 6 Deliverable
> Well-documented, well-tested, production-ready framework on Linux.

---

## Phase 7: Windows & macOS Support

> **Goal**: Cross-platform compatibility — expanding a polished Linux foundation to other OSes.

### 7.1 Windows
- [ ] WebView2 integration (webview/webview handles most of this)
- [ ] Windows-specific native APIs (dialogs, tray, clipboard, notifications)
- [ ] CMake + MSVC build support
- [ ] Test on Windows 10/11
- [ ] Bundle WebView2 bootstrapper for systems without Edge

### 7.2 macOS
- [ ] WKWebView integration (webview/webview handles most of this)
- [ ] macOS-specific native APIs (Objective-C++ wrappers)
- [ ] CMake + Clang/AppleClang build support
- [ ] Test on macOS 12+
- [ ] App bundle (.app) structure

### 7.3 CI/CD
- [ ] GitHub Actions matrix: Linux (GCC), Windows (MSVC), macOS (Clang)
- [ ] Build + test on all platforms per commit
- [ ] Artifact publishing

### 7.4 Cross-Platform Packaging
- [ ] MSI/NSIS (Windows), DMG (macOS) — extends Phase 5 Linux packaging

### Phase 7 Deliverable
> LibAnyar apps compile and run on Linux, Windows, and macOS.

---

## Phase 8: Plugin System & Packaging

> **Goal**: Extensible plugin architecture and distribution.

### 8.1 Dynamic Plugin Loading
- [ ] Load `.so`/`.dll`/`.dylib` at runtime via dlopen/LoadLibrary
- [ ] Plugin discovery from `plugins/` directory
- [ ] Plugin manifest (JSON) for metadata and dependencies

### 8.2 System Tray Plugin
- [ ] Cross-platform tray icon support
- [ ] Context menu from JSON definition
- [ ] Tray events forwarded to frontend

### 8.3 Notification Plugin
- [ ] Cross-platform native notifications
- [ ] `notification:send` command

### 8.4 Auto-Updater Plugin
- [ ] Check for updates via LibAsyik HTTP client
- [ ] Download + verify update package
- [ ] Apply update and restart

### Phase 8 Deliverable
> Feature-complete framework with plugin ecosystem and distribution tooling.

---

## Accepted Scope Boundaries

Things we **do** build:
- Core framework library (libanyar)
- JS bridge NPM package
- Built-in plugins (fs, dialog, shell, clipboard, db)
- CLI tool for scaffolding and building
- Project templates
- Examples

Things we **don't** build (rely on existing):
- HTTP server/client → LibAsyik
- WebSocket server/client → LibAsyik
- Database driver → LibAsyik SOCI
- Fiber concurrency → LibAsyik / Boost.Fiber
- Native webview bindings → webview/webview (`webview_bind`, `webview_return`, `webview_eval`)
- Web rendering engine → OS WebView
- Frontend build tools → Vite/Webpack (user's choice)

---

## Phase 4c: Hybrid IPC

> **Goal**: Replace HTTP-only IPC with Tauri-class native IPC as the primary channel, keeping HTTP/WebSocket as a fallback for browser-based development.

### 4c.1 Window Native IPC Plumbing
- [x] Add `Window::bind(name, callback)` wrapping `webview_bind()`
- [x] Add `Window::return_result(seq, status, result)` wrapping `webview_return()`
- [x] Add `Window::init(js)` wrapping `webview_init()`
- [x] Store callbacks via `std::unique_ptr<BindCallback>` to ensure stable pointers

### 4c.2 EventBus Native Support
- [x] Add `EventBus::emit_local()` for C++ subscribers only (no frontend push)
- [x] Used to prevent echo loop when JS emits events via native IPC

### 4c.3 App Native IPC Setup
- [x] `App::setup_native_ipc()` called before `window_->run()`
- [x] Bind `__anyar_ipc__` global function in webview (commands)
- [x] Register native event push sink via `EventBus::add_ws_sink()`
- [x] Inject `window.__LIBANYAR_NATIVE__ = true` and `__anyar_dispatch_event__` via `webview_init()`
- [x] Add `anyar:emit_event` command for JS→C++ event forwarding
- [x] Threading: bind callback on GTK thread → dispatch to fiber → dispatch result back to GTK thread

### 4c.4 JS Bridge Updates
- [x] `invoke.ts`: Native IPC primary (`window.__anyar_ipc__`), HTTP POST fallback
- [x] `events.ts`: Native event bridge primary (`__anyar_dispatch_event__`), WebSocket fallback
- [x] `config.ts`: Add `isNativeIpc()` export
- [x] `index.ts`: Export `isNativeIpc` and add to `window.__anyar__`

### 4c.5 Verification
- [x] All 6 existing tests pass (they use HTTP path, which is unchanged)
- [x] Both example frontends (hello-world, video-player) rebuilt successfully
- [x] Architecture documentation updated
