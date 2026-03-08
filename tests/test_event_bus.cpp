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
