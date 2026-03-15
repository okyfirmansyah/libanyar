#pragma once

// LibAnyar — Core types and aliases
// Used across all components

#include <functional>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace anyar {

using json = nlohmann::json;

// ── Forward Declarations ────────────────────────────────────────────────────
class App;
class Window;
class WindowManager;
class CommandRegistry;
class EventBus;
class IpcRouter;

// ── Type Aliases ────────────────────────────────────────────────────────────

/// Handler for synchronous commands: receives args, returns result JSON
using CommandHandler = std::function<json(const json& args)>;

/// Reply callback for async commands
using CommandReply = std::function<void(const json& data, const std::string& error)>;

/// Handler for async commands: receives args + reply callback
using AsyncCommandHandler = std::function<void(const json& args, CommandReply reply)>;

/// Handler for events
using EventHandler = std::function<void(const json& payload)>;

/// Subscription handle — call to unsubscribe
using UnsubscribeFn = std::function<void()>;

// ── IPC Protocol Structures ─────────────────────────────────────────────────

struct IpcRequest {
    std::string id;
    std::string cmd;
    json args;
};

struct IpcResponse {
    std::string id;
    json data;
    std::string error;  // empty = no error

    json to_json() const {
        json j;
        j["id"] = id;
        if (error.empty()) {
            j["data"] = data;
            j["error"] = nullptr;
        } else {
            j["data"] = nullptr;
            j["error"] = {{"code", "ERROR"}, {"message", error}};
        }
        return j;
    }
};

struct EventMessage {
    std::string type = "event";
    std::string event;
    json payload;
    std::string target;  ///< Empty = broadcast; non-empty = targeted to window label

    json to_json() const {
        json j = {{"type", type}, {"event", event}, {"payload", payload}};
        if (!target.empty()) {
            j["target"] = target;
        }
        return j;
    }

    static EventMessage from_json(const json& j) {
        EventMessage msg;
        msg.type = j.value("type", "event");
        msg.event = j.at("event").get<std::string>();
        msg.payload = j.value("payload", json::object());
        msg.target = j.value("target", std::string());
        return msg;
    }
};

} // namespace anyar
