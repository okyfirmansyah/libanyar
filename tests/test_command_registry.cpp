// LibAnyar — CommandRegistry Unit Tests

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>
#include <anyar/command_registry.h>

using namespace anyar;

TEST_CASE("CommandRegistry: register and dispatch sync handler", "[command_registry]") {
    CommandRegistry reg;

    reg.add("greet", [](const json& args) -> json {
        return {{"msg", "Hello " + args.value("name", "World")}};
    });

    IpcRequest req{"1", "greet", {{"name", "Alice"}}};
    auto resp = reg.dispatch(req);

    REQUIRE(resp.id == "1");
    REQUIRE(resp.error.empty());
    REQUIRE(resp.data["msg"] == "Hello Alice");
}

TEST_CASE("CommandRegistry: register and dispatch async handler", "[command_registry]") {
    CommandRegistry reg;

    reg.add_async("compute", [](const json& args, CommandReply reply) {
        int a = args.value("a", 0);
        int b = args.value("b", 0);
        reply({{"sum", a + b}}, "");
    });

    IpcRequest req{"2", "compute", {{"a", 10}, {"b", 32}}};
    auto resp = reg.dispatch(req);

    REQUIRE(resp.id == "2");
    REQUIRE(resp.error.empty());
    REQUIRE(resp.data["sum"] == 42);
}

TEST_CASE("CommandRegistry: dispatch unknown command returns error", "[command_registry]") {
    CommandRegistry reg;

    IpcRequest req{"3", "nonexistent", {}};
    auto resp = reg.dispatch(req);

    REQUIRE(resp.id == "3");
    REQUIRE_FALSE(resp.error.empty());
    REQUIRE(resp.error.find("not registered") != std::string::npos);
}

TEST_CASE("CommandRegistry: has() returns correct value", "[command_registry]") {
    CommandRegistry reg;

    REQUIRE_FALSE(reg.has("greet"));

    reg.add("greet", [](const json&) -> json { return nullptr; });

    REQUIRE(reg.has("greet"));
    REQUIRE_FALSE(reg.has("missing"));
}

TEST_CASE("CommandRegistry: handler that throws returns error response", "[command_registry]") {
    CommandRegistry reg;

    reg.add("fail", [](const json&) -> json {
        throw std::runtime_error("something broke");
    });

    IpcRequest req{"4", "fail", {}};
    auto resp = reg.dispatch(req);

    REQUIRE(resp.id == "4");
    REQUIRE(resp.error.find("something broke") != std::string::npos);
}

TEST_CASE("CommandRegistry: overwrite handler replaces previous", "[command_registry]") {
    CommandRegistry reg;

    reg.add("cmd", [](const json&) -> json { return "v1"; });

    IpcRequest req{"5", "cmd", {}};
    auto resp1 = reg.dispatch(req);
    REQUIRE(resp1.data == "v1");

    reg.add("cmd", [](const json&) -> json { return "v2"; });

    auto resp2 = reg.dispatch(req);
    REQUIRE(resp2.data == "v2");
}

TEST_CASE("CommandRegistry: async handler that doesnt reply returns error", "[command_registry]") {
    CommandRegistry reg;

    reg.add_async("noreply", [](const json& args, CommandReply reply) {
        // Intentionally don't call reply
    });

    IpcRequest req{"6", "noreply", {}};
    auto resp = reg.dispatch(req);

    REQUIRE_FALSE(resp.error.empty());
    REQUIRE(resp.error.find("did not complete") != std::string::npos);
}

TEST_CASE("CommandRegistry: dispatch with empty args", "[command_registry]") {
    CommandRegistry reg;

    reg.add("ping", [](const json&) -> json {
        return {{"pong", true}};
    });

    IpcRequest req{"7", "ping", json::object()};
    auto resp = reg.dispatch(req);

    REQUIRE(resp.error.empty());
    REQUIRE(resp.data["pong"] == true);
}
