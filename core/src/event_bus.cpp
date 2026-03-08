#include <anyar/event_bus.h>

namespace anyar {

UnsubscribeFn EventBus::on(const std::string& event, EventHandler handler) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    uint64_t id = next_id_++;
    subscribers_[event].push_back({id, std::move(handler)});

    // Return unsubscribe function
    return [this, event, id]() {
        std::lock_guard<boost::fibers::mutex> lock(mutex_);
        auto it = subscribers_.find(event);
        if (it != subscribers_.end()) {
            auto& subs = it->second;
            subs.erase(
                std::remove_if(subs.begin(), subs.end(),
                    [id](const Subscription& s) { return s.id == id; }),
                subs.end()
            );
        }
    };
}

void EventBus::emit(const std::string& event, const json& payload) {
    // Build the event message JSON
    EventMessage msg;
    msg.event = event;
    msg.payload = payload;
    std::string serialized = msg.to_json().dump();

    std::lock_guard<boost::fibers::mutex> lock(mutex_);

    // Notify C++ subscribers
    auto it = subscribers_.find(event);
    if (it != subscribers_.end()) {
        for (auto& sub : it->second) {
            try {
                sub.handler(payload);
            } catch (...) {
                // Don't let one subscriber crash others
            }
        }
    }

    // Push to all connected WebSocket sinks (frontend windows)
    for (auto& [id, sink] : ws_sinks_) {
        try {
            sink(serialized);
        } catch (...) {
            // WebSocket may have disconnected
        }
    }
}

void EventBus::emit_local(const std::string& event, const json& payload) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    auto it = subscribers_.find(event);
    if (it != subscribers_.end()) {
        for (auto& sub : it->second) {
            try {
                sub.handler(payload);
            } catch (...) {}
        }
    }
}

uint64_t EventBus::add_ws_sink(WsPushFn sink) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    uint64_t id = next_id_++;
    ws_sinks_[id] = std::move(sink);
    return id;
}

void EventBus::remove_ws_sink(uint64_t id) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    ws_sinks_.erase(id);
}

void EventBus::on_ws_message(const std::string& message) {
    try {
        auto j = json::parse(message);
        auto msg = EventMessage::from_json(j);

        // Dispatch to C++ subscribers
        std::lock_guard<boost::fibers::mutex> lock(mutex_);
        auto it = subscribers_.find(msg.event);
        if (it != subscribers_.end()) {
            for (auto& sub : it->second) {
                try {
                    sub.handler(msg.payload);
                } catch (...) {}
            }
        }
    } catch (...) {
        // Malformed message — ignore
    }
}

} // namespace anyar
