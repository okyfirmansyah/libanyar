// LibAnyar — Shared Buffer Tests
// End-to-end tests for SharedBuffer, SharedBufferRegistry, SharedBufferPool,
// and the IPC buffer commands (buffer:create, buffer:write, etc.)

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>

#include <anyar/shared_buffer.h>
#include <anyar/command_registry.h>
#include <anyar/event_bus.h>
#include <anyar/ipc_router.h>
#include <anyar/app_config.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>

#include <nlohmann/json.hpp>

#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <random>

using namespace anyar;
using json = nlohmann::json;

// ── Helper: pick random port ────────────────────────────────────────────────

static int pick_test_port() {
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(49152, 60999);
    return dist(gen);
}

// ── Helper: HTTP invoke ─────────────────────────────────────────────────────

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

// ── Helper: simple base64 encode (for tests) ───────────────────────────────

static std::string base64_encode(const uint8_t* data, size_t len) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(4 * ((len + 2) / 3));
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

        out += b64[(n >> 18) & 0x3F];
        out += b64[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? b64[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? b64[n & 0x3F] : '=';
    }
    return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// Unit Tests — SharedBuffer
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("SharedBuffer: create and access raw memory", "[buffer]") {
    // Clean up any prior state
    SharedBufferRegistry::instance().clear();

    auto buf = SharedBuffer::create("test-buf-1", 4096);
    REQUIRE(buf != nullptr);
    REQUIRE(buf->name() == "test-buf-1");
    REQUIRE(buf->size() == 4096);
    REQUIRE(buf->data() != nullptr);

    // Write a known pattern and read it back
    std::memset(buf->data(), 0xAB, 4096);
    REQUIRE(buf->data()[0] == 0xAB);
    REQUIRE(buf->data()[4095] == 0xAB);

    // Should be registered
    auto found = SharedBufferRegistry::instance().get("test-buf-1");
    REQUIRE(found == buf);

    SharedBufferRegistry::instance().clear();
}

TEST_CASE("SharedBuffer: create unique names", "[buffer]") {
    SharedBufferRegistry::instance().clear();

    auto buf1 = SharedBuffer::create("unique-a", 1024);
    auto buf2 = SharedBuffer::create("unique-b", 2048);

    REQUIRE(buf1->name() == "unique-a");
    REQUIRE(buf2->name() == "unique-b");
    REQUIRE(buf1->size() == 1024);
    REQUIRE(buf2->size() == 2048);
    REQUIRE(buf1->data() != buf2->data());

    SharedBufferRegistry::instance().clear();
}

TEST_CASE("SharedBuffer: write and read different data patterns", "[buffer]") {
    SharedBufferRegistry::instance().clear();

    auto buf = SharedBuffer::create("pattern-test", 256);

    // Write sequential bytes
    for (size_t i = 0; i < 256; ++i) {
        buf->data()[i] = static_cast<uint8_t>(i);
    }

    // Verify each byte
    for (size_t i = 0; i < 256; ++i) {
        REQUIRE(buf->data()[i] == static_cast<uint8_t>(i));
    }

    SharedBufferRegistry::instance().clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// Unit Tests — SharedBufferRegistry
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("SharedBufferRegistry: add, get, remove, list", "[buffer][registry]") {
    auto& reg = SharedBufferRegistry::instance();
    reg.clear();

    auto buf1 = SharedBuffer::create("reg-a", 512);
    auto buf2 = SharedBuffer::create("reg-b", 1024);

    SECTION("get returns the correct buffer") {
        REQUIRE(reg.get("reg-a") == buf1);
        REQUIRE(reg.get("reg-b") == buf2);
        REQUIRE(reg.get("nonexistent") == nullptr);
    }

    SECTION("names lists all active buffers") {
        auto names = reg.names();
        REQUIRE(names.size() == 2);
        // Names may be in any order
        bool has_a = false, has_b = false;
        for (auto& n : names) {
            if (n == "reg-a") has_a = true;
            if (n == "reg-b") has_b = true;
        }
        REQUIRE(has_a);
        REQUIRE(has_b);
    }

    SECTION("remove deletes a buffer") {
        reg.remove("reg-a");
        REQUIRE(reg.get("reg-a") == nullptr);
        REQUIRE(reg.get("reg-b") != nullptr);
        auto names = reg.names();
        REQUIRE(names.size() == 1);
    }

    SECTION("clear removes all buffers") {
        reg.clear();
        REQUIRE(reg.get("reg-a") == nullptr);
        REQUIRE(reg.get("reg-b") == nullptr);
        auto names = reg.names();
        REQUIRE(names.empty());
    }

    reg.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// Unit Tests — SharedBufferPool
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("SharedBufferPool: creation and properties", "[buffer][pool]") {
    SharedBufferRegistry::instance().clear();

    SharedBufferPool pool("test-pool", 4096, 3);

    REQUIRE(pool.base_name() == "test-pool");
    REQUIRE(pool.buffer_size() == 4096);
    REQUIRE(pool.count() == 3);

    // Individual buffers should be in the registry
    for (size_t i = 0; i < 3; ++i) {
        std::string name = "test-pool_" + std::to_string(i);
        auto buf = SharedBufferRegistry::instance().get(name);
        REQUIRE(buf != nullptr);
        REQUIRE(buf->size() == 4096);
    }

    SharedBufferRegistry::instance().clear();
}

TEST_CASE("SharedBufferPool: acquire-write and release-write cycle", "[buffer][pool]") {
    SharedBufferRegistry::instance().clear();

    SharedBufferPool pool("cycle-pool", 1024, 2);

    // Acquire first buffer
    SharedBuffer& buf1 = pool.acquire_write();
    REQUIRE(buf1.size() == 1024);

    // Write test data
    std::memset(buf1.data(), 0x42, 1024);
    REQUIRE(buf1.data()[0] == 0x42);

    // Release write (marks as READY)
    pool.release_write(buf1, R"({"frame":1})");

    // Acquire second buffer (should be a different one since first is READY)
    SharedBuffer& buf2 = pool.acquire_write();
    REQUIRE(buf2.size() == 1024);

    // In a pool of 2, the second acquired should be different from the first
    // (first is still READY, not FREE)
    REQUIRE(&buf2 != &buf1);

    pool.release_write(buf2, R"({"frame":2})");

    // Release read on first to make it reusable
    pool.release_read(buf1.name());

    SharedBufferRegistry::instance().clear();
}

TEST_CASE("SharedBufferPool: release_read makes slot reusable", "[buffer][pool]") {
    SharedBufferRegistry::instance().clear();

    SharedBufferPool pool("reuse-pool", 512, 1);

    // Acquire the only slot
    SharedBuffer& buf = pool.acquire_write();
    std::memset(buf.data(), 0xFF, 512);
    pool.release_write(buf, "{}");

    // Release read
    pool.release_read(buf.name());

    // Should be able to acquire again
    SharedBuffer& buf2 = pool.acquire_write();
    REQUIRE(&buf2 == &buf);  // Same slot reused
    REQUIRE(buf2.data()[0] == 0xFF);  // Data persists until overwritten

    SharedBufferRegistry::instance().clear();
}

TEST_CASE("SharedBufferPool: destroy cleans up registry", "[buffer][pool]") {
    SharedBufferRegistry::instance().clear();

    {
        SharedBufferPool pool("cleanup-pool", 256, 3);
        REQUIRE(SharedBufferRegistry::instance().names().size() == 3);
    }
    // After pool destruction, buffers should be removed from registry
    REQUIRE(SharedBufferRegistry::instance().names().empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// Integration Tests — Buffer IPC Commands via HTTP
// ═════════════════════════════════════════════════════════════════════════════

// Helper to set up a service with buffer commands registered
struct BufferTestFixture {
    CommandRegistry cmds;
    EventBus events;
    std::map<std::string, std::unique_ptr<SharedBufferPool>> pools;

    void register_buffer_commands() {
        // Replicate the commands from App::register_buffer_commands()
        // (We can't use App directly because it requires webview/GTK)

        cmds.add("buffer:create", [this](const json& args) -> json {
            std::string name = args.at("name").get<std::string>();
            size_t size = args.at("size").get<size_t>();
            auto buf = SharedBuffer::create(name, size);
            if (!buf) throw std::runtime_error("Failed to create buffer: " + name);
            return json{{"name", buf->name()}, {"size", buf->size()},
                        {"url", "anyar-shm://" + buf->name()}};
        });

        cmds.add("buffer:write", [this](const json& args) -> json {
            std::string name = args.at("name").get<std::string>();
            std::string data_b64 = args.at("data").get<std::string>();
            size_t offset = args.value("offset", 0);
            auto buf = SharedBufferRegistry::instance().get(name);
            if (!buf) throw std::runtime_error("Buffer not found: " + name);

            // Base64 decode
            static const std::string b64chars =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::vector<uint8_t> out;
            out.reserve(data_b64.size() * 3 / 4);
            int val = 0, valb = -8;
            for (unsigned char c : data_b64) {
                if (c == '=') break;
                auto pos = b64chars.find(c);
                if (pos == std::string::npos) continue;
                val = (val << 6) + static_cast<int>(pos);
                valb += 6;
                if (valb >= 0) {
                    out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
            if (offset + out.size() > buf->size())
                throw std::runtime_error("Write exceeds buffer size");
            std::memcpy(buf->data() + offset, out.data(), out.size());
            return json{{"ok", true}, {"bytes_written", out.size()}};
        });

        cmds.add("buffer:destroy", [this](const json& args) -> json {
            std::string name = args.at("name").get<std::string>();
            SharedBufferRegistry::instance().remove(name);
            return json{{"ok", true}};
        });

        cmds.add("buffer:list", [this](const json& args) -> json {
            auto names = SharedBufferRegistry::instance().names();
            json list = json::array();
            for (auto& n : names) {
                auto buf = SharedBufferRegistry::instance().get(n);
                if (buf) {
                    list.push_back(json{{"name", buf->name()}, {"size", buf->size()},
                                        {"url", "anyar-shm://" + buf->name()}});
                }
            }
            return json{{"buffers", list}};
        });

        cmds.add("buffer:notify", [this](const json& args) -> json {
            std::string name = args.at("name").get<std::string>();
            json metadata = args.value("metadata", json::object());
            auto buf = SharedBufferRegistry::instance().get(name);
            if (!buf) throw std::runtime_error("Buffer not found: " + name);
            json payload = {{"name", name}, {"url", "anyar-shm://" + name},
                            {"size", buf->size()}, {"metadata", metadata}};
            events.emit("buffer:ready", payload);
            return json{{"ok", true}};
        });

        cmds.add("buffer:pool-create", [this](const json& args) -> json {
            std::string name = args.at("name").get<std::string>();
            size_t buffer_size = args.at("bufferSize").get<size_t>();
            size_t count = args.value("count", 3);
            if (pools.count(name))
                throw std::runtime_error("Pool already exists: " + name);
            auto pool = std::make_unique<SharedBufferPool>(name, buffer_size, count);
            json buffers = json::array();
            for (size_t i = 0; i < count; ++i) {
                std::string buf_name = name + "_" + std::to_string(i);
                auto buf = SharedBufferRegistry::instance().get(buf_name);
                if (buf) {
                    buffers.push_back(json{{"name", buf->name()}, {"size", buf->size()},
                                            {"url", "anyar-shm://" + buf->name()}});
                }
            }
            pools[name] = std::move(pool);
            return json{{"name", name}, {"bufferSize", buffer_size},
                        {"count", count}, {"buffers", buffers}};
        });

        cmds.add("buffer:pool-destroy", [this](const json& args) -> json {
            std::string name = args.at("name").get<std::string>();
            auto it = pools.find(name);
            if (it == pools.end())
                throw std::runtime_error("Pool not found: " + name);
            pools.erase(it);
            return json{{"ok", true}};
        });

        cmds.add("buffer:pool-acquire", [this](const json& args) -> json {
            std::string name = args.at("name").get<std::string>();
            auto it = pools.find(name);
            if (it == pools.end())
                throw std::runtime_error("Pool not found: " + name);
            SharedBuffer& buf = it->second->acquire_write();
            return json{{"name", buf.name()}, {"size", buf.size()},
                        {"url", "anyar-shm://" + buf.name()}};
        });

        cmds.add("buffer:pool-release-write", [this](const json& args) -> json {
            std::string pool_name = args.at("pool").get<std::string>();
            std::string buf_name = args.at("name").get<std::string>();
            json metadata = args.value("metadata", json::object());
            auto it = pools.find(pool_name);
            if (it == pools.end())
                throw std::runtime_error("Pool not found: " + pool_name);
            auto buf = SharedBufferRegistry::instance().get(buf_name);
            if (!buf) throw std::runtime_error("Buffer not found: " + buf_name);
            it->second->release_write(*buf, metadata.dump());
            json payload = {{"name", buf_name}, {"pool", pool_name},
                            {"url", "anyar-shm://" + buf_name},
                            {"size", buf->size()}, {"metadata", metadata}};
            events.emit("buffer:ready", payload);
            return json{{"ok", true}};
        });

        cmds.add("buffer:pool-release-read", [this](const json& args) -> json {
            std::string pool_name = args.at("pool").get<std::string>();
            std::string buf_name = args.at("name").get<std::string>();
            auto it = pools.find(pool_name);
            if (it == pools.end())
                throw std::runtime_error("Pool not found: " + pool_name);
            it->second->release_read(buf_name);
            return json{{"ok", true}};
        });
    }

    ~BufferTestFixture() {
        pools.clear();
        SharedBufferRegistry::instance().clear();
    }
};

// ── IPC E2E: buffer:create + buffer:list + buffer:destroy ───────────────────

TEST_CASE("IPC E2E: buffer create, list, and destroy", "[buffer][integration]") {
    SharedBufferRegistry::instance().clear();
    BufferTestFixture fixture;
    fixture.register_buffer_commands();

    auto svc = asyik::make_service();
    int port = pick_test_port();

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        IpcRouter router(fixture.cmds, fixture.events);
        router.setup(server);

        // Create a buffer
        auto [s1, r1] = http_invoke(svc, port, {
            {"id", "1"}, {"cmd", "buffer:create"},
            {"args", {{"name", "ipc-test-buf"}, {"size", 2048}}}
        });
        REQUIRE(s1 == 200);
        REQUIRE(r1["data"]["name"] == "ipc-test-buf");
        REQUIRE(r1["data"]["size"] == 2048);
        REQUIRE(r1["data"]["url"] == "anyar-shm://ipc-test-buf");

        // List buffers
        auto [s2, r2] = http_invoke(svc, port, {
            {"id", "2"}, {"cmd", "buffer:list"},
            {"args", json::object()}
        });
        REQUIRE(s2 == 200);
        REQUIRE(r2["data"]["buffers"].size() == 1);
        REQUIRE(r2["data"]["buffers"][0]["name"] == "ipc-test-buf");

        // Destroy the buffer
        auto [s3, r3] = http_invoke(svc, port, {
            {"id", "3"}, {"cmd", "buffer:destroy"},
            {"args", {{"name", "ipc-test-buf"}}}
        });
        REQUIRE(s3 == 200);
        REQUIRE(r3["data"]["ok"] == true);

        // List should be empty now
        auto [s4, r4] = http_invoke(svc, port, {
            {"id", "4"}, {"cmd", "buffer:list"},
            {"args", json::object()}
        });
        REQUIRE(s4 == 200);
        REQUIRE(r4["data"]["buffers"].empty());

        svc->stop();
    });

    svc->run();
}

// ── IPC E2E: buffer:create + buffer:write + read back from memory ───────────

TEST_CASE("IPC E2E: buffer write via base64 and verify data", "[buffer][integration]") {
    SharedBufferRegistry::instance().clear();
    BufferTestFixture fixture;
    fixture.register_buffer_commands();

    auto svc = asyik::make_service();
    int port = pick_test_port();

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        IpcRouter router(fixture.cmds, fixture.events);
        router.setup(server);

        // Create buffer
        auto [s1, r1] = http_invoke(svc, port, {
            {"id", "1"}, {"cmd", "buffer:create"},
            {"args", {{"name", "write-test"}, {"size", 256}}}
        });
        REQUIRE(s1 == 200);

        // Prepare test data: [0, 1, 2, ..., 15]
        uint8_t test_data[16];
        for (int i = 0; i < 16; ++i) test_data[i] = static_cast<uint8_t>(i);
        std::string b64 = base64_encode(test_data, 16);

        // Write via IPC
        auto [s2, r2] = http_invoke(svc, port, {
            {"id", "2"}, {"cmd", "buffer:write"},
            {"args", {{"name", "write-test"}, {"data", b64}}}
        });
        REQUIRE(s2 == 200);
        REQUIRE(r2["data"]["ok"] == true);
        REQUIRE(r2["data"]["bytes_written"] == 16);

        // Verify data in shared memory
        auto buf = SharedBufferRegistry::instance().get("write-test");
        REQUIRE(buf != nullptr);
        for (int i = 0; i < 16; ++i) {
            REQUIRE(buf->data()[i] == static_cast<uint8_t>(i));
        }

        svc->stop();
    });

    svc->run();
    SharedBufferRegistry::instance().clear();
}

// ── IPC E2E: buffer:write with offset ───────────────────────────────────────

TEST_CASE("IPC E2E: buffer write with offset", "[buffer][integration]") {
    SharedBufferRegistry::instance().clear();
    BufferTestFixture fixture;
    fixture.register_buffer_commands();

    auto svc = asyik::make_service();
    int port = pick_test_port();

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        IpcRouter router(fixture.cmds, fixture.events);
        router.setup(server);

        // Create buffer and zero it
        auto [s1, r1] = http_invoke(svc, port, {
            {"id", "1"}, {"cmd", "buffer:create"},
            {"args", {{"name", "offset-test"}, {"size", 128}}}
        });
        REQUIRE(s1 == 200);

        auto buf = SharedBufferRegistry::instance().get("offset-test");
        std::memset(buf->data(), 0, 128);

        // Write 4 bytes at offset 10
        uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        std::string b64 = base64_encode(payload, 4);

        auto [s2, r2] = http_invoke(svc, port, {
            {"id", "2"}, {"cmd", "buffer:write"},
            {"args", {{"name", "offset-test"}, {"data", b64}, {"offset", 10}}}
        });
        REQUIRE(s2 == 200);
        REQUIRE(r2["data"]["bytes_written"] == 4);

        // Verify
        REQUIRE(buf->data()[9] == 0x00);   // Before offset: zero
        REQUIRE(buf->data()[10] == 0xDE);
        REQUIRE(buf->data()[11] == 0xAD);
        REQUIRE(buf->data()[12] == 0xBE);
        REQUIRE(buf->data()[13] == 0xEF);
        REQUIRE(buf->data()[14] == 0x00);  // After data: zero

        svc->stop();
    });

    svc->run();
    SharedBufferRegistry::instance().clear();
}

// ── IPC E2E: buffer:notify emits event ──────────────────────────────────────

TEST_CASE("IPC E2E: buffer notify emits buffer:ready event", "[buffer][integration]") {
    SharedBufferRegistry::instance().clear();
    BufferTestFixture fixture;
    fixture.register_buffer_commands();

    auto svc = asyik::make_service();
    int port = pick_test_port();

    std::atomic<bool> event_received{false};
    std::string received_name;

    fixture.events.on("buffer:ready", [&](const json& payload) {
        received_name = payload.at("name").get<std::string>();
        event_received.store(true);
    });

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        IpcRouter router(fixture.cmds, fixture.events);
        router.setup(server);

        // Create buffer
        http_invoke(svc, port, {
            {"id", "1"}, {"cmd", "buffer:create"},
            {"args", {{"name", "notify-test"}, {"size", 512}}}
        });

        // Notify
        auto [s, r] = http_invoke(svc, port, {
            {"id", "2"}, {"cmd", "buffer:notify"},
            {"args", {{"name", "notify-test"}, {"metadata", {{"frame", 42}}}}}
        });
        REQUIRE(s == 200);
        REQUIRE(r["data"]["ok"] == true);

        // Event should have fired
        REQUIRE(event_received.load());
        REQUIRE(received_name == "notify-test");

        svc->stop();
    });

    svc->run();
    SharedBufferRegistry::instance().clear();
}

// ── IPC E2E: pool create, acquire, release-write, release-read cycle ────────

TEST_CASE("IPC E2E: full pool lifecycle", "[buffer][pool][integration]") {
    SharedBufferRegistry::instance().clear();
    BufferTestFixture fixture;
    fixture.register_buffer_commands();

    auto svc = asyik::make_service();
    int port = pick_test_port();

    std::atomic<int> frame_count{0};
    fixture.events.on("buffer:ready", [&](const json& payload) {
        frame_count.fetch_add(1);
    });

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        IpcRouter router(fixture.cmds, fixture.events);
        router.setup(server);

        // Create pool with 2 buffers
        auto [s1, r1] = http_invoke(svc, port, {
            {"id", "1"}, {"cmd", "buffer:pool-create"},
            {"args", {{"name", "video"}, {"bufferSize", 1024}, {"count", 2}}}
        });
        REQUIRE(s1 == 200);
        REQUIRE(r1["data"]["name"] == "video");
        REQUIRE(r1["data"]["count"] == 2);
        REQUIRE(r1["data"]["buffers"].size() == 2);

        // Acquire slot 0
        auto [s2, r2] = http_invoke(svc, port, {
            {"id", "2"}, {"cmd", "buffer:pool-acquire"},
            {"args", {{"name", "video"}}}
        });
        REQUIRE(s2 == 200);
        std::string buf0_name = r2["data"]["name"].get<std::string>();
        INFO("Acquired buffer: " << buf0_name);

        // Write data into acquired buffer
        auto buf0 = SharedBufferRegistry::instance().get(buf0_name);
        REQUIRE(buf0 != nullptr);
        std::memset(buf0->data(), 0xAA, 1024);

        // Release write → triggers buffer:ready event
        auto [s3, r3] = http_invoke(svc, port, {
            {"id", "3"}, {"cmd", "buffer:pool-release-write"},
            {"args", {{"pool", "video"}, {"name", buf0_name},
                      {"metadata", {{"width", 32}, {"height", 32}}}}}
        });
        REQUIRE(s3 == 200);
        REQUIRE(r3["data"]["ok"] == true);
        REQUIRE(frame_count.load() == 1);

        // Consumer releases the buffer back to the pool
        auto [s4, r4] = http_invoke(svc, port, {
            {"id", "4"}, {"cmd", "buffer:pool-release-read"},
            {"args", {{"pool", "video"}, {"name", buf0_name}}}
        });
        REQUIRE(s4 == 200);
        REQUIRE(r4["data"]["ok"] == true);

        // Should be able to acquire again (buffer recycled)
        auto [s5, r5] = http_invoke(svc, port, {
            {"id", "5"}, {"cmd", "buffer:pool-acquire"},
            {"args", {{"name", "video"}}}
        });
        REQUIRE(s5 == 200);
        // The recycled buffer should still have our data
        std::string reacquired = r5["data"]["name"].get<std::string>();
        auto reacquired_buf = SharedBufferRegistry::instance().get(reacquired);
        REQUIRE(reacquired_buf != nullptr);

        // Destroy pool
        auto [s6, r6] = http_invoke(svc, port, {
            {"id", "6"}, {"cmd", "buffer:pool-destroy"},
            {"args", {{"name", "video"}}}
        });
        REQUIRE(s6 == 200);

        svc->stop();
    });

    svc->run();
    SharedBufferRegistry::instance().clear();
}

// ── IPC E2E: error cases ────────────────────────────────────────────────────

TEST_CASE("IPC E2E: buffer error handling", "[buffer][integration]") {
    SharedBufferRegistry::instance().clear();
    BufferTestFixture fixture;
    fixture.register_buffer_commands();

    auto svc = asyik::make_service();
    int port = pick_test_port();

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        IpcRouter router(fixture.cmds, fixture.events);
        router.setup(server);

        SECTION("destroy nonexistent buffer") {
            auto [s, r] = http_invoke(svc, port, {
                {"id", "1"}, {"cmd", "buffer:destroy"},
                {"args", {{"name", "nonexistent"}}}
            });
            // Should succeed (remove is a no-op for missing keys)
            REQUIRE(s == 200);
        }

        SECTION("write to nonexistent buffer returns error") {
            auto [s, r] = http_invoke(svc, port, {
                {"id", "2"}, {"cmd", "buffer:write"},
                {"args", {{"name", "no-such-buf"}, {"data", "AAAA"}}}
            });
            REQUIRE(s == 200);
            REQUIRE_FALSE(r["error"].is_null());
        }

        SECTION("notify on nonexistent buffer returns error") {
            auto [s, r] = http_invoke(svc, port, {
                {"id", "3"}, {"cmd", "buffer:notify"},
                {"args", {{"name", "no-such-buf"}, {"metadata", json::object()}}}
            });
            REQUIRE(s == 200);
            REQUIRE_FALSE(r["error"].is_null());
        }

        SECTION("pool acquire on nonexistent pool returns error") {
            auto [s, r] = http_invoke(svc, port, {
                {"id", "4"}, {"cmd", "buffer:pool-acquire"},
                {"args", {{"name", "no-such-pool"}}}
            });
            REQUIRE(s == 200);
            REQUIRE_FALSE(r["error"].is_null());
        }

        SECTION("pool-release-read on nonexistent pool returns error") {
            auto [s, r] = http_invoke(svc, port, {
                {"id", "5"}, {"cmd", "buffer:pool-release-read"},
                {"args", {{"pool", "no-pool"}, {"name", "no-buf"}}}
            });
            REQUIRE(s == 200);
            REQUIRE_FALSE(r["error"].is_null());
        }

        svc->stop();
    });

    svc->run();
    SharedBufferRegistry::instance().clear();
}

// ── IPC E2E: concurrent write + notify round-trip ───────────────────────────

TEST_CASE("IPC E2E: native C++ write → IPC notify → event verification", "[buffer][integration]") {
    SharedBufferRegistry::instance().clear();
    BufferTestFixture fixture;
    fixture.register_buffer_commands();

    auto svc = asyik::make_service();
    int port = pick_test_port();

    // Track all buffer:ready events
    std::vector<json> received_events;
    std::mutex events_mu;
    fixture.events.on("buffer:ready", [&](const json& payload) {
        std::lock_guard<std::mutex> lock(events_mu);
        received_events.push_back(payload);
    });

    svc->execute([&]() {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        IpcRouter router(fixture.cmds, fixture.events);
        router.setup(server);

        // Create a buffer via IPC
        auto [s1, r1] = http_invoke(svc, port, {
            {"id", "1"}, {"cmd", "buffer:create"},
            {"args", {{"name", "e2e-frame"}, {"size", 4096}}}
        });
        REQUIRE(s1 == 200);

        // Simulate C++ backend writing pixel data directly into shared memory
        auto buf = SharedBufferRegistry::instance().get("e2e-frame");
        REQUIRE(buf != nullptr);

        // Write a gradient pattern (like RGBA pixels)
        for (size_t i = 0; i < 4096; i += 4) {
            buf->data()[i + 0] = static_cast<uint8_t>(i / 4);        // R
            buf->data()[i + 1] = static_cast<uint8_t>(255 - i / 4);  // G
            buf->data()[i + 2] = 128;                                  // B
            buf->data()[i + 3] = 255;                                  // A
        }

        // Notify via IPC (this is what backend does after writing frame)
        auto [s2, r2] = http_invoke(svc, port, {
            {"id", "2"}, {"cmd", "buffer:notify"},
            {"args", {{"name", "e2e-frame"},
                      {"metadata", {{"width", 32}, {"height", 32}, {"format", "rgba"}}}}}
        });
        REQUIRE(s2 == 200);

        // Verify event was received with correct metadata
        {
            std::lock_guard<std::mutex> lock(events_mu);
            REQUIRE(received_events.size() == 1);
            REQUIRE(received_events[0]["name"] == "e2e-frame");
            REQUIRE(received_events[0]["url"] == "anyar-shm://e2e-frame");
            REQUIRE(received_events[0]["size"] == 4096);
            REQUIRE(received_events[0]["metadata"]["width"] == 32);
            REQUIRE(received_events[0]["metadata"]["height"] == 32);
            REQUIRE(received_events[0]["metadata"]["format"] == "rgba");
        }

        // Verify the data is still accessible (zero-copy: same mmap'd memory)
        REQUIRE(buf->data()[0] == 0);      // R of pixel 0
        REQUIRE(buf->data()[1] == 255);    // G of pixel 0
        REQUIRE(buf->data()[2] == 128);    // B of pixel 0
        REQUIRE(buf->data()[3] == 255);    // A of pixel 0

        // Cleanup
        auto [s3, r3] = http_invoke(svc, port, {
            {"id", "3"}, {"cmd", "buffer:destroy"},
            {"args", {{"name", "e2e-frame"}}}
        });
        REQUIRE(s3 == 200);

        svc->stop();
    });

    svc->run();
    SharedBufferRegistry::instance().clear();
}

// ── Stress test: multiple buffers created and destroyed ─────────────────────

TEST_CASE("Buffer stress: create and destroy many buffers", "[buffer][stress]") {
    SharedBufferRegistry::instance().clear();

    constexpr int N = 50;
    std::vector<std::shared_ptr<SharedBuffer>> buffers;

    for (int i = 0; i < N; ++i) {
        auto buf = SharedBuffer::create("stress-" + std::to_string(i), 1024);
        REQUIRE(buf != nullptr);
        // Write unique pattern
        std::memset(buf->data(), static_cast<uint8_t>(i), 1024);
        buffers.push_back(buf);
    }

    REQUIRE(SharedBufferRegistry::instance().names().size() == N);

    // Verify each buffer has correct data
    for (int i = 0; i < N; ++i) {
        REQUIRE(buffers[i]->data()[0] == static_cast<uint8_t>(i));
        REQUIRE(buffers[i]->data()[1023] == static_cast<uint8_t>(i));
    }

    // Clean up
    buffers.clear();
    SharedBufferRegistry::instance().clear();
    REQUIRE(SharedBufferRegistry::instance().names().empty());
}

// ── Pool streaming simulation ───────────────────────────────────────────────

TEST_CASE("Pool streaming: multi-frame produce-consume cycle", "[buffer][pool][stress]") {
    SharedBufferRegistry::instance().clear();

    constexpr size_t FRAME_SIZE = 1024;
    constexpr int NUM_FRAMES = 10;

    SharedBufferPool pool("stream", FRAME_SIZE, 3);

    for (int frame = 0; frame < NUM_FRAMES; ++frame) {
        // Producer: acquire, write, release
        SharedBuffer& buf = pool.acquire_write();
        std::memset(buf.data(), static_cast<uint8_t>(frame + 1), FRAME_SIZE);
        pool.release_write(buf, "{\"frame\":" + std::to_string(frame) + "}");

        // Consumer: verify data, release read
        REQUIRE(buf.data()[0] == static_cast<uint8_t>(frame + 1));
        REQUIRE(buf.data()[FRAME_SIZE - 1] == static_cast<uint8_t>(frame + 1));
        pool.release_read(buf.name());
    }

    SharedBufferRegistry::instance().clear();
}
