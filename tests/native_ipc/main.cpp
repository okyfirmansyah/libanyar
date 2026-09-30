// LibAnyar — native IPC end-to-end test (real webview, requires a display).
//
// A generated page exercises, from inside the webview:
//   1. command round-trip over window.__anyar_ipc__ (bind → fiber → return)
//   2. C++ → JS event push (EventBus window sink → eval)
//   3. SharedBuffer fetch over the platform's buffer transport
//   4. a command that hops to the UI thread (run_on_main_thread)
// then reports the results and calls window:close-all, which quits the UI
// loop from a service-thread fiber (cross-thread terminate).

#include <anyar/app.h>
#include <anyar/shared_buffer.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const char* kPage = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>native ipc e2e</title></head>
<body>
<script>
async function invoke(cmd, args) {
  const r = await window.__anyar_ipc__(JSON.stringify({ id: String(Math.random()), cmd, args }));
  if (r.error) throw new Error(cmd + ': ' + JSON.stringify(r.error));
  return r.data;
}
async function run() {
  const results = {};
  try {
    // 1. command round-trip
    const echo = await invoke('test:echo', { msg: 'hello' });
    results.echo = echo && echo.msg === 'hello';

    // 2. C++ -> JS event push
    const got = new Promise((resolve) => {
      const ls = window.__anyar_event_listeners__;
      (ls['test:ping'] = ls['test:ping'] || []).push(resolve);
    });
    await invoke('test:emit', {});
    const ping = await Promise.race([got, new Promise((r) => setTimeout(() => r(null), 3000))]);
    results.event = !!ping && ping.n === 42;

    // 3. SharedBuffer bytes, over anyar-shm:// only where it is served
    const useScheme = window.__LIBANYAR_SHM_SCHEME__ !== false;
    const url = useScheme ? 'anyar-shm://e2e-buf'
                          : '/__anyar__/buffer/e2e-buf';
    const bytes = new Uint8Array(await (await fetch(url)).arrayBuffer());
    results.buffer = bytes.length === 4 && bytes[0] === 1 && bytes[3] === 4;
    results.shm_scheme = useScheme;

    // 4. UI-thread hop
    await invoke('window:set-title', { label: 'main', title: 'native-ipc-ok' });
    results.set_title = true;
  } catch (e) {
    results.exception = String(e);
  }
  await invoke('test:report', results);
  await invoke('window:close-all', {});
}
window.addEventListener('load', run);
</script>
</body></html>)HTML";

std::atomic<bool> g_reported{false};
json g_report;

}  // namespace

int main() {
    // Page lives in a scratch dist dir (absolute path → used as-is).
    fs::path dist = fs::temp_directory_path() /
                    ("anyar-native-ipc-" + std::to_string(std::random_device{}()));
    fs::create_directories(dist);
    std::ofstream(dist / "index.html") << kPage;

    // Watchdog: WebView2/WebKit startup can be slow on CI; 25 s total.
    std::thread([dist]() {
        std::this_thread::sleep_for(std::chrono::seconds(25));
        std::cerr << "[FAIL] timed out (reported=" << g_reported.load() << ")\n";
        std::error_code ec;
        fs::remove_all(dist, ec);
        std::_Exit(3);
    }).detach();

    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.dist_path = dist.string();
    anyar::App app(config);

    auto buf = anyar::SharedBuffer::create("e2e-buf", 4);
    const uint8_t pattern[4] = {1, 2, 3, 4};
    std::copy(pattern, pattern + 4, buf->data());

    app.command("test:echo", [](const json& args) -> json {
        return {{"msg", args.value("msg", "")}};
    });
    app.command("test:emit", [&app](const json&) -> json {
        app.emit("test:ping", {{"n", 42}});
        return nullptr;
    });
    app.command("test:report", [](const json& args) -> json {
        g_report = args;
        g_report.erase("_caller_label");
        g_reported.store(true);
        return nullptr;
    });

    anyar::WindowConfig win;
    win.title = "Native IPC E2E";
    win.width = 400;
    win.height = 240;
    app.create_window(win);

    app.run();

    std::error_code ec;
    fs::remove_all(dist, ec);

    if (!g_reported.load()) {
        std::cerr << "[FAIL] page never reported results\n";
        return 1;
    }
    std::cout << "[report] " << g_report.dump() << "\n";
    bool ok = g_report.value("echo", false) && g_report.value("event", false) &&
              g_report.value("buffer", false) && g_report.value("set_title", false) &&
              !g_report.contains("exception");
#ifdef _WIN32
    // WebView2 has no anyar-shm:// handler yet; the page must use HTTP.
    ok = ok && g_report.value("shm_scheme", true) == false;
#endif
    std::cout << (ok ? "[PASS]" : "[FAIL]") << " native IPC end-to-end\n";
    return ok ? 0 : 1;
}
