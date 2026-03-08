// LibAnyar — FsPlugin Unit Tests
// Tests file system commands in a temporary directory.

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>
#include <anyar/command_registry.h>
#include <anyar/event_bus.h>
#include <anyar/app_config.h>
#include <anyar/plugins/fs_plugin.h>

#include <set>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace anyar;

// Helper: create a PluginContext with null service (FsPlugin doesn't need it)
static PluginContext make_ctx(CommandRegistry& cmds, EventBus& events, AppConfig& cfg) {
    return PluginContext{nullptr, nullptr, cmds, events, cfg};
}

// Helper: dispatch a command
static IpcResponse invoke(CommandRegistry& cmds, const std::string& cmd, const json& args = {}) {
    return cmds.dispatch(IpcRequest{"t", cmd, args});
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("anyar_test_" + std::to_string(::getpid()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

TEST_CASE("FsPlugin: writeFile + readFile roundtrip", "[fs_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    FsPlugin plugin;
    plugin.initialize(ctx);

    TempDir tmp;
    std::string file = (tmp.path / "hello.txt").string();

    auto w = invoke(cmds, "fs:writeFile", {{"path", file}, {"content", "Hello World!"}});
    REQUIRE(w.error.empty());

    auto r = invoke(cmds, "fs:readFile", {{"path", file}});
    REQUIRE(r.error.empty());
    REQUIRE(r.data == "Hello World!");
}

TEST_CASE("FsPlugin: exists returns true/false", "[fs_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    FsPlugin plugin;
    plugin.initialize(ctx);

    TempDir tmp;
    std::string file = (tmp.path / "exists_test.txt").string();

    auto r1 = invoke(cmds, "fs:exists", {{"path", file}});
    REQUIRE(r1.error.empty());
    REQUIRE(r1.data == false);

    // Create the file
    invoke(cmds, "fs:writeFile", {{"path", file}, {"content", "x"}});

    auto r2 = invoke(cmds, "fs:exists", {{"path", file}});
    REQUIRE(r2.error.empty());
    REQUIRE(r2.data == true);
}

TEST_CASE("FsPlugin: mkdir creates nested directories", "[fs_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    FsPlugin plugin;
    plugin.initialize(ctx);

    TempDir tmp;
    std::string nested = (tmp.path / "a" / "b" / "c").string();

    auto r = invoke(cmds, "fs:mkdir", {{"path", nested}});
    REQUIRE(r.error.empty());
    REQUIRE(fs::is_directory(nested));
}

TEST_CASE("FsPlugin: readDir returns directory entries", "[fs_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    FsPlugin plugin;
    plugin.initialize(ctx);

    TempDir tmp;

    // Create some files and a subdirectory
    invoke(cmds, "fs:writeFile", {{"path", (tmp.path / "file1.txt").string()}, {"content", "a"}});
    invoke(cmds, "fs:writeFile", {{"path", (tmp.path / "file2.txt").string()}, {"content", "b"}});
    invoke(cmds, "fs:mkdir", {{"path", (tmp.path / "subdir").string()}});

    auto r = invoke(cmds, "fs:readDir", {{"path", tmp.path.string()}});
    REQUIRE(r.error.empty());
    REQUIRE(r.data.is_array());
    REQUIRE(r.data.size() == 3);

    // Collect names
    std::set<std::string> names;
    for (auto& entry : r.data) {
        names.insert(entry["name"].get<std::string>());
    }
    REQUIRE(names.count("file1.txt") == 1);
    REQUIRE(names.count("file2.txt") == 1);
    REQUIRE(names.count("subdir") == 1);
}

TEST_CASE("FsPlugin: remove deletes file", "[fs_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    FsPlugin plugin;
    plugin.initialize(ctx);

    TempDir tmp;
    std::string file = (tmp.path / "to_delete.txt").string();
    invoke(cmds, "fs:writeFile", {{"path", file}, {"content", "bye"}});

    REQUIRE(fs::exists(file));

    auto r = invoke(cmds, "fs:remove", {{"path", file}});
    REQUIRE(r.error.empty());
    REQUIRE_FALSE(fs::exists(file));
}

TEST_CASE("FsPlugin: remove recursive deletes directory tree", "[fs_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    FsPlugin plugin;
    plugin.initialize(ctx);

    TempDir tmp;
    std::string dir = (tmp.path / "tree").string();
    invoke(cmds, "fs:mkdir", {{"path", dir + "/nested"}});
    invoke(cmds, "fs:writeFile", {{"path", dir + "/nested/file.txt"}, {"content", "x"}});

    auto r = invoke(cmds, "fs:remove", {{"path", dir}, {"recursive", true}});
    REQUIRE(r.error.empty());
    REQUIRE_FALSE(fs::exists(dir));
}

TEST_CASE("FsPlugin: metadata returns file info", "[fs_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    FsPlugin plugin;
    plugin.initialize(ctx);

    TempDir tmp;
    std::string file = (tmp.path / "meta.txt").string();
    invoke(cmds, "fs:writeFile", {{"path", file}, {"content", "1234567890"}});  // 10 bytes

    auto r = invoke(cmds, "fs:metadata", {{"path", file}});
    REQUIRE(r.error.empty());
    REQUIRE(r.data["size"] == 10);
    REQUIRE(r.data["isFile"] == true);
    REQUIRE(r.data["isDirectory"] == false);
}

TEST_CASE("FsPlugin: readFile on non-existent file returns error", "[fs_plugin]") {
    CommandRegistry cmds;
    EventBus events;
    AppConfig cfg;
    auto ctx = make_ctx(cmds, events, cfg);

    FsPlugin plugin;
    plugin.initialize(ctx);

    auto r = invoke(cmds, "fs:readFile", {{"path", "/tmp/anyar_nonexistent_file_xyz.txt"}});
    REQUIRE_FALSE(r.error.empty());
}
