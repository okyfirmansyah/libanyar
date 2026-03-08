// LibAnyar — IPC Types Unit Tests

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>
#include <anyar/types.h>

using namespace anyar;

TEST_CASE("IpcResponse: to_json with data (no error)", "[types]") {
    IpcResponse resp;
    resp.id = "req-1";
    resp.data = {{"result", 42}};
    resp.error = "";

    auto j = resp.to_json();

    REQUIRE(j["id"] == "req-1");
    REQUIRE(j["data"]["result"] == 42);
    REQUIRE(j["error"].is_null());
}

TEST_CASE("IpcResponse: to_json with error (no data)", "[types]") {
    IpcResponse resp;
    resp.id = "req-2";
    resp.error = "Something failed";

    auto j = resp.to_json();

    REQUIRE(j["id"] == "req-2");
    REQUIRE(j["data"].is_null());
    REQUIRE(j["error"]["code"] == "ERROR");
    REQUIRE(j["error"]["message"] == "Something failed");
}

TEST_CASE("IpcResponse: to_json with null data and empty error", "[types]") {
    IpcResponse resp;
    resp.id = "req-3";
    resp.data = nullptr;
    resp.error = "";

    auto j = resp.to_json();

    REQUIRE(j["id"] == "req-3");
    REQUIRE(j["data"].is_null());
    REQUIRE(j["error"].is_null());
}

TEST_CASE("EventMessage: to_json serialization", "[types]") {
    EventMessage msg;
    msg.event = "counter:updated";
    msg.payload = {{"count", 5}};

    auto j = msg.to_json();

    REQUIRE(j["type"] == "event");
    REQUIRE(j["event"] == "counter:updated");
    REQUIRE(j["payload"]["count"] == 5);
}

TEST_CASE("EventMessage: from_json roundtrip", "[types]") {
    json j = {
        {"type", "event"},
        {"event", "test:data"},
        {"payload", {{"x", 1}, {"y", "hello"}}}
    };

    auto msg = EventMessage::from_json(j);

    REQUIRE(msg.type == "event");
    REQUIRE(msg.event == "test:data");
    REQUIRE(msg.payload["x"] == 1);
    REQUIRE(msg.payload["y"] == "hello");

    // Re-serialize and compare
    auto j2 = msg.to_json();
    REQUIRE(j2 == j);
}

TEST_CASE("EventMessage: from_json with missing payload defaults to empty object", "[types]") {
    json j = {{"type", "event"}, {"event", "no_payload"}};

    auto msg = EventMessage::from_json(j);

    REQUIRE(msg.event == "no_payload");
    REQUIRE(msg.payload.is_object());
    REQUIRE(msg.payload.empty());
}

TEST_CASE("EventMessage: from_json with missing type defaults to 'event'", "[types]") {
    json j = {{"event", "test"}, {"payload", {}}};

    auto msg = EventMessage::from_json(j);

    REQUIRE(msg.type == "event");
}

TEST_CASE("EventMessage: from_json without event field throws", "[types]") {
    json j = {{"type", "event"}, {"payload", {}}};

    REQUIRE_THROWS_AS(EventMessage::from_json(j), json::out_of_range);
}
