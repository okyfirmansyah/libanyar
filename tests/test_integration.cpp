// LibAnyar — Integration Tests
// Tests that require a running LibAsyik service (IpcRouter, DbPlugin, etc.)

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>

#include <anyar/command_registry.h>
#include <anyar/event_bus.h>
#include <anyar/ipc_router.h>
#include <anyar/app_config.h>
#include <anyar/plugins/db_plugin.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>
#include <libasyik/sql.hpp>

#include <nlohmann/json.hpp>

#include <thread>
#include <chrono>
#include <atomic>
#include <random>

using namespace anyar;
using json = nlohmann::json;

// Pick a random ephemeral port to reduce collision risk between test runs
static int pick_test_port() {
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(49152, 60999);
    return dist(gen);
}

// ─── IpcRouter integration tests ────────────────────────────────────────────

// Helper: send HTTP POST to IPC invoke endpoint (must be called from a fiber)
static std::pair<int, json> http_invoke(asyik::service_ptr svc, int port,
                                         const json& body) {
    auto req = asyik::http_easy_request(
        svc, "POST",
        "http://127.0.0.1:" + std::to_string(port) + "/__anyar__/invoke",
        body.dump(),
        {{"Content-Type", "application/json"}}
    );
    int status_code = static_cast<int>(req->response.result());
    json result = json::parse(req->response.body);
    return {status_code, result};
}

TEST_CASE("IpcRouter: POST invoke dispatches to command registry", "[integration]") {
    CommandRegistry cmds;
    EventBus events;

    cmds.add("test:greet", [](const json& args) -> json {
        return "Hello, " + args.at("name").get<std::string>() + "!";
    });

    auto svc = asyik::make_service();
    int port = pick_test_port();

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);

        IpcRouter router(cmds, events);
        router.setup(server);

        // Send a test invoke
        auto [status, resp] = http_invoke(svc, port, {
            {"id", "1"},
            {"cmd", "test:greet"},
            {"args", {{"name", "World"}}}
        });

        REQUIRE(status == 200);
        REQUIRE(resp["data"] == "Hello, World!");
        REQUIRE(resp["error"].is_null());

        svc->stop();
    });

    svc->run();
}

TEST_CASE("IpcRouter: POST invoke unknown command returns error", "[integration]") {
    CommandRegistry cmds;
    EventBus events;

    auto svc = asyik::make_service();
    int port = pick_test_port();

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);

        IpcRouter router(cmds, events);
        router.setup(server);

        auto [status, resp] = http_invoke(svc, port, {
            {"id", "2"},
            {"cmd", "nonexistent:command"},
            {"args", json::object()}
        });

        REQUIRE(status == 200);
        REQUIRE(resp["data"].is_null());
        REQUIRE_FALSE(resp["error"].is_null());

        svc->stop();
    });

    svc->run();
}

TEST_CASE("IpcRouter: POST invoke with malformed JSON returns 400", "[integration]") {
    CommandRegistry cmds;
    EventBus events;

    auto svc = asyik::make_service();
    int port = pick_test_port();

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);

        IpcRouter router(cmds, events);
        router.setup(server);

        // Send raw invalid JSON directly
        auto req = asyik::http_easy_request(
            svc, "POST",
            "http://127.0.0.1:" + std::to_string(port) + "/__anyar__/invoke",
            "{ bad json %%%",
            {{"Content-Type", "application/json"}}
        );
        int status_code = static_cast<int>(req->response.result());
        json result = json::parse(req->response.body);

        REQUIRE(status_code == 400);
        REQUIRE_FALSE(result["error"].is_null());

        svc->stop();
    });

    svc->run();
}

// ─── DbPlugin integration tests ────────────────────────────────────────────

TEST_CASE("DbPlugin: open + exec + query + close round-trip", "[integration][db]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;

    auto svc = asyik::make_service();

    svc->execute([&]() {
        PluginContext ctx{svc, nullptr, cmds, events, cfg};
        DbPlugin db;
        db.initialize(ctx);

        // Open in-memory SQLite
        auto r_open = cmds.dispatch(IpcRequest{"1", "db:open", {
            {"backend", "sqlite3"},
            {"connStr", ":memory:"},
            {"poolSize", 1}
        }});
        REQUIRE(r_open.error.empty());
        std::string handle = r_open.data.get<std::string>();
        REQUIRE_FALSE(handle.empty());

        // Create table
        auto r_exec = cmds.dispatch(IpcRequest{"2", "db:exec", {
            {"handle", handle},
            {"sql", "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)"},
            {"params", json::array()}
        }});
        INFO("db:exec CREATE error: " << r_exec.error);
        REQUIRE(r_exec.error.empty());

        // Insert a row
        auto r_ins = cmds.dispatch(IpcRequest{"3", "db:exec", {
            {"handle", handle},
            {"sql", "INSERT INTO items (id, name) VALUES ($1, $2)"},
            {"params", {1, "apple"}}
        }});
        INFO("db:exec INSERT error: " << r_ins.error);
        REQUIRE(r_ins.error.empty());
        REQUIRE(r_ins.data["affectedRows"] == 1);

        // Insert another row
        cmds.dispatch(IpcRequest{"4", "db:exec", {
            {"handle", handle},
            {"sql", "INSERT INTO items (id, name) VALUES ($1, $2)"},
            {"params", {2, "banana"}}
        }});

        // Query
        auto r_q = cmds.dispatch(IpcRequest{"5", "db:query", {
            {"handle", handle},
            {"sql", "SELECT * FROM items ORDER BY id"},
            {"params", json::array()}
        }});
        INFO("db:query error: " << r_q.error);
        REQUIRE(r_q.error.empty());
        auto rows = r_q.data["rows"];
        REQUIRE(rows.size() == 2);
        REQUIRE(rows[0]["name"] == "apple");
        REQUIRE(rows[1]["name"] == "banana");

        // Close
        auto r_close = cmds.dispatch(IpcRequest{"6", "db:close", {
            {"handle", handle}
        }});
        REQUIRE(r_close.error.empty());
        REQUIRE(r_close.data["closed"] == true);

        db.shutdown();
        svc->stop();
    });

    svc->run();
}

TEST_CASE("DbPlugin: batch transaction commit", "[integration][db]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;

    auto svc = asyik::make_service();

    svc->execute([&]() {
        PluginContext ctx{svc, nullptr, cmds, events, cfg};
        DbPlugin db;
        db.initialize(ctx);

        // Open in-memory SQLite
        auto r_open = cmds.dispatch(IpcRequest{"1", "db:open", {
            {"backend", "sqlite3"}, {"connStr", ":memory:"}, {"poolSize", 1}
        }});
        std::string handle = r_open.data.get<std::string>();

        // Create table
        cmds.dispatch(IpcRequest{"2", "db:exec", {
            {"handle", handle},
            {"sql", "CREATE TABLE kv (key TEXT, val TEXT)"},
            {"params", json::array()}
        }});

        // Batch insert
        auto r_batch = cmds.dispatch(IpcRequest{"3", "db:batch", {
            {"handle", handle},
            {"statements", json::array({
                {{"sql", "INSERT INTO kv (key, val) VALUES ($1, $2)"}, {"params", {"a", "1"}}},
                {{"sql", "INSERT INTO kv (key, val) VALUES ($1, $2)"}, {"params", {"b", "2"}}},
                {{"sql", "INSERT INTO kv (key, val) VALUES ($1, $2)"}, {"params", {"c", "3"}}}
            })}
        }});
        REQUIRE(r_batch.error.empty());
        REQUIRE(r_batch.data.size() == 3);

        // Verify all rows inserted
        auto r_q = cmds.dispatch(IpcRequest{"4", "db:query", {
            {"handle", handle},
            {"sql", "SELECT * FROM kv ORDER BY key"},
            {"params", json::array()}
        }});
        REQUIRE(r_q.data["rows"].size() == 3);

        // Close
        cmds.dispatch(IpcRequest{"5", "db:close", {{"handle", handle}}});
        db.shutdown();
        svc->stop();
    });

    svc->run();
}

TEST_CASE("DbPlugin: close with bad handle returns error", "[integration][db]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;

    auto svc = asyik::make_service();

    svc->execute([&]() {
        PluginContext ctx{svc, nullptr, cmds, events, cfg};
        DbPlugin db;
        db.initialize(ctx);

        auto r = cmds.dispatch(IpcRequest{"1", "db:close", {
            {"handle", "nonexistent_handle"}
        }});
        REQUIRE_FALSE(r.error.empty());
        REQUIRE(r.error.find("Unknown database handle") != std::string::npos);

        db.shutdown();
        svc->stop();
    });

    svc->run();
}

// ─── Full IPC round-trip: IpcRouter + command via HTTP ──────────────────────

TEST_CASE("Full IPC round-trip: register command + HTTP invoke", "[integration]") {
    CommandRegistry cmds;
    EventBus events;

    cmds.add("math:add", [](const json& args) -> json {
        int a = args.at("a").get<int>();
        int b = args.at("b").get<int>();
        return a + b;
    });

    auto svc = asyik::make_service();
    int port = pick_test_port();

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);

        IpcRouter router(cmds, events);
        router.setup(server);

        auto [status, resp] = http_invoke(svc, port, {
            {"id", "calc-1"},
            {"cmd", "math:add"},
            {"args", {{"a", 17}, {"b", 25}}}
        });

        REQUIRE(status == 200);
        REQUIRE(resp["data"] == 42);

        svc->stop();
    });

    svc->run();
}
