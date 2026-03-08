#pragma once

// LibAnyar — Main Application Class
// Central entry point owning the service, HTTP server, IPC, and window.

#include <anyar/app_config.h>
#include <anyar/command_registry.h>
#include <anyar/event_bus.h>
#include <anyar/ipc_router.h>
#include <anyar/plugin.h>
#include <anyar/types.h>
#include <anyar/window.h>

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

    /// Create and display a window (only one window in Phase 1)
    void create_window(WindowConfig config = {});

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
    void setup_native_ipc();
    int find_available_port();

    AppConfig config_;
    int port_ = 0;

    asyik::service_ptr service_;
    anyar_http_server_ptr server_;
    std::thread service_thread_;

    CommandRegistry commands_;
    EventBus events_;
    std::unique_ptr<IpcRouter> ipc_router_;

    std::unique_ptr<Window> window_;
    WindowConfig window_config_;
    bool has_window_ = false;
    uint64_t native_event_sink_id_ = 0;

    std::vector<std::shared_ptr<IAnyarPlugin>> plugins_;
};

} // namespace anyar
