#include <anyar/ipc_router.h>
#include <nlohmann/json.hpp>

namespace anyar {

IpcRouter::IpcRouter(CommandRegistry& commands, EventBus& events)
    : commands_(commands), events_(events) {}

void IpcRouter::setup(anyar_http_server_ptr server) {
    // ── Command endpoint: POST /__anyar__/invoke ────────────────────────────
    server->on_http_request(
        "/__anyar__/invoke", "POST",
        [this](auto req, auto args) {
            handle_invoke(req);
        }
    );

    // ── Event endpoint: WebSocket /__anyar_ws__ ─────────────────────────────
    server->on_websocket(
        "/__anyar_ws__",
        [this](auto ws, auto args) {
            handle_websocket(ws, args);
        }
    );

    // ── CORS preflight for IPC endpoint ─────────────────────────────────────
    server->on_http_request(
        "/__anyar__/invoke", "OPTIONS",
        [](auto req, auto args) {
            req->response.headers.set("Access-Control-Allow-Origin", "*");
            req->response.headers.set("Access-Control-Allow-Methods", "POST, OPTIONS");
            req->response.headers.set("Access-Control-Allow-Headers", "Content-Type, X-Anyar-Window");
            req->response.result(204);
        }
    );
}

void IpcRouter::handle_invoke(asyik::http_request_ptr req) {
    // Set CORS headers
    req->response.headers.set("Access-Control-Allow-Origin", "*");
    req->response.headers.set("Content-Type", "application/json");

    try {
        auto body = json::parse(req->body);

        IpcRequest ipc_req;
        ipc_req.id = body.value("id", "");
        ipc_req.cmd = body.at("cmd").get<std::string>();
        ipc_req.args = body.value("args", json::object());

        auto response = commands_.dispatch(ipc_req);
        req->response.body = response.to_json().dump();
        req->response.result(200);

    } catch (const json::exception& e) {
        IpcResponse error_resp;
        error_resp.error = std::string("Invalid IPC request: ") + e.what();
        req->response.body = error_resp.to_json().dump();
        req->response.result(400);

    } catch (const std::exception& e) {
        IpcResponse error_resp;
        error_resp.error = std::string("Internal error: ") + e.what();
        req->response.body = error_resp.to_json().dump();
        req->response.result(500);
    }
}

void IpcRouter::handle_websocket(asyik::websocket_ptr ws, const asyik::http_route_args& args) {
    // Register this WebSocket as an event sink so backend events get pushed
    auto ws_weak = std::weak_ptr<asyik::websocket>(ws);
    uint64_t sink_id = events_.add_ws_sink([ws_weak](const std::string& message) {
        auto ws_shared = ws_weak.lock();
        if (ws_shared) {
            try {
                ws_shared->send_string(message);
            } catch (...) {}
        }
    });

    try {
        // Read loop — receives events from frontend
        while (true) {
            auto message = ws->get_string();
            events_.on_ws_message(message);
        }
    } catch (...) {
        // Connection closed or error
    }

    // Cleanup
    events_.remove_ws_sink(sink_id);
}

} // namespace anyar
