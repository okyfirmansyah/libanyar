# LibAnyar — Implementation Plan

> Last updated: 2026-03-14 (CI achieved)

## Phase Overview

| Phase | Title | Status | Est. Duration |
|-------|-------|--------|---------------|
| 1 | [Core Prototype (Linux)](#phase-1-core-prototype-linux) | ✅ Complete | 2-3 weeks |
| 2 | [JS Bridge NPM Package](#phase-2-js-bridge-npm-package) | ✅ Complete | 1-2 weeks |
| 3 | [Native APIs & Plugins](#phase-3-native-apis--plugins) | ✅ Complete | 2-3 weeks |
| 4 | [Database Integration](#phase-4-database-integration) | ✅ Complete | 1-2 weeks |
| 4b | [Test Suite](#phase-4b-test-suite) | ✅ Complete | 1 week |
| 4c | [Hybrid IPC (Native + HTTP Fallback)](#phase-4c-hybrid-ipc) | ✅ Complete | 1 day |
| 4d | [Multi-Window & Child Windows](#phase-4d-multi-window--child-windows) | ✅ Complete | 2-3 weeks |
| 4e | [Platform Abstraction Refactor](#phase-4e-platform-abstraction-refactor) | ✅ Complete | 3-5 days |
| 4f | [Shared Memory IPC & WebGL Canvas](#phase-4f-shared-memory-ipc--webgl-canvas) | ✅ Complete | 2-3 weeks |
| 5 | [CLI Tool](#phase-5-cli-tool) | 🟡 Partial | 2-3 weeks |
| 6 | [Polish & Documentation](#phase-6-polish--documentation) | 🟡 Partial | Ongoing |
| **→** | **[Next Steps (Prioritized)](#next-steps-prioritized)** | **🎯 Active** | — |
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
- [x] Implement `anyar::WindowManager` — implemented in Phase 4d (multi-window support)
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
- [x] Document build & run steps in example README

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

## Phase 4d: Multi-Window & Child Windows

> **Goal**: Enable creating multiple webview windows from C++ or JS — including modal dialogs, popup/child windows, and separate top-level windows. This is a foundational desktop framework capability that Tauri, Electron, and Qt all provide.

### Research: WRY & Tauri Multi-Window Comparison

> Research conducted before implementation to validate our design against production-grade frameworks.

#### WRY (Low-Level Webview Rendering)

WRY is Tauri's low-level webview rendering library (analogous to our use of `webview/webview`). Key findings:

1. **Window-agnostic**: WRY does NOT manage windows. It takes any `HasWindowHandle` and attaches a webview to it. Window creation/management is delegated to `tao` (or `winit`).
2. **`WebViewBuilder` pattern**: `WebViewBuilder::new().with_url("...").build(&window)` — webview attaches to an externally-created window.
3. **Child webviews**: `build_as_child(&window)` creates a webview as child *inside* the same window (for embedding multiple webviews in one window — NOT separate popup windows).
4. **`with_new_window_req_handler`**: Handles `window.open()` from JS → returns `NewWindowResponse::Allow | Create { webview } | Deny`.
5. **`NewWindowOpener` / `NewWindowFeatures`**: Contains opener webview reference + target size/position. On Linux, new webview must use `with_related_view()` to share the web process with the opener.
6. **Linux specifics**: `WebViewBuilderExtUnix::build_gtk(&container)` to build into any GTK container; `with_related_view(webkit2gtk::WebView)` for sharing web process between views.
7. **`WebContext`**: Shared web context across multiple webviews for cookie/storage sharing.
8. **`WebViewId`**: String ID (`&str`) passed to callbacks — same concept as our planned string labels.
9. **Per-webview IPC**: `ipc_handler` set per `WebViewBuilder` — IPC is per-webview, not global.
10. **`reparent()`**: Move webview between windows (Linux: between GTK containers).

#### Tauri (Application Framework)

Tauri sits on top of WRY + tao, providing the high-level multi-window API. Key findings:

1. **Three-tier architecture**: `Window` (native window) + `Webview` (web content) + `WebviewWindow` (convenience 1:1 combo). We will start with 1:1 (like `WebviewWindow`).
2. **Label-based identification**: Every window/webview has a unique string `label` (alphanumeric + `-/:_`). Static helpers: `getByLabel()`, `getAll()`, `getCurrent()`.
3. **`WebviewWindowBuilder` pattern**: `WebviewWindowBuilder::new(manager, "label", url)` composes `WindowBuilder` + `WebviewBuilder` internally. Match confirmed with our `WindowCreateOptions` design.
4. **Parent/child windows** (cross-platform):
   - `.parent(&webview_window)` — owned on Windows, **transient on Linux** (GTK `set_transient_for`), child on macOS
   - `.transient_for(&webview_window)` — Linux-specific explicit transient
   - `.owner(&webview_window)` — Windows-specific owned window
   - `.parent_raw(handle)` — platform-specific raw handle
5. **No explicit modal API**: Tauri does **NOT** have a `.modal(true)` method. Modality is faked by:
   - Calling `parent.set_enabled(false)` to disable input on parent window
   - Re-enabling parent when child closes
   - This is a gap we can improve upon with native GTK `set_modal()`.
6. **`on_new_window` handler**: Handles `window.open()` from JS → returns `NewWindowResponse::Allow | Create { window } | Deny`. Can pass `window_features(features)` to inherit position/size/related view from opener.
7. **Related views on Linux**: `with_related_view(webkit2gtk::WebView)` shares web process between webviews — **important for `window.open()` to work** correctly on WebKitGTK.
8. **Events**: Per-webview/per-window events with `EventTarget::Webview { label }`. `emitTo(target, event, payload)` for targeted events. `emit()` broadcasts to all.
9. **`on_close_requested` handler**: With `event.preventDefault()` to intercept and optionally cancel close.
10. **Desktop window features**: Full set of `setAlwaysOnTop`, `setAlwaysOnBottom`, `setSize`, `setPosition`, `center`, `setDecorations`, `setMinSize`, `setMaxSize`, `setEnabled`, `setFocusable`, etc.

#### Comparison with LibAnyar's Plan

| Feature | WRY | Tauri | LibAnyar (Phase 4d) |
|---------|-----|-------|---------------------|
| Window management | Delegated to tao/winit | `WindowBuilder` + manager | `WindowManager` in App |
| Webview per window | Separate `WebView` | `WebviewWindow` 1:1 combo | 1:1 via `webview_create()` |
| Label identification | `WebViewId` string | string label | string label ✅ |
| Parent/child | N/A (windowing lib) | `.parent()` → transient on Linux | `gtk_window_set_transient_for()` ✅ |
| Modal | N/A | **None** (fake via `set_enabled`) | **Native** `gtk_window_set_modal()` ✅ |
| `window.open()` handling | `NewWindowResponse` | `on_new_window` handler | Planned in 4d.5 |
| Related view (Linux) | `with_related_view()` | Via `window_features()` | **Need to add** |
| Per-window IPC | `ipc_handler` per builder | Per-webview command routing | Per-window `__anyar_ipc__` ✅ |
| Targeted events | N/A | `emitTo(label, event)` | `emit_to_window(label, event)` ✅ |
| Reparenting | `reparent()` | `reparent()` | Deferred (not MVP) |

#### Design Decisions from Research

1. **Native modal wins**: Tauri's lack of native modal is a known pain point. We use `gtk_window_set_modal()` which is properly supported by GTK — **this is our advantage**.
2. **Related views**: Add `with_related_view` support when creating child windows on Linux to share the WebKitGTK web process. Without this, `window.open()` from JS may not work. Add to §4d.2.
3. **Keep 1:1 window:webview**: Tauri v2 separates Window from Webview for multi-webview-per-window, but this is complex and rarely used. We keep 1:1 for simplicity.
4. **`on_close_requested` pattern**: Adopt Tauri's interceptable close pattern — emit `window:close-requested` event that JS can `preventDefault()` before allowing GTK `delete-event` to proceed.
5. **`window.open()` handler**: Defer to Phase 4d+ or later. Most LibAnyar apps will use `createWindow()` from JS explicitly rather than `window.open()`. Can add `on_new_window` later.
6. **`set_enabled` for pseudo-modal fallback**: Even with native modal, expose `set_enabled(bool)` as a window operation — useful for custom dimming/blocking patterns.
7. **Platform-neutral public API**: All public headers (`window.h`, `window_manager.h`, `main_thread.h`) must use **only LibAnyar/C++ types** — no GTK, Cocoa, or Win32 types in signatures. Platform-specific code stays in `.cpp` files behind pimpl or in platform-suffixed source files (e.g., `window_linux.cpp`, `window_win32.cpp`, `window_macos.mm`). This ensures Phase 7 only requires adding new implementation files, not changing the API.

---

### Cross-Platform Design Principle

The public API is **platform-neutral**. No GTK/Cocoa/Win32 types appear in any public header.

| Concern | Public API (header) | Linux Impl (.cpp) | Windows Impl (Phase 7) | macOS Impl (Phase 7) |
|---------|--------------------|--------------------|-------------------------|------------------------|
| Main-thread dispatch | `run_on_main_thread(fn)` | `g_idle_add()` | `PostMessage()` to UI thread | `dispatch_async(main_queue)` |
| Window handle | `Window::native_handle() → void*` | cast to `GtkWindow*` | cast to `HWND` | cast to `NSWindow*` |
| Parent/child | `Window::set_parent(Window&)` | `gtk_window_set_transient_for()` | `SetWindowLongPtr(GWL_HWNDPARENT)` | `addChildWindow:ordered:` |
| Modal | `Window::set_modal(bool)` | `gtk_window_set_modal()` | `EnableWindow(parent, FALSE)` + custom | `runModal` / sheet |
| Always-on-top | `Window::set_always_on_top(bool)` | `gtk_window_set_keep_above()` | `SetWindowPos(HWND_TOPMOST)` | `NSWindow.level = .floating` |
| Position | `Window::set_position(x, y)` | `gtk_window_move()` | `SetWindowPos()` | `setFrameOrigin:` |
| Close interception | `Window::on_close_requested` | `delete-event` signal | `WM_CLOSE` handler | `windowShouldClose:` |
| Dialogs | `dialog:open`, `dialog:save` | `GtkFileChooserDialog` | `IFileDialog` | `NSOpenPanel` / `NSSavePanel` |
| Clipboard | `clipboard:read/write` | `gtk_clipboard_get()` | `OpenClipboard()` / `SetClipboardData()` | `NSPasteboard` |

**File organization** (for Phase 4d, Linux-only; Phase 7 adds the other files):
```
core/include/anyar/
    window.h              ← platform-neutral public API
    window_manager.h      ← platform-neutral
    main_thread.h         ← replaces gtk_dispatch.h (platform-neutral)
core/src/
    window_linux.cpp      ← GTK/WebKitGTK implementation (Phase 4d)
    window_win32.cpp      ← Win32/WebView2 implementation (Phase 7)
    window_macos.mm       ← Cocoa/WKWebView implementation (Phase 7)
    main_thread_linux.cpp ← g_idle_add implementation
    main_thread_win32.cpp ← PostMessage implementation (Phase 7)
    main_thread_macos.mm  ← dispatch_async implementation (Phase 7)
    plugins/
        dialog_linux.cpp      ← GtkFileChooser (current, rename from dialog_plugin.cpp)
        dialog_win32.cpp      ← IFileDialog (Phase 7)
        dialog_macos.mm       ← NSOpenPanel (Phase 7)
        clipboard_linux.cpp   ← GtkClipboard (current, rename from clipboard_plugin.cpp)
        clipboard_win32.cpp   ← Win32 clipboard (Phase 7)
        clipboard_macos.mm    ← NSPasteboard (Phase 7)
```

### Design Overview

**Architecture**: Each window is a native platform window + webview pair (on Linux: `GtkWindow` + `WebKitWebView`; on Windows: `HWND` + `WebView2`; on macOS: `NSWindow` + `WKWebView`), managed by a central `WindowManager` inside `App`. Windows are identified by a string label (e.g., `"main"`, `"settings"`, `"entry-detail"`). The first window is always `"main"`.

**Window Types**:
| Type | Behavior | Use Case |
|------|----------|----------|
| **Top-level** | Independent window, no parent | Secondary app windows |
| **Child (transient)** | Stays on top of parent, minimizes with parent | Settings, preferences |
| **Modal** | Blocks interaction with parent until closed | Confirmations, detail editors |

**Threading**: All GTK/WebKit calls run on the main thread via `g_idle_add` (existing `run_on_gtk_main` pattern). Window creation/destruction dispatched from fibers to GTK main loop.

**IPC**: Each child window connects to the **same** LibAsyik HTTP server on the same port, with its own native IPC bridge (`__anyar_ipc__`) bound per webview instance. Events can be targeted to specific windows or broadcast to all.

### 4d.1 WindowManager Core (C++)

> Replaces the single `std::unique_ptr<Window> window_` in `App` with a multi-window manager.

- [x] Create `core/include/anyar/window_manager.h` — `WindowManager` class
  - `std::map<std::string, std::shared_ptr<Window>> windows_` — label → window
  - `create(label, WindowCreateOptions) → std::string` — create & show window, return label
  - `close(label)` — close and destroy a specific window
  - `close_all()` — close all windows (called on app shutdown)
  - `get(label) → Window*` — get window by label, nullptr if not found
  - `main_window() → Window*` — shortcut for `get("main")`
  - `labels() → std::vector<std::string>` — list all open window labels
  - `count() → size_t` — number of open windows
  - `on_window_closed` callback — notified when any window is closed (for cleanup)
- [x] `WindowCreateOptions` struct (extends `WindowConfig`):
  - Inherits: `title`, `width`, `height`, `resizable`, `decorations`, `debug`
  - New: `parent` (string, label of parent window — empty = top-level)
  - New: `modal` (bool, default false — if true + parent set, blocks parent)
  - New: `url` (string, optional — path or full URL; default = `"/"` = app root)
  - New: `center` (bool, default true — center on screen or on parent)
  - New: `alwaysOnTop` (bool, default false)
  - New: `closable` (bool, default true)
  - New: `minimizable` (bool, default true)

### 4d.2 Window Class Enhancements (C++)

> Extend the existing `Window` class with a **platform-neutral public API**. No GTK/Cocoa/Win32 types in public headers. Platform-specific calls live in `window_linux.cpp` (pimpl).

Public API (platform-neutral `window.h`):
- [x] `Window::native_handle() → void*` — returns opaque native handle (caller casts per platform)
- [x] `Window::set_parent(Window& parent)` — establish parent/child relationship (takes our own `Window`, not a GTK type)
- [x] `Window::set_modal(bool)` — block interaction with parent
- [x] `Window::set_enabled(bool)` — enable/disable input on this window (pseudo-modal fallback)
- [x] `Window::set_always_on_top(bool)` — keep above other windows
- [x] `Window::set_closable(bool)` — allow/prevent user closing
- [x] `Window::set_position(int x, int y)` — move window
- [x] `Window::center_on_parent()` — center relative to parent (or screen if no parent)
- [x] `Window::on_close` callback — invoked when window is destroyed
- [x] `Window::on_close_requested` interceptable callback — JS can `preventDefault()` to cancel close
- [x] Change `Window` constructor to accept `WindowCreateOptions` instead of `WindowConfig`

Linux implementation (`window_linux.cpp`, inside `Window::Impl`):
- [x] `native_handle()` → `webview_get_window(wv)` → returns `GtkWindow*` as `void*`
- [x] Internal `webkit_view()` → `webview_get_native_handle(BROWSER_CONTROLLER)` → `WebKitWebView*` (for related view sharing, NOT public)
- [x] `set_parent(Window&)` → extract parent's native handle → `gtk_window_set_transient_for()`
- [x] `set_modal(true)` → `gtk_window_set_modal()`
- [x] `set_enabled(bool)` → `gtk_widget_set_sensitive()`
- [x] `set_always_on_top(bool)` → `gtk_window_set_keep_above()`
- [x] `set_closable(false)` → connect GTK `delete-event` signal → return `TRUE` to block
- [x] `set_position(x, y)` → `gtk_window_move()`
- [x] `on_close_requested` → GTK `delete-event` signal handler; emit `window:close-requested` event
- [x] When creating child window: use `webkit_web_view_new_with_related_view()` to share web process with parent (related view pattern from WRY)
- [x] Navigating child windows: navigate to `http://127.0.0.1:<port>/<url_path>` (same server, different route)

### 4d.3 App Integration

> Wire `WindowManager` into `App`, replacing the single-window code.

- [x] Replace `std::unique_ptr<Window> window_` with `WindowManager window_mgr_`
- [x] Update `App::create_window()` to delegate to `WindowManager::create("main", ...)`
- [x] Update `App::run()`:
  - Create main window via `window_mgr_.create("main", ...)`
  - Set up native IPC for main window
  - Run platform main loop — on Linux: `gtk_main()` (single loop for all windows, **not** `webview_run()`)
  - Monitor open window count — when all windows closed, stop service
- [x] Update `App::setup_native_ipc()` → `App::setup_native_ipc(Window* w)`
  - Bind `__anyar_ipc__` per window instance
  - Register per-window event sink
  - Inject `window.__LIBANYAR_WINDOW_LABEL__` so frontend knows its own label
- [x] Register built-in window management commands (see §4d.5)
- [x] Event routing: emit to specific window label or broadcast to all (`"*"`)
- [x] `on_window_closed` handler: if `"main"` closes → close all windows + stop app

### 4d.4 Platform Main Loop Abstraction

> Currently `webview_run(wv)` calls `gtk_main()` internally for the single window. With multiple windows we need direct control over the platform main loop.

**Rename `gtk_dispatch.h` → `main_thread.h`** (platform-neutral public API):
- [x] Create `core/include/anyar/main_thread.h` — exports `run_on_main_thread(F&& fn)` template (same signature as current `run_on_gtk_main`)
- [x] Create `core/src/main_thread_linux.cpp` — implements via `g_idle_add()` + `boost::fibers::promise` (move current `gtk_dispatch.h` body here)
- [x] Update all callers: `run_on_gtk_main(...)` → `run_on_main_thread(...)`
- [x] Deprecate / remove `gtk_dispatch.h`

**Main loop takeover** (Linux-specific, in `app_linux.cpp` or behind `#ifdef`):
- [x] Research: verify `webview_run()` calls `gtk_main()` (read webview source)
- [x] Switch from `webview_run()` to `gtk_main()` directly on main thread
  - Create all windows via `webview_create()` + manual `gtk_widget_show_all()`
  - Run `gtk_main()` once for all windows
  - On last window close → `gtk_main_quit()`
- [x] Ensure `webview_dispatch()` still works (it uses `g_idle_add` internally — should be fine)
- [x] Alternative: keep first window via `webview_run()`, create child windows via GTK APIs alongside
  - Simpler, less invasive — child windows are native `GtkWindow` + `WebKitWebView`, not `webview_t`
  - Investigate as Plan B if `gtk_main()` takeover causes issues

**Phase 7 will add**: `main_thread_win32.cpp` (Win32 message pump), `main_thread_macos.mm` (NSRunLoop/dispatch_async)

### 4d.5 Window Commands (IPC)

> C++ commands callable from frontend JS to manage windows.

- [x] `window:create` — create a new window from frontend
  - Args: `{ label, title, width, height, url, parent, modal, resizable, center, alwaysOnTop }`
  - Returns: `{ label }` — the assigned label
  - If `label` already exists → error
- [x] `window:close` — close a specific window
  - Args: `{ label }` — if omitted, close the calling window
- [x] `window:close-all` — close all windows (triggers app shutdown)
- [x] `window:list` — list all open window labels with their config
  - Returns: `[{ label, title, width, height, modal, parent }]`
- [x] `window:set-title` — change a window's title
  - Args: `{ label, title }`
- [x] `window:set-size` — resize a window
  - Args: `{ label, width, height }`
- [x] `window:focus` — bring a window to front
  - Args: `{ label }`
- [x] `window:set-enabled` — enable/disable input on a window (pseudo-modal pattern)
  - Args: `{ label, enabled }`
- [x] `window:set-always-on-top` — toggle always-on-top
  - Args: `{ label, alwaysOnTop }`
- [x] `window:emit` — emit an event to a specific window (or all)
  - Args: `{ label, event, payload }` — label `"*"` = broadcast
- [x] `window:get-label` — returns the calling window's own label

### 4d.6 JS Bridge — Window Module

> TypeScript API for frontend developers: `@libanyar/api/window`

- [x] Create `js-bridge/src/modules/window.ts`:
  ```typescript
  // Create a new child/modal window
  createWindow(opts: WindowOptions): Promise<string>
  
  // Close a window (default: self)
  closeWindow(label?: string): Promise<void>
  
  // Close all windows (exit app)
  closeAll(): Promise<void>
  
  // List open windows
  listWindows(): Promise<WindowInfo[]>
  
  // Change title / size of any window
  setTitle(label: string, title: string): Promise<void>
  setSize(label: string, width: number, height: number): Promise<void>
  
  // Bring window to front
  focusWindow(label: string): Promise<void>
  
  // Enable/disable window input (pseudo-modal pattern)
  setEnabled(label: string, enabled: boolean): Promise<void>
  
  // Toggle always-on-top
  setAlwaysOnTop(label: string, alwaysOnTop: boolean): Promise<void>
  
  // Emit event to a specific window
  emitTo(label: string, event: string, payload?: any): Promise<void>
  
  // Get current window's label
  getLabel(): string  // sync — read from window.__LIBANYAR_WINDOW_LABEL__
  
  // Listen for window lifecycle events
  onClose(handler: () => void): UnlistenFn  // before this window closes
  onCloseRequested(handler: (event: CloseRequestedEvent) => void): UnlistenFn  // interceptable close (call event.preventDefault() to cancel)
  ```
- [x] `WindowOptions` interface:
  ```typescript
  interface WindowOptions {
    label: string;           // unique window identifier
    title?: string;          // window title (default: app name)
    width?: number;          // pixels (default: 800)
    height?: number;         // pixels (default: 600)
    url?: string;            // path to load (default: "/")
    parent?: string;         // parent window label (default: none)
    modal?: boolean;         // block parent (default: false) 
    resizable?: boolean;     // (default: true)
    center?: boolean;        // center on screen/parent (default: true)
    alwaysOnTop?: boolean;   // (default: false)
    decorations?: boolean;   // show title bar (default: true)
    closable?: boolean;      // allow user close (default: true)
    minimizable?: boolean;   // (default: true)
  }
  ```
- [x] Export from `js-bridge/src/index.ts`
- [x] Add to `window.__anyar__` global object

### 4d.7 Frontend Routing for Child Windows

> Child windows need to load different content than the main window. Two approaches supported:

**Approach A — URL path routing** (recommended for SPA):
- Child window navigates to `http://127.0.0.1:<port>/settings` or `/#/entry-detail`
- Frontend router (svelte-routing, react-router) renders the right component
- The static file server already has SPA fallback (`/index.html`)

**Approach B — Separate HTML entry** (for isolated windows):
- Vite multi-page: `frontend/src/popup.html` → built as separate entry
- Child window navigates to `http://127.0.0.1:<port>/popup.html`
- Useful when popup needs minimal bundle size

- [x] Document both approaches in `docs/multi-window.md`
- [x] Key-storage example: use Approach A — `/#/entry/:id` route for modal window

### 4d.8 Event System Enhancements

> Events need window-awareness for targeted delivery.

- [ ] Extend `EventBus` to support per-window sinks (keyed by label) *(currently uses generic broadcast to all ws_sinks_)*
  - `add_window_sink(label, sink_fn) → uint64_t`
  - `emit_to_window(label, event, payload)` — send to one window
  - `emit(event, payload)` — broadcast to all windows (existing behavior)
- [x] JS-side: `listen()` receives events for current window + broadcasts
- [ ] New: `listenGlobal(event, handler)` — listen to events from any window
- [x] Window lifecycle events (emitted automatically):
  - `window:created` — `{ label, title }` — broadcast when a window is created
  - `window:closed` — `{ label }` — broadcast when a window is closed
  - [ ] `window:focused` — `{ label }` — broadcast when a window gains focus *(not yet implemented)*

### 4d.9 Key-Storage Example: Native Modal Window

> Refactor the key-storage entry detail from CSS overlay to a native modal window, demonstrating the multi-window system.

- [x] Add URL-based routing to key-storage frontend (hash router in main.js)
- [x] Create `/#/entry/:id` route that renders `EntryDetailPage.svelte` standalone
- [x] On double-click entry in main window:
  ```typescript
  import { createWindow } from '@libanyar/api';
  await createWindow({
    label: `entry-${id}`,
    title: 'Entry Detail',
    url: `/#/entry/${id}`,
    parent: 'main',
    modal: true,
    width: 550,
    height: 700,
    resizable: true,
    center: true,
  });
  ```
- [x] Entry detail window: on save/delete → emit event to main window → close self
- [x] Keep CSS overlay as fallback for `anyar dev` (browser mode where native windows unavailable)

### Phase 4d Deliverable
> LibAnyar apps can create multiple native windows — top-level, transient child, or modal. Frontend developers use `createWindow()` / `closeWindow()` from JS. Events can be sent to specific windows. The key-storage example demonstrates a native modal entry editor.

### Implementation Order (recommended)

1. **4d.2** — Window class enhancements (GTK parent/modal/position APIs)
2. **4d.4** — GTK main loop change (prerequisite for multiple windows)
3. **4d.1** — WindowManager core implementation
4. **4d.3** — App integration (wire WindowManager, per-window IPC)
5. **4d.5** — Window IPC commands
6. **4d.8** — Event system per-window routing
7. **4d.6** — JS bridge window moduleok
8. **4d.7** — Documentation for frontend routing patterns
9. **4d.9** — Key-storage example refactor

---

## Phase 4e: Platform Abstraction Refactor

> **Goal**: Eliminate all platform-specific (GTK/Linux) coupling from public headers and cross-platform source files, so that Phase 7 (Windows & macOS) only requires adding new implementation files — no API changes, no `#ifdef` in business logic.
>
> **Timing**: Should be done alongside Phase 4d (or immediately before). Phase 4d already plans `main_thread.h` and platform-split `window_linux.cpp`, but there are **6 additional areas** of platform coupling outside multi-window that need fixing.

### Audit Results: Platform Coupling Inventory

| # | Component | File(s) | Problem | Severity |
|---|-----------|---------|---------|----------|
| 1 | **Main-thread dispatch** | `gtk_dispatch.h` (public header) | `#include <gtk/gtk.h>`, `g_idle_add`, `gboolean`, `gpointer` in public header. Every file that includes it gets GTK dependency. | 🔴 Critical |
| 2 | **Dialog plugin** | `dialog_plugin.cpp` | 100% GTK API: `GtkFileChooserDialog`, `gtk_dialog_run`, `gtk_message_dialog_new` | 🟡 Expected (impl) |
| 3 | **Clipboard plugin** | `clipboard_plugin.cpp` | 100% GTK API: `gtk_clipboard_get`, `GDK_SELECTION_CLIPBOARD` | 🟡 Expected (impl) |
| 4 | **Shell plugin** | `shell_plugin.cpp` | POSIX-only: `fork()`, `pipe()`, `execvp()`, `waitpid()`, `<sys/wait.h>`, `<unistd.h>`, `xdg-open` | 🔴 Critical |
| 5 | **App startup** | `app.cpp` | `sanitise_snap_env()` — Linux/snap-specific; `unsetenv()` is POSIX | 🟢 Low (Linux-only) |
| 6 | **CLI exe finder** | `cli/src/util.cpp` | `readlink("/proc/self/exe")` is Linux-only | 🟡 Medium |
| 7 | **CMakeLists.txt** | `core/CMakeLists.txt` | `webkit2gtk-4.0`, `gtk+-3.0`, `WEBVIEW_GTK=1` hard-coded unconditionally | 🔴 Critical |
| 8 | **Run script** | `run.sh` | Bash-only, `unset GTK_*`, Linux `LD_LIBRARY_PATH` | 🟢 Low (convenience) |
| 9 | **LibAsyik dependency** | All of `core/` | LibAsyik uses Boost.Asio + Boost.Fiber internally. It builds on Linux; Windows/macOS portability is **unverified**. This is the deepest dependency risk. | 🟠 Risk |

### 4e.1 Main-Thread Dispatch (Public Header → Platform-Split)

> **Currently**: `gtk_dispatch.h` is a **public header** with `#include <gtk/gtk.h>` and inline GTK code. Anyone who includes it (plugins, app.cpp) pulls in GTK headers.

- [x] Create `core/include/anyar/main_thread.h` — platform-neutral public API:
  ```cpp
  namespace anyar {
    // Execute fn on the UI main thread, block until done, return result.
    // Implementation: g_idle_add (Linux), PostMessage (Win32), dispatch_async (macOS).
    template<typename F>
    auto run_on_main_thread(F&& fn) -> decltype(fn());
  }
  ```
- [x] The template body needs the platform impl. Two approaches:
  - **A) Platform-specific header include** (simpler): `main_thread.h` includes an internal detail header conditionally:
    ```cpp
    #if defined(__linux__)
      #include "detail/main_thread_linux.h"   // g_idle_add
    #elif defined(_WIN32)
      #include "detail/main_thread_win32.h"   // PostMessage
    #elif defined(__APPLE__)
      #include "detail/main_thread_macos.h"   // dispatch_async
    #endif
    ```
  - **B) Type-erased non-template** (cleaner but runtime cost): `void run_on_main_thread_impl(std::function<void()>)` declared in header, implemented per-platform in `.cpp` files. Template wrapper calls it.
  - **→ Chose approach B**: `post_to_main_thread(std::function<void()>)` in header, implemented in `main_thread_linux.cpp`. Cleanest separation.
- [x] Rename all `run_on_gtk_main(...)` → `run_on_main_thread(...)` across codebase
- [x] Deprecate `gtk_dispatch.h` (kept as thin shim that forwards to `main_thread.h`)

### 4e.2 Dialog Plugin — Platform-Split Implementation

> **Currently**: `dialog_plugin.cpp` is 100% GTK. The **header** (`dialog_plugin.h`) is clean — only the `.cpp` needs splitting.

- [x] Rename `dialog_plugin.cpp` → `dialog_linux.cpp`
- [x] Create CMake `if(LINUX)` / `if(WIN32)` / `if(APPLE)` source selection in `core/CMakeLists.txt`
- [ ] Phase 7 adds: `dialog_win32.cpp` (`IFileDialog`, `TaskDialog`), `dialog_macos.mm` (`NSOpenPanel`, `NSSavePanel`, `NSAlert`)

### 4e.3 Clipboard Plugin — Platform-Split Implementation

> **Currently**: `clipboard_plugin.cpp` uses GTK clipboard. Header is clean.

- [x] Rename `clipboard_plugin.cpp` → `clipboard_linux.cpp`
- [ ] Phase 7 adds: `clipboard_win32.cpp` (`OpenClipboard`/`SetClipboardData`), `clipboard_macos.mm` (`NSPasteboard`)

### 4e.4 Shell Plugin — Cross-Platform Process Execution

> **Currently**: Uses `fork()`, `pipe()`, `execvp()`, `waitpid()` (POSIX-only), and `xdg-open` (Linux-only).

- [x] Split `shell_plugin.cpp` → `shell_linux.cpp`
- [ ] Abstract process execution:
  - `shell:openUrl` — Linux: `xdg-open`, Windows: `ShellExecuteW`, macOS: `open`
  - `shell:openPath` — same as above but with file paths
  - `shell:execute` — Linux: `fork/execvp`, Windows: `CreateProcess`, macOS: `posix_spawn` or `fork/execvp`
- [ ] Phase 7 adds: `shell_win32.cpp`, `shell_macos.cpp`

### 4e.5 App Startup — Platform-Conditional Initialization

> **Currently**: `sanitise_snap_env()` in `app.cpp` is Linux/snap-specific. Uses `unsetenv()` (POSIX).

- [x] Wrap `sanitise_snap_env()` call in `#ifdef __linux__` guard — refactored to `platform_init()` with `#ifdef __linux__` / `#else` no-op
- [x] Or extract to `core/src/platform_linux.cpp` with a `platform_init()` function — done inline in app.cpp with `#ifdef` guard
- [ ] Phase 7: Add `platform_win32.cpp` (COM init, DPI awareness), `platform_macos.mm` (NSApplication setup)

### 4e.6 CLI Executable Path Finder

> **Currently**: `cli/src/util.cpp` uses `readlink("/proc/self/exe")` behind `#ifdef __linux__` (already guarded!).

- [ ] Add `#elif defined(_WIN32)` block using `GetModuleFileNameW()`
- [ ] Add `#elif defined(__APPLE__)` block using `_NSGetExecutablePath()`
- [ ] Already partially abstracted — just needs the other platform cases

### 4e.7 CMake Platform Selection

> **Currently**: `core/CMakeLists.txt` unconditionally requires `webkit2gtk-4.0`, `gtk+-3.0`, and defines `WEBVIEW_GTK=1`.

- [x] Wrap GTK/WebKitGTK `pkg_check_modules` in `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` guard
- [ ] Add `if(WIN32)` block for WebView2 includes/libs
- [ ] Add `if(APPLE)` block for WebKit framework
- [x] Remove `WEBVIEW_GTK=1` define — `webview/webview`'s `macros.h` auto-detects platform
- [x] Select platform source files conditionally:
  ```cmake
  if(LINUX)
    target_sources(anyar_core PRIVATE
      src/main_thread_linux.cpp
      src/plugins/dialog_linux.cpp
      src/plugins/clipboard_linux.cpp
      src/plugins/shell_linux.cpp
      src/platform_linux.cpp
    )
    # ... GTK/WebKitGTK pkg_check_modules ...
  elseif(WIN32)
    target_sources(anyar_core PRIVATE
      src/main_thread_win32.cpp
      src/plugins/dialog_win32.cpp
      # ... etc
    )
  elseif(APPLE)
    target_sources(anyar_core PRIVATE
      src/main_thread_macos.mm
      src/plugins/dialog_macos.mm
      # ... etc
    )
  endif()
  ```

### 4e.8 LibAsyik Cross-Platform Risk Assessment

> **Assumption**: LibAsyik is portable. Its foundation (Boost.Beast + Boost.Fiber + Boost.Asio) is cross-platform, and we will assume it builds on Windows (MSVC) and macOS (Clang) without major changes. If issues arise during Phase 7, we will fork-and-fix.

- [x] ~~Verify LibAsyik builds on Windows/macOS~~ — **Assumed portable** (Boost underpinnings are cross-platform)
- N/A — alternatives evaluation deferred unless proven needed in Phase 7

### 4e.9 Run Script — Per-Platform Launcher

> **Currently**: `run.sh` is Bash with Linux-specific `unset GTK_*`, `LD_LIBRARY_PATH`.

- [ ] No code change needed now — just document it's Linux-only
- [ ] Phase 7: Add `run.bat` (Windows), `run.command` (macOS) if needed
- [ ] The CLI `anyar dev` already abstracts this; `run.sh` is a dev convenience script

### Phase 4e Implementation Strategy

**Phase 4e work is mostly renaming and wrapping — NOT rewriting implementations.**

The key principle: keep all existing Linux code working unchanged, just reorganize it so that Phase 7 can add parallel implementations without touching existing files.

| Priority | Task | Effort | Impact |
|----------|------|--------|--------|
| ~~**P0**~~ | ~~4e.1 main_thread.h~~ | ~~2h~~ | ✅ Done — `main_thread.h` + `main_thread_linux.cpp` + `gtk_dispatch.h` shim |
| ~~**P0**~~ | ~~4e.7 CMake guards~~ | ~~1h~~ | ✅ Done — `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`, removed `WEBVIEW_GTK=1` |
| ~~**P1**~~ | ~~4e.2 dialog_linux.cpp rename~~ | ~~30m~~ | ✅ Done |
| ~~**P1**~~ | ~~4e.3 clipboard_linux.cpp rename~~ | ~~30m~~ | ✅ Done |
| ~~**P1**~~ | ~~4e.4 shell_linux.cpp rename~~ | ~~30m~~ | ✅ Done |
| ~~**P1**~~ | ~~4e.5 platform_init guard~~ | ~~15m~~ | ✅ Done — `platform_init()` with `#ifdef __linux__` |
| **P2** (do in 7) | 4e.6 CLI exe finder | 30m | Already has `#ifdef __linux__` |
| ~~**P2**~~ | ~~4e.8 LibAsyik portability~~ | ~~N/A~~ | **Assumed portable** — revisit only if Phase 7 build fails |
| **Low** | 4e.9 run scripts | 15m | Dev convenience only |

### Phase 4e Deliverable
> All platform-specific code is confined to platform-suffixed source files (`*_linux.cpp`, `*_win32.cpp`, `*_macos.mm`). Public headers contain zero platform includes. The codebase compiles on Linux exactly as before, but is structurally ready for Phase 7 to add Windows and macOS implementations by adding files — not modifying existing ones.

---

## Phase 4f: Shared Memory IPC & WebGL Canvas

> **Goal**: Provide a high-performance binary data channel using shared memory (zero-copy where possible), and a WebGL-based frame renderer that consumes it. These are two separate APIs — `@libanyar/api/buffer` (general-purpose shared memory IPC) and `@libanyar/api/canvas` (WebGL frame rendering convenience layer).

### Context & Motivation

The existing IPC channels in LibAnyar are optimized for JSON messages:
- **Native IPC** (`webview_bind`/`webview_return`): JSON-string-only, ~0.01ms for small payloads
- **WebSocket binary**: Already used by video-player example for RGBA frames, ~1-5ms for <1MB
- **HTTP POST**: Request/response, JSON serialized

For large binary payloads (video frames, images, point clouds, ML tensors), these channels become bottlenecks:
- 1080p RGBA frame = ~8MB → WebSocket takes 3-8ms per frame (limits to ~30fps with overhead)
- 4K RGBA frame = ~33MB → WebSocket saturates at ~8fps
- JSON base64 encoding adds 33% overhead + CPU cost

Shared memory eliminates the copy entirely: C++ writes directly to a memory region that JS can read as an `ArrayBuffer`.

### Benchmark & Industry Research

Research was conducted across the Tauri/wry ecosystem and WebKit documentation:

**Tauri/wry findings (as of 2025-2026):**
1. **Tauri IPC is slow for big payloads** — Users report ~100ms for 12MB via `IPC::Response`, ~200ms for 3MB via JSON events ([tauri#13405](https://github.com/tauri-apps/tauri/issues/13405))
2. **SharedArrayBuffer does NOT help** — It only shares memory between JS main thread and Web Workers, not between the native backend and webview. Tauri maintainer confirmed this is a dead end ([tauri-apps/discussions#6269](https://github.com/orgs/tauri-apps/discussions/6269))
3. **wry sync IPC / shared memory** — Still open after 4+ years, on indefinite hold because "proper shared memory primitives are not well supported across enough target platforms" ([wry#454](https://github.com/tauri-apps/wry/issues/454))
4. **Practical workaround** — Users building LiDAR/image apps ended up using wgpu to render directly into the native window, bypassing the webview for heavy rendering
5. **WebView2 (Windows)** has `CreateSharedBuffer` + `PostSharedBufferToScript` — true zero-copy shared memory, official API since SDK 1.0.1661.34
6. **WebKitGTK (Linux)** — URI scheme handlers can stream binary data; JavaScriptCore API can create `ArrayBuffer` backed by native memory pointers (zero-copy)
7. **WKWebView (macOS)** — `WKURLSchemeHandler` for binary streaming; no shared buffer equivalent to WebView2

**WPE WebKit article** ([wpewebkit.org/blog/06-integrating-wpe](https://wpewebkit.org/blog/06-integrating-wpe.html)):
- Demonstrates custom URI scheme handlers streaming binary data via `GMemoryInputStream`
- Script message handlers for bidirectional communication
- `webkit_user_content_manager_register_script_message_handler_with_reply()` for Promise-based IPC (WPE WebKit 2.40+, applies to WebKitGTK too)

### Architecture

```
┌──────────────────────────────┐
│  @libanyar/api/canvas        │  ← Feature 2 (WebGL frame renderer)
│  createFrameRenderer()       │
│  drawFrame(buffer, meta)     │
└──────────┬───────────────────┘
           │ uses internally
┌──────────▼───────────────────┐
│  @libanyar/api/buffer        │  ← Feature 1 (shared memory IPC)
│  onSharedBuffer()            │
│  SharedBufferHandle          │
└──────────┬───────────────────┘
           │ uses internally
┌──────────▼───────────────────┐
│  Platform shared memory      │  ← C++ core (anyar::SharedBuffer)
│  Linux: shm_open + mmap      │
│  Win:   CreateSharedBuffer   │
│  macOS: shm_open + mmap      │
└──────────────────────────────┘
```

Feature 1 (`buffer`) is a standalone general-purpose API. Feature 2 (`canvas`) is a convenience layer that consumes Feature 1 internally.

### Design Decisions

1. **Two-tier approach**: WebSocket binary remains the fallback (works in dev mode with external browser). Shared memory is the high-performance path (requires webview).

2. **Platform-specific implementations behind uniform API**: Each platform uses its best available mechanism:

   | Platform | Shared Memory Mechanism | JS Access Method | Copies |
   |---|---|---|---|
   | **Linux (WebKitGTK)** | `shm_open()` + `mmap()` | Custom URI scheme (`anyar-shm://`) via `webkit_web_context_register_uri_scheme()` → `fetch()` → `ArrayBuffer`. Advanced: `jsc_value_new_array_buffer()` for true zero-copy | 0-1 |
   | **Windows (WebView2)** | `ICoreWebView2Environment12::CreateSharedBuffer()` | `PostSharedBufferToScript()` → `chrome.webview.addEventListener("sharedbufferreceived")` → `ArrayBuffer` | 0 (true zero-copy) |
   | **macOS (WKWebView)** | `shm_open()` + `mmap()` | `WKURLSchemeHandler` → `fetch()` → `ArrayBuffer` | 1 |

3. **Ring buffer protocol**: For streaming use cases (video), C++ maintains a ring of N shared buffers. A lightweight notification (via existing native IPC) tells JS which buffer index is ready. JS reads, renders, then signals release.

4. **WebGL for frame rendering**: The `canvas` module handles all WebGL boilerplate — texture creation, shader compilation, fullscreen quad. Supports RGBA and YUV420 (GPU-side color conversion saves ~60% bandwidth).

5. **Fallback for dev mode**: When running in an external browser (`vite dev`), shared memory is unavailable. The system falls back to WebSocket binary push, which already works.

### Performance Targets

| Metric | WebSocket (current) | Shared Memory (target) |
|---|---|---|
| 1080p RGBA (8MB) | ~3-8ms | **<0.5ms** |
| 4K RGBA (33MB) | ~15-40ms | **<1ms** |
| Max throughput | ~2 GB/s | **>10 GB/s** (memory bandwidth) |
| CPU overhead | memcpy + WS framing | Near zero (pointer handoff) |

### 4f.1 Feature 1: Shared Memory IPC — C++ Core (`anyar::SharedBuffer`)

#### 4f.1.1 SharedBuffer Class (Linux Implementation)
- [x] Add `core/include/anyar/shared_buffer.h` — platform-neutral public API:
  ```cpp
  namespace anyar {
  class SharedBuffer {
  public:
      static std::shared_ptr<SharedBuffer> create(const std::string& name, size_t size);
      ~SharedBuffer();  // unmaps + unlinks

      uint8_t* data();              // raw pointer to mapped memory
      const uint8_t* data() const;
      size_t size() const;
      const std::string& name() const;

      // Notify a specific window that buffer content is ready
      void post_to_window(Window& window, const std::string& metadata_json);

      // Notify all windows
      void post_to_all(App& app, const std::string& metadata_json);
  };
  } // namespace anyar
  ```
- [x] Add `core/src/shared_buffer_linux.cpp`:
  - `create()`: `shm_open(O_CREAT|O_RDWR)` → `ftruncate(size)` → `mmap(PROT_READ|PROT_WRITE, MAP_SHARED)`
  - Store `fd`, `ptr`, `size`, `shm_name` (auto-generated `/anyar_<pid>_<name>`)
  - Destructor: `munmap()` + `shm_unlink()`
  - `post_to_window()`: dispatches a lightweight event via native IPC telling JS the buffer name + metadata
- [ ] Phase 7 adds: `shared_buffer_win32.cpp` (WebView2 `CreateSharedBuffer`), `shared_buffer_macos.cpp` (POSIX shm)

#### 4f.1.2 SharedBuffer URI Scheme (Linux)
- [x] Register `anyar-shm://` custom URI scheme via `webkit_web_context_register_uri_scheme()` in `App` startup
- [x] Scheme handler: parses `anyar-shm://<buffer-name>` → looks up SharedBuffer by name → creates `GMemoryInputStream` from mapped pointer → responds with `application/octet-stream`
- [x] CORS headers for cross-origin fetch from `http://127.0.0.1:<port>`
- [ ] Phase 7: Windows uses `PostSharedBufferToScript()` (no URI scheme needed); macOS uses `WKURLSchemeHandler`

#### 4f.1.3 SharedBufferPool (Ring Buffer for Streaming)
- [x] Add `core/include/anyar/shared_buffer_pool.h`:
  ```cpp
  namespace anyar {
  class SharedBufferPool {
  public:
      SharedBufferPool(const std::string& base_name, size_t buffer_size, size_t count = 3);

      // Producer side (C++)
      SharedBuffer& acquire_write();   // get next writable buffer (blocks if all in use)
      void release_write(SharedBuffer& buf, const std::string& metadata_json, Window& window);

      // Consumer side (called by JS via IPC)
      void release_read(const std::string& buffer_name);  // JS signals it's done reading
  };
  } // namespace anyar
  ```
- [x] Implement with atomic index + semaphore/condition_variable for back-pressure
- [x] Prevents producer from overwriting a buffer JS hasn't finished reading

#### 4f.1.4 IPC Commands for Buffer Management
- [x] `buffer:create` — create a named shared buffer (returns name + size)
- [x] `buffer:release` — JS signals it has finished reading a buffer
- [x] `buffer:destroy` — cleanup a named buffer
- [x] `buffer:list` — list active shared buffers (for debugging)
- [x] Register commands in `App::register_builtin_commands()`

### 4f.2 Feature 1: Shared Memory IPC — JS Bridge (`@libanyar/api/buffer`)

#### 4f.2.1 Buffer Module
- [x] Add `js-bridge/src/modules/buffer.ts`:
  ```typescript
  export interface SharedBufferHandle {
      name: string;
      size: number;
      metadata: Record<string, any>;
      getBuffer(): Promise<ArrayBuffer>;  // fetches from anyar-shm:// or receives from shared buffer event
      release(): void;                    // signals C++ the buffer can be reused
  }

  // Subscribe to buffer-ready notifications from C++
  export function onSharedBuffer(
      name: string,
      handler: (handle: SharedBufferHandle) => void
  ): UnlistenFn;

  // One-shot: request a buffer's current content
  export function fetchBuffer(name: string): Promise<ArrayBuffer>;
  ```
- [x] `getBuffer()` implementation:
  - Native mode: `fetch("anyar-shm://<name>")` → `response.arrayBuffer()` (Linux/macOS); or direct `ArrayBuffer` from shared buffer event (Windows)
  - Fallback mode (external browser): `invoke("buffer:read", {name})` → base64 decode (slow but functional)
- [x] `release()`: calls `invoke("buffer:release", {name})` to signal C++ the buffer is free
- [x] Export from `@libanyar/api` main entry point

#### 4f.2.2 Platform Detection & Fallback
- [x] Detect if `anyar-shm://` scheme is available (native webview mode)
- [x] If not (dev server in browser), fall back to WebSocket binary or HTTP fetch
- [x] Transparent to consumer code — same `onSharedBuffer()` API regardless of transport

### 4f.3 Feature 2: WebGL Canvas Renderer (`@libanyar/api/canvas`)

#### 4f.3.1 Frame Renderer Module
- [x] Add `js-bridge/src/modules/canvas.ts`:
  ```typescript
  export interface FrameRendererOptions {
      format: 'rgba' | 'yuv420' | 'rgb' | 'nv12';
      fragmentShader?: string;   // custom GLSL override
      flipY?: boolean;           // default false
      premultiplyAlpha?: boolean;
  }

  export interface FrameRenderer {
      drawFrame(buffer: ArrayBuffer, meta: { width: number; height: number }): void;
      resize(width: number, height: number): void;
      destroy(): void;
  }

  export function createFrameRenderer(
      canvas: HTMLCanvasElement,
      options: FrameRendererOptions
  ): FrameRenderer;
  ```

#### 4f.3.2 WebGL Shader Pipeline
- [x] RGBA path: single `texImage2D()` upload → fullscreen quad with passthrough shader
- [x] YUV420 path: three separate textures (Y, U, V) → YUV→RGB conversion fragment shader:
  ```glsl
  uniform sampler2D y_tex, u_tex, v_tex;
  varying vec2 v_uv;
  void main() {
      float y = texture2D(y_tex, v_uv).r;
      float u = texture2D(u_tex, v_uv).r - 0.5;
      float v = texture2D(v_tex, v_uv).r - 0.5;
      gl_FragColor = vec4(
          y + 1.402 * v,
          y - 0.344 * u - 0.714 * v,
          y + 1.772 * u,
          1.0
      );
  }
  ```
- [x] NV12 path: two textures (Y plane, interleaved UV plane)
- [x] Custom shader support: user provides fragment shader, renderer handles vertex + texture setup
- [x] Vertex shader: fullscreen quad (two triangles), UV coordinates for texture mapping

#### 4f.3.3 Integration Helper
- [x] `createBufferRenderer()` — convenience that wires `onSharedBuffer()` to `drawFrame()` automatically:
  ```typescript
  export function createBufferRenderer(
      canvas: HTMLCanvasElement,
      bufferName: string,
      options: FrameRendererOptions
  ): { destroy: () => void };
  // Internally: onSharedBuffer(name, h => { renderer.drawFrame(h.getBuffer(), h.metadata); h.release(); })
  ```
- [x] Export from `@libanyar/api` main entry point and as `@libanyar/api/canvas`

### 4f.4 CMake & Build Integration
- [x] Add `shared_buffer_linux.cpp` to `core/CMakeLists.txt` under Linux guard
- [x] Link `-lrt` on Linux (required for `shm_open`)
- [ ] Phase 7 adds: `shared_buffer_win32.cpp`, `shared_buffer_macos.cpp`

### 4f.5 WebSocket Binary Fallback Path
- [ ] Ensure existing WebSocket binary push (`ws->write_basic_buffer()`) continues to work as-is
- [ ] `onSharedBuffer()` in fallback mode listens on a dedicated WebSocket channel for binary messages with a header prefix identifying the buffer name
- [ ] Video player example can optionally be updated to use the new buffer API, but existing WS path remains functional

### 4f.6 Testing & Validation
- [x] Unit test: create/destroy shared buffer, write/read data integrity
- [x] Unit test: SharedBufferPool acquire/release cycling
- [x] Integration test: C++ writes frame → JS fetches via `anyar-shm://` → validates content
- [ ] Performance test: measure latency for 8MB and 33MB buffers (target <0.5ms and <1ms)
- [ ] Fallback test: verify WebSocket binary path works when scheme is unavailable

### 4f.7 Example: SharedBuffer Video Player (Enhancement)
- [x] Optionally update video-player example to use `SharedBufferPool` instead of raw WebSocket
- [x] Add YUV420 mode: send Y/U/V planes as separate shared buffers, render with GPU shader
- [ ] Benchmark before/after: WebSocket RGBA vs SharedBuffer YUV420

### Implementation Order

| Step | Task | Dependency | Platform |
|------|------|------------|----------|
| 1 | `shared_buffer.h` + `shared_buffer_linux.cpp` | None | Linux |
| 2 | URI scheme handler (`anyar-shm://`) | Step 1 | Linux |
| 3 | `SharedBufferPool` | Step 1 | All |
| 4 | Buffer IPC commands | Step 1 | All |
| 5 | `@libanyar/api/buffer` JS module | Steps 2, 4 | All |
| 6 | `@libanyar/api/canvas` WebGL renderer | None (standalone) | All |
| 7 | `createBufferRenderer()` integration | Steps 5, 6 | All |
| 8 | Testing & benchmarks | Steps 1-7 | Linux |
| 9 | Video player enhancement (optional) | Steps 5, 6 | Linux |
| — | `shared_buffer_win32.cpp` | Step 1 | Phase 7 (Windows) |
| — | `shared_buffer_macos.cpp` | Step 1 | Phase 7 (macOS) |

### Phase 4f Deliverable
> Two new LibAnyar APIs — `@libanyar/api/buffer` for zero-copy shared memory IPC and `@libanyar/api/canvas` for WebGL frame rendering — working on Linux with WebSocket fallback for dev mode. Platform-specific implementations for Windows and macOS deferred to Phase 7.

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
- [x] Guide: Shared Memory & WebGL Canvas
- [x] Guide: Multi-Window
- [x] Architecture overview for contributors

### 6.2 Examples
- [x] Hello World (greet command) — exists with README
- [x] Key Storage (SQLite CRUD + Svelte + multi-window modal) — exists with README
- [x] Video Player (SharedBufferPool + WebGL canvas, RGBA/YUV420) — exists with README
- [x] WiFi Analyzer (passive/active scan, libnl, WebGL canvas) — exists with README
- [ ] Todo App (SQLite CRUD + React)
- [ ] File Explorer (native dialogs + file system)
- [ ] Markdown Editor (file read/write + live preview)
- [ ] Chat App (WebSocket events demonstration)

### 6.3 Testing
- [x] Unit tests for core components (Catch2) — 8 test files, all 8/8 pass (WebGL teardown segfault fixed)
- [x] Integration tests (webview + server + IPC) — SharedBuffer integration tests implemented
- [ ] JS bridge unit tests (Vitest)
- [x] Linux CI validation (CircleCI — Ubuntu 22.04, GCC 11)

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
> **Prerequisite**: Phase 4d's platform-neutral public API ensures Phase 7 only adds new implementation files, not API changes.

### 7.1 Windows
- [ ] Add `core/src/window_win32.cpp` — implement `Window::Impl` using `webview/webview` + Win32 APIs
- [ ] Add `core/src/main_thread_win32.cpp` — implement `run_on_main_thread()` via Win32 message pump
- [ ] Add `core/src/plugins/dialog_win32.cpp` — `IFileDialog` / `MessageBox`
- [ ] Add `core/src/plugins/clipboard_win32.cpp` — Win32 clipboard API
- [ ] WebView2 integration (webview/webview handles most of this)
- [ ] CMake + MSVC build support (conditional `pkg_check_modules` only on Linux)
- [ ] Test on Windows 10/11
- [ ] Bundle WebView2 bootstrapper for systems without Edge

### 7.2 macOS
- [ ] Add `core/src/window_macos.mm` — implement `Window::Impl` using `webview/webview` + Cocoa APIs
- [ ] Add `core/src/main_thread_macos.mm` — implement `run_on_main_thread()` via `dispatch_async(main_queue)`
- [ ] Add `core/src/plugins/dialog_macos.mm` — `NSOpenPanel` / `NSSavePanel` / `NSAlert`
- [ ] Add `core/src/plugins/clipboard_macos.mm` — `NSPasteboard`
- [ ] WKWebView integration (webview/webview handles most of this)
- [ ] CMake + Clang/AppleClang build support
- [ ] Test on macOS 12+
- [ ] App bundle (.app) structure

### 7.3 CMake Platform Selection
- [ ] `core/CMakeLists.txt` uses `if(LINUX)`, `if(WIN32)`, `if(APPLE)` to select platform source files
- [ ] GTK/WebKitGTK pkg-config only on Linux; Win32 libs on Windows; Cocoa frameworks on macOS
- [ ] Remove hard-coded `WEBVIEW_GTK=1` — let webview/webview auto-detect via its `macros.h`

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

---

## Next Steps (Prioritized)

> **Context**: Phases 1–4f are complete. Phase 5 (CLI) and Phase 6 (Polish) are partially done. Linux CI is operational (CircleCI). The framework is functionally complete on Linux — what remains is hardening, packaging, and expansion.

### Remaining Items Inventory

All unchecked `[ ]` items across the plan, categorized:

| Category | Items | Phases |
|----------|-------|--------|
| **Linux hardening** | WebGL test segfault fix, performance benchmarks, JS bridge tests | 6.3, 6.4 |
| **Incomplete features** | EventBus per-window sinks, `listenGlobal`, `window:focused` event | 4d.8 |
| **Dev experience** | C++ watch mode, embed frontend (cmrc), Linux packaging (DEB/AppImage) | 5.2, 5.3 |
| **Fallback paths** | SharedBuffer WebSocket fallback, SharedBuffer perf benchmarks | 4f.5, 4f.6, 4f.7 |
| **More examples** | Todo App, File Explorer, Markdown Editor, Chat App | 6.2 |
| **Stretch goals** | Type-safe query builder, migration runner, periodic fiber events | 4.2, 4.3, 1.6 |
| **Cross-platform** | All of Phase 7 (Windows/macOS) + Phase 8 (plugins/packaging) | 7, 8 |

### Recommended Priority Order

#### Tier 1 — Ship-Ready Linux (do next)

These items complete the Linux story — green CI, distributable binaries, quantified performance.

| # | Task | Phase | Effort | Why Now |
|---|------|-------|--------|----------|
| **1** | ~~**Fix WebGL E2E teardown segfault**~~ | 6.3 | ~~1-2d~~ | ✅ Done — fixed 4 root causes: `~Impl()` destruction order, stale `g_idle_add` drain, `GBytes` shared_ptr capture, `App::run()` shutdown sequence. 8/8 tests pass. |
| **2** | ~~**JS bridge unit tests (Vitest)**~~ | 6.3 | ~~2-3d~~ | ✅ Done — 112 tests across 10 files: config, invoke, events, fs, dialog, shell, db, buffer, window, React hooks. Added to CI pipeline. |
| **3** | **Linux packaging (DEB + AppImage)** | 5.3 | 2-3d | Makes apps distributable. `anyar build --package deb`. |
| **4** | **Embed frontend into binary (cmrc)** | 5.3 | 1-2d | Single-binary deployment, no external dist/ needed. |
| **5** | **Performance benchmarks** | 6.4 | 1-2d | Quantify startup, IPC latency, memory. Publish in README. |

#### Tier 2 — Feature Completeness

Gaps in implemented phases that should be closed before expanding.

| # | Task | Phase | Effort | Why |
|---|------|-------|--------|-----|
| **6** | **EventBus per-window sinks** | 4d.8 | 1d | Targeted events use broadcast workaround; proper impl for multi-window apps. |
| **7** | **SharedBuffer WebSocket fallback** | 4f.5 | 1-2d | `anyar dev` (browser mode) can't use `anyar-shm://`; needs WS path. |
| **8** | **C++ watch mode** (`anyar dev --watch`) | 5.2 | 1-2d | DX improvement — auto-rebuild C++ on save. |
| **9** | **`window:focused` event** | 4d.8 | 0.5d | Small gap in window lifecycle events. |

#### Tier 3 — Ecosystem Growth

More examples and stretch features that demonstrate breadth.

| # | Task | Phase | Effort | Notes |
|---|------|-------|--------|-------|
| **10** | **Todo App example** (React + SQLite) | 6.2 | 2-3d | Demonstrates the React path (current examples are Svelte-heavy). |
| **11** | **File Explorer example** | 6.2 | 2-3d | Showcases fs + dialog plugins working together. |
| **12** | **SharedBuffer perf benchmarks** | 4f.6-7 | 1d | Quantify improvement over WebSocket for marketing. |
| **13** | **Migration runner** | 4.3 | 1-2d | Convenience for SQLite-heavy apps. |

#### Tier 4 — Cross-Platform Expansion

Only after Linux is fully polished.

| # | Task | Phase | Effort | Notes |
|---|------|-------|--------|-------|
| **14** | **Windows support** | 7.1 | 2-3w | WebView2 + Win32 implementations. Requires LibAsyik MSVC verification. |
| **15** | **macOS support** | 7.2 | 2-3w | WKWebView + Cocoa implementations. |
| **16** | **Multi-platform CI** | 7.3 | 2-3d | GitHub Actions matrix for Linux/Win/macOS. |
| **17** | **Dynamic plugin system** | 8 | 2-3w | dlopen/LoadLibrary, tray, notifications, auto-updater. |

### Decision Point

The recommended next action is **Tier 1, Item 1: Fix the WebGL E2E teardown segfault**. This achieves a fully green CI pipeline and demonstrates framework stability. After Tier 1 is complete, LibAnyar on Linux is production-distributable.

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
