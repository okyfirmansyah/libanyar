#pragma once

// LibAnyar — IPC Router
// Routes HTTP POST commands and WebSocket events between frontend and backend

#include <anyar/command_registry.h>
#include <anyar/event_bus.h>
#include <anyar/types.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>

#include <memory>
#include <string>

namespace anyar {

/// Convenience type alias for the LibAsyik HTTP server pointer
using anyar_http_server_ptr = asyik::http_server_ptr<asyik::http_stream_type>;

class IpcRouter {
public:
    IpcRouter(CommandRegistry& commands, EventBus& events);

    /// Set up IPC endpoints on the given HTTP server
    void setup(anyar_http_server_ptr server);

private:
    /// Handle POST /__anyar__/invoke — command dispatch
    void handle_invoke(asyik::http_request_ptr req);

    /// Handle WebSocket /__anyar_ws__ — event streaming
    void handle_websocket(asyik::websocket_ptr ws, const asyik::http_route_args& args);

    CommandRegistry& commands_;
    EventBus& events_;
};

} // namespace anyar
