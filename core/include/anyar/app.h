#pragma once

// LibAnyar — Main Application Class
// Central entry point owning the service, HTTP server, IPC, and windows.

#include <anyar/app_config.h>
#include <anyar/command_registry.h>
#include <anyar/event_bus.h>
#include <anyar/ipc_router.h>
#include <anyar/plugin.h>
#include <anyar/shared_buffer.h>
#include <anyar/types.h>
#include <anyar/window.h>
#include <anyar/window_manager.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace anyar {

class App {
public:
    /// Create app with default config
    App();

    /// Create app with custom config
    explicit App(AppConfig config);

    ~App();

    // ── Command Registration ────────────────────────────────────────────────

    /// Register a synchronous command
    void command(const std::string& name, CommandHandler handler);

    /// Register an async command
    void command_async(const std::string& name, AsyncCommandHandler handler);

    // ── Event System ────────────────────────────────────────────────────────

    /// Emit an event to all frontends and C++ subscribers
    void emit(const std::string& event, const json& payload = json::object());

    /// Emit an event to a specific window only (+ C++ subscribers).
    /// Other windows will not receive it unless they called listenGlobal().
    void emit_to(const std::string& label, const std::string& event,
                 const json& payload = json::object());

    /// Subscribe to an event (C++ side)
    UnsubscribeFn on(const std::string& event, EventHandler handler);

    // ── Window Management ───────────────────────────────────────────────────

    /// Configure and display the main window (convenience for simple apps).
    /// Equivalent to calling create_window(opts) with label "main".
    void create_window(WindowConfig config = {});

    /// Create and display a window with full options.
    /// Can be called before run() for the main window, or from commands
    /// during run() for child/modal windows.
    void create_window(WindowCreateOptions opts);

    /// Get the WindowManager for direct window manipulation.
    WindowManager& window_manager() { return window_mgr_; }

    // ── Plugin System ───────────────────────────────────────────────────────

    /// Register a plugin
    void use(std::shared_ptr<IAnyarPlugin> plugin);

    // ── Custom HTTP Routes ──────────────────────────────────────────────────

    /// Route handler type: void(request, route_args)
    using RouteHandler = std::function<void(asyik::http_request_ptr,
                                            asyik::http_route_args)>;

    /// Register a GET route. Routes registered via http_get/http_post always
    /// take priority over serve_static (uses insert_front when server is live).
    void http_get(const std::string& path, RouteHandler handler);

    /// Register a POST route (same priority semantics as http_get).
    void http_post(const std::string& path, RouteHandler handler);

    // ── Local File Access ───────────────────────────────────────────────────

    /// Allow the webview to access files under the given directory via
    /// the `anyar-file://` custom URI scheme.  Multiple directories can
    /// be allowed.  Path traversal outside allowed roots is rejected.
    void allow_file_access(const std::string& directory);

    // ── Accessors ───────────────────────────────────────────────────────────

    /// Get underlying LibAsyik service
    asyik::service_ptr service() const { return service_; }

    /// Get the HTTP server (available after run() starts)
    anyar_http_server_ptr server() const { return server_; }

    /// Get the HTTP server port
    int port() const { return port_; }

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// Run the application (blocks until all windows are closed)
    int run();

    /// Set a resolver for embedded frontend resources.
    /// When set, the HTTP server serves frontend files from this resolver
    /// instead of the filesystem dist_path.
    /// @see anyar::make_embedded_resolver() in <anyar/embed.h>
    void set_frontend_resolver(FileResolver resolver);

    /// Register a callback that is invoked after the HTTP server is created
    /// and plugins are initialized, but before the window loads.
    /// Use this for async setup that needs the running service.
    using ReadyCallback = std::function<void()>;
    void on_ready(ReadyCallback cb);

    /// Register a callback that is invoked after the HTTP server is created
    /// but before serve_static is set up. Use this to add custom HTTP routes
    /// that should take priority over static file serving.
    /// @deprecated Use http_get()/http_post() or on_ready() instead.
    using ServerReadyCallback = std::function<void(anyar_http_server_ptr)>;
    void set_on_server_ready(ServerReadyCallback cb);

private:
    void start_server();
    void setup_native_ipc(Window* window);
    void register_window_commands();
    void register_buffer_commands();
    int find_available_port();

    AppConfig config_;
    int port_ = 0;

    asyik::service_ptr service_;
    anyar_http_server_ptr server_;
    std::thread service_thread_;

    CommandRegistry commands_;
    EventBus events_;
    std::unique_ptr<IpcRouter> ipc_router_;

    WindowManager window_mgr_;
    WindowCreateOptions main_window_opts_;
    bool has_window_ = false;

    /// Per-window native event sink IDs (label → sink id)
    std::map<std::string, uint64_t> native_event_sinks_;

    /// Active shared buffer pools (base_name → pool)
    std::map<std::string, std::unique_ptr<SharedBufferPool>> buffer_pools_;

    std::vector<std::shared_ptr<IAnyarPlugin>> plugins_;

    /// Optional resolver for embedded frontend resources
    FileResolver frontend_resolver_;

    /// Optional callback for registering HTTP routes before serve_static
    ServerReadyCallback on_server_ready_;

    /// Optional on_ready callback (fires after server + plugins init)
    ReadyCallback on_ready_;

    /// Deferred HTTP routes: {method, path, handler} registered before run()
    struct DeferredRoute {
        std::string method;
        std::string path;
        RouteHandler handler;
    };
    std::vector<DeferredRoute> deferred_routes_;

    /// Allowed directories for anyar-file:// URI scheme
    std::vector<std::string> allowed_file_roots_;
};

} // namespace anyar
