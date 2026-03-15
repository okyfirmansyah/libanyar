// LibAnyar — EventBus Unit Tests

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>
#include <anyar/event_bus.h>

using namespace anyar;

TEST_CASE("EventBus: subscribe and emit delivers payload", "[event_bus]") {
    EventBus bus;
    json received;

    bus.on("test:event", [&](const json& payload) {
        received = payload;
    });

    bus.emit("test:event", {{"value", 42}});

    REQUIRE(received["value"] == 42);
}

TEST_CASE("EventBus: multiple subscribers all receive event", "[event_bus]") {
    EventBus bus;
    int call_count = 0;

    bus.on("multi", [&](const json&) { call_count++; });
    bus.on("multi", [&](const json&) { call_count++; });
    bus.on("multi", [&](const json&) { call_count++; });

    bus.emit("multi");

    REQUIRE(call_count == 3);
}

TEST_CASE("EventBus: unsubscribe stops delivery", "[event_bus]") {
    EventBus bus;
    int call_count = 0;

    auto unsub = bus.on("evt", [&](const json&) { call_count++; });

    bus.emit("evt");
    REQUIRE(call_count == 1);

    unsub();

    bus.emit("evt");
    REQUIRE(call_count == 1);  // unchanged
}

TEST_CASE("EventBus: emit with no subscribers does not crash", "[event_bus]") {
    EventBus bus;
    REQUIRE_NOTHROW(bus.emit("nobody_listening", {{"x", 1}}));
}

TEST_CASE("EventBus: WS sink receives serialized event", "[event_bus]") {
    EventBus bus;
    std::string received_msg;

    auto sink_id = bus.add_ws_sink([&](const std::string& msg) {
        received_msg = msg;
    });

    bus.emit("ws:test", {{"data", "hello"}});

    REQUIRE_FALSE(received_msg.empty());

    auto parsed = json::parse(received_msg);
    REQUIRE(parsed["type"] == "event");
    REQUIRE(parsed["event"] == "ws:test");
    REQUIRE(parsed["payload"]["data"] == "hello");

    bus.remove_ws_sink(sink_id);
}

TEST_CASE("EventBus: removed WS sink no longer receives events", "[event_bus]") {
    EventBus bus;
    int call_count = 0;

    auto sink_id = bus.add_ws_sink([&](const std::string&) {
        call_count++;
    });

    bus.emit("evt1");
    REQUIRE(call_count == 1);

    bus.remove_ws_sink(sink_id);

    bus.emit("evt2");
    REQUIRE(call_count == 1);  // unchanged
}

TEST_CASE("EventBus: on_ws_message dispatches to C++ subscribers", "[event_bus]") {
    EventBus bus;
    json received;

    bus.on("frontend:action", [&](const json& payload) {
        received = payload;
    });

    // Simulate a message from the frontend WebSocket
    json msg = {{"type", "event"}, {"event", "frontend:action"}, {"payload", {{"key", "val"}}}};
    bus.on_ws_message(msg.dump());

    REQUIRE(received["key"] == "val");
}

TEST_CASE("EventBus: on_ws_message with malformed JSON does not crash", "[event_bus]") {
    EventBus bus;
    REQUIRE_NOTHROW(bus.on_ws_message("not valid json"));
    REQUIRE_NOTHROW(bus.on_ws_message("{}"));  // missing "event" field
}

TEST_CASE("EventBus: subscriber exception does not affect others", "[event_bus]") {
    EventBus bus;
    int call_count = 0;

    bus.on("boom", [&](const json&) {
        throw std::runtime_error("oops");
    });

    bus.on("boom", [&](const json&) {
        call_count++;
    });

    bus.emit("boom");
    REQUIRE(call_count == 1);  // second handler still called
}

// ── Per-window sink tests ───────────────────────────────────────────────────

TEST_CASE("EventBus: add_window_sink receives broadcast events", "[event_bus]") {
    EventBus bus;
    std::string received;

    bus.add_window_sink("main", [&](const std::string& msg) {
        received = msg;
    });

    bus.emit("test:broadcast", {{"key", "value"}});

    REQUIRE_FALSE(received.empty());
    auto parsed = json::parse(received);
    REQUIRE(parsed["event"] == "test:broadcast");
    REQUIRE(parsed["payload"]["key"] == "value");
    REQUIRE_FALSE(parsed.contains("target"));
}

TEST_CASE("EventBus: emit_to_window delivers to target only", "[event_bus]") {
    EventBus bus;
    std::string received_a, received_b;

    bus.add_window_sink("win-a", [&](const std::string& msg) {
        received_a = msg;
    });
    bus.add_window_sink("win-b", [&](const std::string& msg) {
        received_b = msg;
    });

    bus.emit_to_window("win-b", "targeted:event", {{"data", 42}});

    REQUIRE(received_a.empty());
    REQUIRE_FALSE(received_b.empty());

    auto parsed = json::parse(received_b);
    REQUIRE(parsed["event"] == "targeted:event");
    REQUIRE(parsed["payload"]["data"] == 42);
    REQUIRE(parsed["target"] == "win-b");
}

TEST_CASE("EventBus: emit_to_window notifies C++ subscribers", "[event_bus]") {
    EventBus bus;
    json received;

    bus.on("targeted:cpp", [&](const json& payload) {
        received = payload;
    });

    bus.emit_to_window("any-window", "targeted:cpp", {{"from", "cpp"}});

    REQUIRE(received["from"] == "cpp");
}

TEST_CASE("EventBus: emit_to_window to non-existent label only fires C++ subs", "[event_bus]") {
    EventBus bus;
    int cpp_count = 0;

    bus.on("ghost", [&](const json&) { cpp_count++; });

    // Target a label that doesn't exist — should not crash
    REQUIRE_NOTHROW(bus.emit_to_window("no-such-window", "ghost"));
    REQUIRE(cpp_count == 1);
}

TEST_CASE("EventBus: remove_window_sink cleans up by label", "[event_bus]") {
    EventBus bus;
    int call_count = 0;

    bus.add_window_sink("temp", [&](const std::string&) {
        call_count++;
    });

    bus.emit("test");
    REQUIRE(call_count == 1);

    bus.remove_window_sink("temp");

    bus.emit("test");
    REQUIRE(call_count == 1);  // no longer receives

    // emit_to_window to removed label — should not crash
    REQUIRE_NOTHROW(bus.emit_to_window("temp", "test"));
}

TEST_CASE("EventBus: global listener receives targeted events for other windows", "[event_bus]") {
    EventBus bus;
    std::string received_a, received_b, received_monitor;

    auto id_a = bus.add_window_sink("win-a", [&](const std::string& msg) {
        received_a = msg;
    });
    bus.add_window_sink("win-b", [&](const std::string& msg) {
        received_b = msg;
    });
    auto id_monitor = bus.add_window_sink("monitor", [&](const std::string& msg) {
        received_monitor = msg;
    });

    // Mark monitor as global listener
    bus.set_global_listener(id_monitor, true);

    // Emit targeted to win-a
    bus.emit_to_window("win-a", "secret", {{"info", "classified"}});

    REQUIRE_FALSE(received_a.empty());      // target receives
    REQUIRE(received_b.empty());            // non-target doesn't
    REQUIRE_FALSE(received_monitor.empty()); // global listener receives

    auto parsed = json::parse(received_monitor);
    REQUIRE(parsed["target"] == "win-a");
    REQUIRE(parsed["event"] == "secret");
}

TEST_CASE("EventBus: global listener does not double-receive when it is the target", "[event_bus]") {
    EventBus bus;
    int call_count = 0;

    auto id = bus.add_window_sink("win-a", [&](const std::string&) {
        call_count++;
    });

    bus.set_global_listener(id, true);

    // Target win-a directly — should receive once, not twice
    bus.emit_to_window("win-a", "test");
    REQUIRE(call_count == 1);
}

TEST_CASE("EventBus: set_global_listener false disables global delivery", "[event_bus]") {
    EventBus bus;
    std::string received_monitor;

    bus.add_window_sink("target", [&](const std::string&) {});
    auto id_monitor = bus.add_window_sink("monitor", [&](const std::string& msg) {
        received_monitor = msg;
    });

    bus.set_global_listener(id_monitor, true);
    bus.emit_to_window("target", "evt1");
    REQUIRE_FALSE(received_monitor.empty());

    // Disable global listener
    received_monitor.clear();
    bus.set_global_listener(id_monitor, false);
    bus.emit_to_window("target", "evt2");
    REQUIRE(received_monitor.empty());
}

TEST_CASE("EventBus: broadcast still reaches all sinks including global listeners", "[event_bus]") {
    EventBus bus;
    int count_a = 0, count_b = 0;

    auto id_a = bus.add_window_sink("a", [&](const std::string&) { count_a++; });
    bus.add_window_sink("b", [&](const std::string&) { count_b++; });

    bus.set_global_listener(id_a, true);

    bus.emit("broadcast");
    REQUIRE(count_a == 1);
    REQUIRE(count_b == 1);
}
