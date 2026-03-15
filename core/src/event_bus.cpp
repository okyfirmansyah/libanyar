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
    global_listener_sinks_.erase(id);
}

uint64_t EventBus::add_window_sink(const std::string& label, WsPushFn sink) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    uint64_t id = next_id_++;
    ws_sinks_[id] = std::move(sink);
    label_to_sink_[label] = id;
    return id;
}

void EventBus::remove_window_sink(const std::string& label) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    auto it = label_to_sink_.find(label);
    if (it != label_to_sink_.end()) {
        uint64_t id = it->second;
        ws_sinks_.erase(id);
        global_listener_sinks_.erase(id);
        label_to_sink_.erase(it);
    }
}

void EventBus::emit_to_window(const std::string& label,
                              const std::string& event,
                              const json& payload) {
    EventMessage msg;
    msg.event = event;
    msg.payload = payload;
    msg.target = label;
    std::string serialized = msg.to_json().dump();

    std::lock_guard<boost::fibers::mutex> lock(mutex_);

    // Notify C++ subscribers
    auto it = subscribers_.find(event);
    if (it != subscribers_.end()) {
        for (auto& sub : it->second) {
            try {
                sub.handler(payload);
            } catch (...) {}
        }
    }

    // Push to target window only
    auto sit = label_to_sink_.find(label);
    uint64_t target_id = 0;
    if (sit != label_to_sink_.end()) {
        target_id = sit->second;
        auto writ = ws_sinks_.find(target_id);
        if (writ != ws_sinks_.end()) {
            try {
                writ->second(serialized);
            } catch (...) {}
        }
    }

    // Also push to global listeners (excluding the target itself)
    for (uint64_t gid : global_listener_sinks_) {
        if (gid != target_id) {
            auto git = ws_sinks_.find(gid);
            if (git != ws_sinks_.end()) {
                try {
                    git->second(serialized);
                } catch (...) {}
            }
        }
    }
}

void EventBus::set_global_listener(uint64_t sink_id, bool enabled) {
    std::lock_guard<boost::fibers::mutex> lock(mutex_);
    if (enabled) {
        global_listener_sinks_.insert(sink_id);
    } else {
        global_listener_sinks_.erase(sink_id);
    }
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
