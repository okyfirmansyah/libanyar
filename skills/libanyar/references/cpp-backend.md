# C++ backend reference

Headers live in `core/include/anyar/`. `using json = nlohmann::json;` is available as `anyar::json`.

## Contents
- App setup and lifecycle
- Commands (sync and async)
- Events
- Plugins
- Background work
- HTTP routes and files
- Windows

## App setup and lifecycle

```cpp
anyar::AppConfig config;
config.host = "127.0.0.1";
config.port = 0;                    // 0 = free port; fixed port lets you open the UI in a browser
config.debug = false;               // true = webview DevTools, context menu, zoom
config.dist_path = "./dist";        // relative: cwd first, then next to the executable
// config.debug_tools.console = true;   // see debugging-and-testing.md
anyar::App app(config);
```

Order inside `app.run()`:
1. Server starts; every plugin's `initialize(ctx)` runs; `on_ready` callbacks fire (fiber context, server up).
2. Main window is created; `on_window_ready(Window&)` fires on the UI thread before the loop (the place for `create_pinhole()`).
3. The UI loop runs until the main window closes (or `window:close-all` / `debug:exit`).
4. Shutdown: bounded UI drain → every plugin's `shutdown()` (main thread, service still alive) → server closed → service stopped → windows destroyed.
5. `run()` returns the exit code (0, or the code from `debug:exit` / a test timeout).

Useful members: `app.service()` (LibAsyik service for `run_blocking`), `app.server()`, `app.port()`, `app.emit()`, `app.emit_to(label, …)`, `app.window_manager()`, `app.allow_file_access(dir)`, `app.http_get/http_post(path, handler)`, `app.on_ready(fn)`, `app.on_window_ready(fn)`.

## Commands

```cpp
// Sync: runs in a fiber on the service thread. Return JSON or throw.
app.command("notes:get", [&](const anyar::json& args) -> anyar::json {
    const auto id = args.at("id").get<int>();          // .at() throws if missing → JS rejection
    if (id < 0) throw std::invalid_argument("id must be >= 0");
    return {{"id", id}, {"text", store.get(id)}};
});

// Async: reply later from any thread/fiber (e.g. when a callback fires).
app.command_async("net:ping", [](const anyar::json& args, anyar::CommandReply reply) {
    start_ping(args.at("host"), [reply](bool ok, int ms) {
        if (ok) reply({{"ms", ms}}, "");
        else    reply(nullptr, "unreachable");
    });
});
```

- Name commands `namespace:verb`. Built-in namespaces are taken: `anyar:`, `window:`, `buffer:`, `pinhole:`, `debug:`, `fs:`, `dialog:`, `shell:`, `clipboard:`, `db:`.
- The calling window's label arrives as `args["_caller_label"]`.
- A sync handler may call `anyar::run_blocking(...)` and `anyar::run_on_main_thread(...)`: both suspend only that fiber.
- Async: if every copy of `reply` is dropped without being called, the caller gets a "did not complete" error; only the first call counts.

## Events

```cpp
app.emit("job:progress", {{"pct", 42}});               // every window
app.emit_to("settings", "prefs:changed", prefs);       // one window by label
```
JS side: `listen('job:progress', fn)` (this window) / `listenGlobal` (all). Events are fire-and-forget; use them for progress and pushes, commands for request/response.

## Plugins

```cpp
#include <anyar/plugin.h>
#include <anyar/task.h>

class SensorPlugin : public anyar::IAnyarPlugin {
public:
    std::string name() const override { return "sensor"; }

    void initialize(anyar::PluginContext& ctx) override {
        service_ = ctx.service;
        events_ = &ctx.events;
        ctx.commands.add("sensor:start", [this](const anyar::json& a) -> anyar::json {
            std::lock_guard<std::mutex> l(mu_);           // state shared with shutdown()
            if (stopping_) throw std::runtime_error("sensor: shutting down");
            if (loop_.running()) return {{"ok", true}};
            loop_.start(service_, [this](anyar::StopToken st) {
                while (!st.stop_requested()) {
                    auto sample = anyar::run_blocking(service_, [] { return read_device(); });
                    events_->emit("sensor:sample", sample);   // emit from the fiber, not the worker
                }
            });
            return {{"ok", true}};
        });
    }

    void shutdown() override {        // main thread; service still dispatching commands
        { std::lock_guard<std::mutex> l(mu_); stopping_ = true; }   // refuse new starts
        loop_.stop();                  // request + join — OUTSIDE the lock (see below)
    }

private:
    asyik::service_ptr service_;
    anyar::EventBus* events_ = nullptr;
    anyar::BackgroundTask loop_;
    std::mutex mu_;
    bool stopping_ = false;
};
// main: app.use(std::make_shared<SensorPlugin>());
```

`PluginContext` gives you `service`, `server` (for custom routes), `commands`, `events` and `config`.

Teardown checklist for `shutdown()`:
- Stop every BackgroundTask (`stop()` joins).
- Close pools/queues that something may be blocked on.
- Lock any map/vector that command handlers also touch — but **never hold a `std::mutex` while joining** (`loop_.stop()`, waiting for in-flight jobs). A command fiber that blocks on that mutex freezes the whole service thread, so the loop you are joining can never finish. Set a `stopping_` flag under the lock, release it, then join; or use a `boost::fibers::mutex`, which suspends the waiting fiber instead of the thread.
- Drain or refuse commands still waiting on `run_blocking` (count them in flight).
- Free SharedBuffers you registered (`SharedBufferRegistry::instance().remove(name)`).

## Background work (`<anyar/task.h>`)

| Need | Use |
|---|---|
| One blocking call inside a command | `anyar::run_blocking(service, fn)` |
| Long-lived loop | `anyar::BackgroundTask` + `StopToken`; `stop()` in `shutdown()` and before restarting |
| Many independent jobs | several `run_blocking` calls (the worker pool runs them in parallel) |

Never hold a fiber mutex across `run_blocking`. Emit events and touch EventBus from the fiber after the job returns, not from inside the worker lambda.

## HTTP routes and files

```cpp
app.http_get("/api/health", [](auto req, auto args) {
    req->response.headers.set("Content-Type", "application/json");
    req->response.body = R"({"ok":true})";
});
// Large/media files: streaming, Range-aware (required for <video>/<audio>).
// In a plugin's initialize(): capture the server weakly — the route is owned by it.
std::weak_ptr<asyik::http_server<asyik::http_stream_type>> weak = ctx.server;
ctx.server->on_http_request("/media/stream", "GET",
    [this, weak](asyik::http_request_ptr req, asyik::http_route_args) {
        anyar::serve_file(weak.lock(), req, current_path());   // <anyar/http_file.h>
    });
```
- Route args are not URL-decoded: run `anyar::percent_decode()`, then reject `..`.
- `app.allow_file_access(dir)` exposes a directory to the page as `anyar-file://` (Linux) and `/__anyar__/file/` (all platforms), with root containment checked.

## Windows (C++ side)

- `app.create_window(WindowConfig)` creates the main window: `title, width, height, resizable, decorations, debug, min_width/min_height`.
- Extra windows are usually created from JS (`createWindow({label, url, parent, modal})`).
- `Window` methods run on the UI thread: `eval(js)`, `init(js)` (runs before page scripts, every navigation), `set_title`, `set_size`, `focus`, `set_always_on_top`, `set_close_confirmation`, `create_pinhole`, `capture` (debug), `inject_input` (debug).
- From a fiber, get onto the UI thread with `run_on_main_thread`.
