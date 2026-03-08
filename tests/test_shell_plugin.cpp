// LibAnyar — ShellPlugin Unit Tests
// Tests shell:execute with safe commands (echo, ls, etc.)

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>
#include <anyar/command_registry.h>
#include <anyar/event_bus.h>
#include <anyar/app_config.h>
#include <anyar/plugins/shell_plugin.h>

#include <filesystem>

using namespace anyar;

static PluginContext make_ctx(CommandRegistry& cmds, EventBus& events, AppConfig& cfg) {
    return PluginContext{nullptr, nullptr, cmds, events, cfg};
}

static IpcResponse invoke(CommandRegistry& cmds, const std::string& cmd, const json& args = {}) {
    return cmds.dispatch(IpcRequest{"t", cmd, args});
}

TEST_CASE("ShellPlugin: execute echo returns stdout", "[shell_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    ShellPlugin plugin;
    plugin.initialize(ctx);

    auto r = invoke(cmds, "shell:execute", {{"program", "echo"}, {"args", {"hello"}}});
    REQUIRE(r.error.empty());
    REQUIRE(r.data["code"] == 0);
    REQUIRE(r.data["stdout"].get<std::string>().find("hello") != std::string::npos);
}

TEST_CASE("ShellPlugin: execute with multiple args", "[shell_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    ShellPlugin plugin;
    plugin.initialize(ctx);

    auto r = invoke(cmds, "shell:execute", {
        {"program", "printf"},
        {"args", {"%s %s", "hello", "world"}}
    });
    REQUIRE(r.error.empty());
    REQUIRE(r.data["code"] == 0);
    REQUIRE(r.data["stdout"] == "hello world");
}

TEST_CASE("ShellPlugin: execute bad command returns non-zero exit", "[shell_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    ShellPlugin plugin;
    plugin.initialize(ctx);

    auto r = invoke(cmds, "shell:execute", {
        {"program", "/bin/false"},
        {"args", json::array()}
    });
    REQUIRE(r.error.empty());
    REQUIRE(r.data["code"] != 0);
}

TEST_CASE("ShellPlugin: execute nonexistent program returns code 127", "[shell_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    ShellPlugin plugin;
    plugin.initialize(ctx);

    auto r = invoke(cmds, "shell:execute", {
        {"program", "nonexistent_binary_xyz_abc"},
        {"args", json::array()}
    });
    REQUIRE(r.error.empty());
    REQUIRE(r.data["code"] == 127);
}

TEST_CASE("ShellPlugin: execute with cwd option", "[shell_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    ShellPlugin plugin;
    plugin.initialize(ctx);

    auto r = invoke(cmds, "shell:execute", {
        {"program", "pwd"},
        {"args", json::array()},
        {"cwd", "/tmp"}
    });
    REQUIRE(r.error.empty());
    REQUIRE(r.data["code"] == 0);
    REQUIRE(r.data["stdout"].get<std::string>().find("/tmp") != std::string::npos);
}

TEST_CASE("ShellPlugin: execute captures stderr", "[shell_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    ShellPlugin plugin;
    plugin.initialize(ctx);

    // "ls /nonexistent" should produce something on stderr
    auto r = invoke(cmds, "shell:execute", {
        {"program", "ls"},
        {"args", {"/nonexistent_path_xyz"}}
    });
    REQUIRE(r.data["code"] != 0);
    REQUIRE_FALSE(r.data["stderr"].get<std::string>().empty());
}
