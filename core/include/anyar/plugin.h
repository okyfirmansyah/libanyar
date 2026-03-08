#pragma once

// LibAnyar — Plugin Interface

#include <anyar/command_registry.h>
#include <anyar/event_bus.h>
#include <anyar/ipc_router.h>
#include <anyar/app_config.h>
#include <anyar/types.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>

#include <memory>
#include <string>

namespace anyar {

/// Context passed to plugins during initialization
struct PluginContext {
    asyik::service_ptr service;
    anyar_http_server_ptr server;   ///< HTTP server (for registering custom routes)
    CommandRegistry& commands;
    EventBus& events;
    AppConfig& config;
};

/// Interface for LibAnyar plugins
class IAnyarPlugin {
public:
    virtual ~IAnyarPlugin() = default;

    /// Plugin name (used for logging and identification)
    virtual std::string name() const = 0;

    /// Called once during app initialization
    virtual void initialize(PluginContext& ctx) = 0;

    /// Called during app shutdown (optional)
    virtual void shutdown() {}
};

} // namespace anyar
