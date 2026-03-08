#pragma once

// LibAnyar — Event Bus
// Pub/sub event system that bridges C++ backend ↔ WebSocket ↔ Frontend

#include <anyar/types.h>
#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/fiber/mutex.hpp>

namespace anyar {

class EventBus {
public:
    EventBus() = default;

    /// Subscribe to an event. Returns an unsubscribe function.
    UnsubscribeFn on(const std::string& event, EventHandler handler);

    /// Emit an event to all subscribers (C++ side)
    void emit(const std::string& event, const json& payload = json::object());

    /// Emit to C++ subscribers only — no push to frontend sinks.
    /// Used when the event originates from a frontend to avoid echo.
    void emit_local(const std::string& event, const json& payload = json::object());

    /// Register a WebSocket push function (called when events are emitted)
    /// Each connected window registers its WS push here.
    using WsPushFn = std::function<void(const std::string& message)>;
    uint64_t add_ws_sink(WsPushFn sink);
    void remove_ws_sink(uint64_t id);

    /// Called when a message arrives from a frontend WebSocket
    void on_ws_message(const std::string& message);

private:
    struct Subscription {
        uint64_t id;
        EventHandler handler;
    };

    std::unordered_map<std::string, std::vector<Subscription>> subscribers_;
    std::unordered_map<uint64_t, WsPushFn> ws_sinks_;
    std::atomic<uint64_t> next_id_{1};
    boost::fibers::mutex mutex_;
};

} // namespace anyar
