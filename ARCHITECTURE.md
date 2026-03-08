# LibAnyar — Architecture Document

> Last updated: 2026-03-07

## Table of Contents

1. [Overview](#overview)
2. [System Architecture](#system-architecture)
3. [Component Details](#component-details)
   - [Window Behavior — Native App Feel](#5a-window-behavior--native-app-feel)
4. [IPC Protocol](#ipc-protocol)
5. [Threading & Concurrency Model](#threading--concurrency-model)
6. [Platform Abstraction](#platform-abstraction)
7. [Dependency Map](#dependency-map)
8. [Directory Structure](#directory-structure)
9. [Security Model](#security-model)

---

## Overview

LibAnyar is a C++17 desktop application framework that combines:
- **OS-native webviews** for rendering web-based UI (React, Vue, Svelte, etc.)
- **Native IPC** via `webview_bind`/`webview_return` for Tauri-class command latency (~0.01ms)
- **LibAsyik** as the core runtime for HTTP serving, WebSocket fallback, SQL database access, and fiber-based concurrency
- **Thin native API wrappers** for platform features (dialogs, tray, clipboard, etc.)

### Design Principles

1. **Leverage, don't rewrite** — Use LibAsyik for everything it provides; only build what's missing
2. **OS webview, not bundled browser** — Keep binary size small (3-8MB)
3. **Native-first IPC** — In-process `webview_bind`/`webview_return` as primary channel; HTTP/WS as fallback for dev/browser mode
4. **Fiber-first concurrency** — Synchronous-looking async code via Boost.Fiber
5. **Convention over configuration** — Sensible defaults, minimal boilerplate
6. **Plugin-extensible** — Core stays lean; features added via plugins

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   Web Frontend (React/Vue/Svelte)           │
│                  Built with Vite/Webpack → dist/            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│    @libanyar/api  ←── NPM package (Hybrid IPC)             │
│    ┌──────────────────────────────────────────────────┐     │
│    │                                                  │     │
│    │  ★ Primary: Native IPC (in-process, ~0.01ms)     │     │
│    │    invoke(cmd, args)  → window.__anyar_ipc__()   │     │
│    │    listen(event, fn)  → __anyar_dispatch_event__  │     │
│    │    emit(event, data)  → invoke('anyar:emit_event')│    │
│    │                                                  │     │
│    │  ○ Fallback: HTTP/WebSocket (browser dev, ~1ms)  │     │
│    │    invoke(cmd, args)  → POST /__anyar__/invoke   │     │
│    │    listen(event, fn)  → WebSocket /__anyar_ws__   │     │
│    │    emit(event, data)  → WebSocket /__anyar_ws__   │     │
│    │                                                  │     │
│    │  fs.readFile(path)    → invoke('fs:read', ...)   │     │
│    │  dialog.open(opts)    → invoke('dialog:open', ...)│    │
│    │  db.query(sql, ...)   → invoke('db:query', ...)  │     │
│    └──────────────────────────────────────────────────┘     │
│                                                             │
├──────────────── OS WebView ─────────────────────────────────┤
│  Linux: WebKitGTK  │  Windows: WebView2  │  macOS: WKWebView│
│                                                             │
│  Navigates to: http://127.0.0.1:<port>/                     │
│  Wrapped by: webview/webview single-header library          │
│  Native IPC via: webview_bind / webview_return / webview_eval│
├─────────────────────────────────────────────────────────────┤
│                                                             │
│                   LibAnyar Core (C++17)                      │
│                                                             │
│  ┌──────────────┐ ┌──────────────┐ ┌─────────────────┐     │
│  │ anyar::App   │ │ IPC Router   │ │ CommandRegistry  │     │
│  │  - lifecycle │ │  - HTTP POST │ │  - dispatch map  │     │
│  │  - config    │ │  - WebSocket │ │  - middleware     │     │
│  │  - run()     │ │  - JSON-RPC  │ │  - async support │     │
│  └──────────────┘ └──────────────┘ └─────────────────┘     │
│                                                             │
│  ┌──────────────┐ ┌──────────────┐ ┌─────────────────┐     │
│  │ EventBus     │ │ WindowManager│ │ PluginSystem     │     │
│  │  - pub/sub   │ │  - webview   │ │  - IAnyarPlugin  │     │
│  │  - channels  │ │  - multi-win │ │  - dlopen/DLL    │     │
│  │  - WS fanout │ │  - lifecycle │ │  - static link   │     │
│  └──────────────┘ └──────────────┘ └─────────────────┘     │
│                                                             │
│  Native API Wrappers:                                       │
│    dialog.h │ tray.h │ clipboard.h │ shell.h │ fs.h        │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│                 LibAsyik  (Foundation Layer)                 │
│                                                             │
│  ┌───────────┐ ┌────────────┐ ┌────────┐ ┌──────────────┐  │
│  │ HTTP/S    │ │ WebSocket  │ │ SOCI   │ │ Fiber Engine │  │
│  │ Server    │ │ Server +   │ │ SQL    │ │ boost::fiber  │  │
│  │ + Static  │ │ Client     │ │ Pool   │ │ boost::asio   │  │
│  │ Serving   │ │            │ │        │ │ async()       │  │
│  └───────────┘ └────────────┘ └────────┘ └──────────────┘  │
│  ┌────────────┐ ┌────────────┐ ┌─────────────────────────┐ │
│  │ HTTP Client│ │ Rate Limit │ │ KV Cache │ Logging      │ │
│  │ + Digest   │ │ LeakyBucket│ │ Aixlog                  │ │
│  └────────────┘ └────────────┘ └─────────────────────────┘ │
│                                                             │
│  Underneath: Boost.Asio │ Boost.Beast │ Boost.Fiber │ SOCI │
└─────────────────────────────────────────────────────────────┘
```

---

## Component Details

### 1. `anyar::App` — Application Lifecycle

Central entry point. Owns the LibAsyik service, HTTP server, and window manager.

```cpp
class App {
public:
    App();
    App(AppConfig config);

    // Command registration
    void command(const std::string& name, CommandHandler handler);
    void command(const std::string& name, AsyncCommandHandler handler);

    // Event system
    void emit(const std::string& event, const json& payload = {});
    void on(const std::string& event, EventHandler handler);

    // Window management
    WindowHandle create_window(WindowConfig config);

    // Database (optional, via LibAsyik SOCI)
    std::shared_ptr<asyik::sql_pool> sql_pool(int backend, const std::string& conn, int pool_size);

    // Plugin system
    void use(std::shared_ptr<IAnyarPlugin> plugin);

    // Access to underlying LibAsyik service
    asyik::service_ptr service() const;

    // Run the application (blocks until all windows closed)
    int run();
};
```

### 2. IPC Router — Hybrid Architecture

LibAnyar uses a **hybrid IPC model** inspired by Tauri: in-process native
IPC as the primary channel (near-zero latency), with HTTP/WebSocket kept
as a fallback for browser-based development and content streaming.

#### 2a. Native IPC Channel (Primary — in webview)

**Commands** — `webview_bind` / `webview_return`:
- C++ calls `window_->bind("__anyar_ipc__", callback)` at startup
- JS calls `await window.__anyar_ipc__(json)` → returns a Promise
- Callback runs on the **GTK main thread**, dispatches work to a
  LibAsyik fiber via `service_->execute()`, then dispatches the result
  back to the GTK thread via `window_->dispatch()` + `window_->return_result()`
- Latency: ~0.01–0.1 ms (in-process, no TCP/HTTP overhead)

**Events** — `webview_eval` push:
- C++ registers a native event sink via `EventBus::add_ws_sink()`
- When an event fires, the sink calls `window_->eval("window.__anyar_dispatch_event__(msg)")` via `window_->dispatch()`
- JS sets up `window.__anyar_dispatch_event__` to fan out to registered listeners
- JS → C++ events use `invoke('anyar:emit_event', {event, payload})` which calls `EventBus::emit_local()` (no echo loop)

**Detection**: JS bridge checks `typeof window.__anyar_ipc__ === 'function'`
and the C++ side injects `window.__LIBANYAR_NATIVE__ = true` via `webview_init()`.

#### 2b. HTTP/WebSocket Channel (Fallback — browser dev mode)

**HTTP POST Channel** (`/__anyar__/invoke`):
- Request-response pattern
- Used by `invoke()` when native IPC is unavailable (e.g., `vite dev` in browser)
- Payload: `{"cmd": "name", "args": {...}, "id": "uuid"}`
- Response: `{"id": "uuid", "data": {...}, "error": null}`

**WebSocket Channel** (`/__anyar_ws__`):
- Bidirectional event streaming
- Used by `listen()` and `emit()` when not inside a webview
- Backend → Frontend: `{"type": "event", "event": "name", "payload": {...}}`
- Frontend → Backend: `{"type": "event", "event": "name", "payload": {...}}`
- One WebSocket per window, managed as a fiber

#### 2c. HTTP Server (Always Active)

The LibAsyik HTTP server remains active regardless of IPC mode:
- **Static file serving** (`serve_static("/")`) — serves the bundled frontend `dist/`
- **Content streaming** — video/audio with `Range` header support
- **Plugin HTTP routes** — any plugin that needs raw HTTP (e.g., thumbnail endpoints)

### 3. CommandRegistry

Dispatch table mapping command names to C++ handlers.

```cpp
using CommandHandler = std::function<json(const json& args)>;
using AsyncCommandHandler = std::function<void(const json& args, CommandReply reply)>;

class CommandRegistry {
    void add(const std::string& name, CommandHandler handler);
    void add_async(const std::string& name, AsyncCommandHandler handler);
    json dispatch(const std::string& name, const json& args);
};
```

### 4. EventBus

Fiber-channel-based pub/sub system that fans out events to:
- Native event sink (webview_eval push to frontend — primary)
- Connected WebSocket clients (browser fallback)
- Internal C++ subscribers (plugins, other fibers)

```cpp
class EventBus {
    void emit(const std::string& event, const json& payload);
    void emit_local(const std::string& event, const json& payload);
    SubscriptionHandle on(const std::string& event, EventHandler handler);
    void off(SubscriptionHandle handle);
};
```

`emit()` broadcasts to **all** sinks (native, WebSocket, C++ subscribers).
`emit_local()` dispatches to **C++ subscribers only** — used when JS emits
via native IPC to avoid an echo loop (JS → C++ → back to JS).
```

### 5. WindowManager

Manages webview instances. Each window:
- Gets its own webview (via `webview/webview`)
- Connects to the shared LibAsyik HTTP server
- Uses native IPC binding (`__anyar_ipc__`) for commands
- Receives events via native push (`webview_eval`)
- Falls back to WebSocket when opened in an external browser
- Runs on the main thread (OS requirement for GUI)
- Exposes `bind()`, `return_result()`, `init()`, `eval()`, `dispatch()` for native IPC plumbing

### 5a. Window Behavior — Native App Feel

When `debug = false`, LibAnyar applies several policies to make the webview
behave like a native desktop window rather than a browser:

#### Right-Click Context Menu

The browser's default right-click context menu (Inspect Element, etc.) is
**disabled** in production mode. A `contextmenu` event listener calls
`preventDefault()` on every right-click.

| `debug` | Behavior |
|---------|----------|
| `true`  | Browser context menu enabled (Inspect Element, etc.) |
| `false` | Context menu fully suppressed |

To re-enable the context menu for a specific element, add a local event
listener that stops propagation before the global blocker runs:

```js
myElement.addEventListener('contextmenu', (e) => {
  e.stopPropagation();      // Prevent the global blocker from firing
  // Show your custom context menu here...
}, { capture: true });
```

#### Page Zoom / Pinch-to-Zoom

In production mode, LibAnyar locks the page zoom to 1.0× using two layers:

1. **Native layer (WebKitGTK)** — Touchpad pinch gestures are blocked at the
   GDK level by removing `GDK_TOUCHPAD_GESTURE_MASK` from the widget's event
   mask. Any `GDK_TOUCHPAD_PINCH` events that slip through are consumed by an
   `"event"` signal handler. A `notify::zoom-level` handler snaps WebKit's
   zoom-level back to 1.0 if anything changes it.

2. **JavaScript layer** — Init scripts block `Ctrl+Wheel`, `Ctrl+Plus`,
   `Ctrl+Minus`, `Ctrl+0`, and multi-touch zoom at the document level.

| `debug` | Behavior |
|---------|----------|
| `true`  | Zoom freely with Ctrl+Scroll, Ctrl+/-, pinch gestures |
| `false` | Page zoom locked at 1.0×; all zoom inputs blocked |

##### Per-Component Zoom (data-pinch-zoom)

While global zoom is locked, individual components can **opt-in** to receive
zoom-related events by adding the `data-pinch-zoom` HTML attribute. The
JavaScript-level blockers check for this attribute in the DOM ancestry and
skip suppression for those elements.

**How it works:**

```html
<!-- This container will receive Ctrl+Wheel events for custom zoom -->
<div data-pinch-zoom class="image-viewer">
  <canvas id="viewer-canvas"></canvas>
</div>
```

```js
const viewer = document.querySelector('.image-viewer');
let scale = 1.0;

viewer.addEventListener('wheel', (e) => {
  if (e.ctrlKey) {
    e.preventDefault();
    const delta = e.deltaY > 0 ? 0.95 : 1.05;
    scale = Math.min(5, Math.max(0.1, scale * delta));
    viewer.querySelector('canvas').style.transform = `scale(${scale})`;
  }
}, { passive: false });
```

> **Note:** Native touchpad pinch events (`GDK_TOUCHPAD_PINCH`) are blocked
> at the GDK level for all components. The `data-pinch-zoom` mechanism works
> via `Ctrl+Wheel` events, which is how browsers translate trackpad pinch
> gestures into scroll events. This distinction means `Ctrl+Scroll`
> (mouse/trackpad two-finger) flows to JavaScript while raw pinch gestures
> are stopped before reaching WebKit.

#### Configuration

Both `AppConfig` and `WindowConfig` have a `debug` flag. They are merged
at startup: `window.debug = window.debug || app.debug`. Set either one
to `true` to enable developer features.

```cpp
anyar::AppConfig config;
config.debug = false;              // production: native-app behavior

anyar::WindowConfig win;
win.debug = false;                 // inherits from app.debug if not set
// OR explicitly: win.debug = true to override per-window
```

#### Frontend CSS (Recommended)

For completeness, add these CSS properties to prevent any residual browser
scrolling behavior:

```css
html {
  overflow: hidden;
  height: 100%;
  touch-action: none;           /* Disable touch gestures on the page */
}

body {
  overflow: hidden;
  touch-action: none;
  overscroll-behavior: none;    /* Prevent rubber-band scrolling */
}
```

And include a locked viewport meta tag:

```html
<meta name="viewport" content="width=device-width, initial-scale=1.0,
      maximum-scale=1.0, user-scalable=no" />
```

### 6. Plugin Interface

```cpp
class IAnyarPlugin {
public:
    virtual ~IAnyarPlugin() = default;
    virtual std::string name() const = 0;
    virtual void initialize(PluginContext& ctx) = 0;
    virtual void shutdown() {}
};

struct PluginContext {
    asyik::service_ptr service;
    CommandRegistry& commands;
    EventBus& events;
    AppConfig& config;
};
```

---

## IPC Protocol

### Native IPC — Command Invocation (Primary)

JS calls the bound function which returns a Promise:

```js
// JS side (inside webview)
const result = await window.__anyar_ipc__(JSON.stringify({
  cmd: "fs:readFile",
  args: { path: "/home/user/file.txt" }
}));
// result is a JSON string: {"data": {...}, "error": null}
```

C++ callback flow:
```
JS: __anyar_ipc__(json)  →  webview_bind callback (GTK thread)
                            │
                            ├→ service_->execute() (dispatch to fiber)
                            │   └→ commands_.dispatch(cmd, args)
                            │       └→ returns json result
                            │
                            └→ window_->dispatch() (back to GTK thread)
                                └→ window_->return_result(seq, 0, result)
                                    └→ JS Promise resolves
```

### Native IPC — Event Push (Primary)

```
C++ emit("fs:changed", payload)
  └→ native event sink
      └→ window_->dispatch()
          └→ window_->eval("window.__anyar_dispatch_event__({...})")
              └→ JS listener callbacks fire

JS emit("ui:ready", payload)
  └→ invoke('anyar:emit_event', {event, payload})  [native IPC]
      └→ C++ events_.emit_local()  [C++ subscribers only, no echo]
```

### HTTP POST — Command Invocation (Fallback)

```
POST /__anyar__/invoke HTTP/1.1
Content-Type: application/json
X-Anyar-Window: <window-id>

{
  "cmd": "fs:readFile",
  "args": { "path": "/home/user/file.txt" },
  "id": "550e8400-e29b-41d4-a716-446655440000"
}
```

```
HTTP/1.1 200 OK
Content-Type: application/json

{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "data": { "content": "file contents here..." },
  "error": null
}
```

### WebSocket — Event Stream (Fallback)

```
→ Backend to Frontend:
{"type":"event","event":"fs:changed","payload":{"path":"/home/user/file.txt"}}

→ Frontend to Backend:
{"type":"event","event":"ui:ready","payload":{}}
```

### Error Format

```json
{
  "id": "...",
  "data": null,
  "error": {
    "code": "NOT_FOUND",
    "message": "Command 'foo' not registered"
  }
}
```

---

## Threading & Concurrency Model

```
Main Thread (OS/GUI)                     LibAsyik Service Thread
┌───────────────────────────┐           ┌──────────────────────────────┐
│  Window event loop        │           │  asyik::service::run()       │
│  (webview.run())          │           │                              │
│                           │  native   │  ┌─ Fiber: HTTP server       │
│  - Webview rendering      │◄─ IPC ──►│  ├─ Fiber: WS connection #1 │
│  - OS events              │  bind/    │  ├─ Fiber: WS connection #2 │
│  - Native dialogs         │  return   │  ├─ Fiber: DB query          │
│  - __anyar_ipc__ binding  │           │  ├─ Fiber: File watcher      │
│  - webview_dispatch() ◄───┤  thread-  │  └─ Fiber: Plugin task       │
│  - webview_return()   ────┤  safe     │                              │
│  - webview_eval()     ────┤  calls    │  Worker Thread Pool:         │
│                           │           │  ┌─ async() task #1          │
│  Legacy (fallback):       │  HTTP/WS  │  ├─ async() task #2          │
│  - Browser HTTP requests  │◄────────►│  └─ async() task #3          │
└───────────────────────────┘           └──────────────────────────────┘
```

**Key rules:**
1. Webview must run on the **main thread** (OS requirement)
2. LibAsyik service runs on a **dedicated thread**, spawning fibers for all I/O
3. Blocking/CPU-intensive work offloaded via `as->async()` to **worker thread pool**
4. Fibers within one service share memory **without locks** (use `boost::fibers::mutex` when needed)
5. Cross-thread communication (main ↔ service) via `webview_dispatch()` and `service_->execute()`

### Native IPC Threading Flow

```
1. JS calls __anyar_ipc__(json)       [GTK main thread, webview_bind callback]
2.   → service_->execute(lambda)       [Dispatch to service thread as a fiber]
3.     → commands_.dispatch(cmd, args) [Runs inside fiber — can yield, do I/O]
4.     → result = json                 [Handler returns]
5.   → window_->dispatch(lambda)       [webview_dispatch: enqueue to GTK thread]
6.     → window_->return_result(seq)   [GTK main thread: resolves JS Promise]
```

This ensures:
- The **GTK main thread** is never blocked by command handlers
- Command handlers can call `run_on_gtk_main()` for native dialogs without deadlock
- `webview_return()` is thread-safe (per webview docs) but we dispatch to GTK thread for consistency
- Event push via `webview_eval()` is always dispatched through `webview_dispatch()`

---

## Platform Abstraction

### WebView Backend

| Platform | Engine | Library | Min Version |
|----------|--------|---------|-------------|
| Linux | WebKitGTK | webkit2gtk-4.1 | Ubuntu 22.04+ |
| Windows | WebView2 (Edge) | WebView2 SDK | Windows 10 1803+ |
| macOS | WKWebView | WebKit.framework | macOS 11+ |

Abstracted via `webview/webview` single-header C/C++ library.

### Native APIs

| API | Linux | Windows | macOS |
|-----|-------|---------|-------|
| File Dialog | GTK | COM/IFileDialog | NSOpenPanel |
| System Tray | libappindicator3 | Shell_NotifyIconW | NSStatusItem |
| Notifications | libnotify | WinToast | NSUserNotification |
| Clipboard | X11/Wayland | Win32 | NSPasteboard |
| Global Hotkeys | X11/XCB | RegisterHotKey | CGEvent |

Each wrapped behind a platform-agnostic C++ interface in `anyar::native::`.

---

## Dependency Map

### Core (Always Linked)

| Library | Version | Purpose | License |
|---------|---------|---------|---------|
| LibAsyik | >= 1.5.x | HTTP, WS, SQL, Fibers, Logging | MIT |
| Boost | >= 1.81 | Asio, Beast, Fiber, Context, URL | BSL-1.0 |
| OpenSSL | >= 1.1 | TLS for HTTPS/WSS | Apache-2.0 |
| webview/webview | latest | OS webview wrapper | MIT |
| nlohmann/json | >= 3.11 | JSON serialization | MIT |
| nativefiledialog-extended | >= 1.1 | File dialogs | zlib |

### Optional

| Library | Purpose | Enabled By |
|---------|---------|-----------|
| SOCI + SQLite3 | Local database | `ANYAR_ENABLE_SQLITE=ON` |
| SOCI + PostgreSQL | Remote database | `ANYAR_ENABLE_POSTGRESQL=ON` |
| cmrc | Embed assets in binary | `ANYAR_EMBED_FRONTEND=ON` |

---

## Directory Structure

```
libanyar/
├── CMakeLists.txt                  # Root build
├── README.md
├── PLAN.md                         # Implementation roadmap
├── ARCHITECTURE.md                 # This file
├── .ai/                            # AI assistant context
│   ├── context.md                  # Project context & conventions
│   ├── decisions.md                # Architecture decision log
│   └── progress.md                 # Current progress tracking
│
├── core/                           # LibAnyar framework library
│   ├── CMakeLists.txt
│   ├── include/anyar/
│   │   ├── app.h                   # anyar::App
│   │   ├── app_config.h            # Configuration structs
│   │   ├── window.h                # Window management
│   │   ├── ipc_router.h            # HTTP + WS IPC routing
│   │   ├── command_registry.h      # Command dispatch
│   │   ├── event_bus.h             # Pub/sub events
│   │   ├── plugin.h                # Plugin interface
│   │   ├── types.h                 # Common types & aliases
│   │   └── native/                 # Platform API wrappers
│   │       ├── dialog.h
│   │       ├── tray.h
│   │       ├── clipboard.h
│   │       ├── notification.h
│   │       └── shell.h
│   └── src/
│       ├── app.cpp
│       ├── ipc_router.cpp
│       ├── command_registry.cpp
│       ├── event_bus.cpp
│       ├── window.cpp
│       ├── asset_server.cpp        # serve_static configuration
│       └── platform/
│           ├── linux/
│           ├── windows/
│           └── macos/
│
├── plugins/                        # Built-in plugins
│   ├── fs/                         # Filesystem commands
│   ├── dialog/                     # Native dialog commands
│   ├── sqlite/                     # SQLite via SOCI
│   ├── shell/                      # Shell/subprocess
│   └── http_client/                # HTTP proxy for frontend
│
├── js-bridge/                      # NPM: @libanyar/api
│   ├── package.json
│   ├── tsconfig.json
│   └── src/
│       ├── index.ts                # invoke(), listen(), emit()
│       ├── fs.ts
│       ├── dialog.ts
│       ├── db.ts
│       ├── event.ts
│       └── http.ts
│
├── cli/                            # `anyar` CLI tool
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp
│       ├── cmd_init.cpp
│       ├── cmd_dev.cpp
│       └── cmd_build.cpp
│
├── templates/                      # Project templates
│   ├── react-ts/
│   ├── vue-ts/
│   └── vanilla/
│
├── examples/
│   ├── hello-world/
│   ├── todo-app/
│   └── file-explorer/
│
├── tests/
│   ├── unit/
│   └── integration/
│
└── third_party/                    # Git submodules or FetchContent
    ├── webview/
    ├── nlohmann_json/
    └── nfd/
```

---

## Security Model

### Origin Isolation
- LibAsyik HTTP server binds to `127.0.0.1` only (no network exposure)
- Random port assigned at startup, passed to webview via URL
- `X-Anyar-Window` header validates requests come from known windows

### Command Permissions (Future)
- Commands can be tagged with permission scopes (e.g., `fs:read`, `fs:write`, `shell:exec`)
- Frontend manifest declares required permissions
- User prompted on first use of sensitive commands

### Path Traversal Prevention
- LibAsyik's `serve_static()` already canonicalizes paths via `realpath()`
- File system plugin additionally restricts access to app-scoped directories
