#pragma once

// LibAnyar — Event Bus
// Pub/sub event system that bridges C++ backend ↔ WebSocket ↔ Frontend

#include <anyar/types.h>
#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

    /// Register a labeled window sink (for per-window targeted events).
    /// @param label  Unique window label (e.g. "main", "settings")
    /// @param sink   Push function for this window
    /// @returns      Sink ID (also stored in label_to_sink_ map)
    uint64_t add_window_sink(const std::string& label, WsPushFn sink);

    /// Remove a labeled window sink by label.
    void remove_window_sink(const std::string& label);

    /// Emit an event to a specific window only (+ C++ subscribers).
    /// If the window is not found, only C++ subscribers are notified.
    void emit_to_window(const std::string& label, const std::string& event,
                        const json& payload = json::object());

    /// Mark a sink as a global listener. Global listeners receive
    /// targeted events even when they are not the target window.
    /// Used by JS listenGlobal().
    void set_global_listener(uint64_t sink_id, bool enabled);

    /// Called when a message arrives from a frontend WebSocket
    void on_ws_message(const std::string& message);

private:
    struct Subscription {
        uint64_t id;
        EventHandler handler;
    };

    std::unordered_map<std::string, std::vector<Subscription>> subscribers_;
    std::unordered_map<uint64_t, WsPushFn> ws_sinks_;
    std::unordered_map<std::string, uint64_t> label_to_sink_;  ///< window label → sink ID
    std::unordered_set<uint64_t> global_listener_sinks_;       ///< sinks receiving ALL events
    std::atomic<uint64_t> next_id_{1};
    boost::fibers::mutex mutex_;
};

} // namespace anyar
