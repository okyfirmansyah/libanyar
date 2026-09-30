// LibAnyar — CommandRegistry Unit Tests

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>
#include <anyar/command_registry.h>

#include <libasyik/service.hpp>
#include <boost/fiber/operations.hpp>

#include <chrono>
#include <thread>

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

TEST_CASE("CommandRegistry: async handler may reply later from another thread", "[command_registry]") {
    CommandRegistry reg;
    std::thread worker;

    reg.add_async("later", [&](const json& args, CommandReply reply) {
        int n = args.value("n", 0);
        worker = std::thread([reply, n] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            reply({{"double", n * 2}}, "");
        });
    });

    IpcRequest req{"7", "later", {{"n", 21}}};
    auto resp = reg.dispatch(req);   // waits for the worker's reply
    worker.join();

    REQUIRE(resp.error.empty());
    REQUIRE(resp.data["double"] == 42);
}

TEST_CASE("CommandRegistry: async reply from another fiber does not block the service", "[command_registry]") {
    CommandRegistry reg;
    auto svc = asyik::make_service();

    reg.add_async("fiber-later", [&](const json&, CommandReply reply) {
        svc->execute([reply] {
            boost::this_fiber::sleep_for(std::chrono::milliseconds(20));
            reply("done", "");
        });
    });

    IpcResponse resp;
    int ticks = 0;
    svc->execute([&] {
        bool finished = false, ticker_exited = false;
        svc->execute([&] {   // proves the dispatching fiber only suspends itself
            while (!finished) { ++ticks; boost::this_fiber::sleep_for(std::chrono::milliseconds(2)); }
            ticker_exited = true;
        });
        resp = reg.dispatch({"8", "fiber-later", json::object()});
        finished = true;
        // Fibers must be gone before svc->stop() (libasyik rule).
        while (!ticker_exited) boost::this_fiber::sleep_for(std::chrono::milliseconds(1));
        svc->stop();
    });
    svc->run();

    REQUIRE(resp.error.empty());
    REQUIRE(resp.data == "done");
    REQUIRE(ticks >= 3);
}

TEST_CASE("CommandRegistry: async error reply and duplicate replies", "[command_registry]") {
    CommandRegistry reg;
    reg.add_async("twice", [](const json&, CommandReply reply) {
        reply(nullptr, "first wins");
        reply({{"ignored", true}}, "");
    });
    auto resp = reg.dispatch({"9", "twice", json::object()});
    REQUIRE(resp.error == "first wins");
}

TEST_CASE("CommandRegistry: async handler that throws reports the exception", "[command_registry]") {
    CommandRegistry reg;
    reg.add_async("throws", [](const json&, CommandReply) {
        throw std::runtime_error("kaboom");
    });
    auto resp = reg.dispatch({"10", "throws", json::object()});
    REQUIRE(resp.error.find("kaboom") != std::string::npos);
}
