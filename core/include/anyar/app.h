#pragma once

// LibAnyar — Main Application Class
// Central entry point owning the service, HTTP server, IPC, and windows.

#include <anyar/app_config.h>
#include <anyar/command_registry.h>
#include <anyar/event_bus.h>
#include <anyar/ipc_router.h>
#include <anyar/plugin.h>
#include <anyar/types.h>
#include <anyar/window.h>
#include <anyar/window_manager.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>

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

    // ── Accessors ───────────────────────────────────────────────────────────

    /// Get underlying LibAsyik service
    asyik::service_ptr service() const { return service_; }

    /// Get the HTTP server port
    int port() const { return port_; }

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// Run the application (blocks until all windows are closed)
    int run();

private:
    void start_server();
    void setup_native_ipc(Window* window);
    void register_window_commands();
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

    std::vector<std::shared_ptr<IAnyarPlugin>> plugins_;
};

} // namespace anyar
