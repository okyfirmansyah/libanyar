// LibAnyar — regression: window:close-all that arrives while the main window
// is still being created must still end app.run().
//
// The HTTP server (and so IPC) is live before the main window exists, and
// WebView2 takes ~2 s to create one.  The command used to find no main window
// and silently do nothing, leaving the app running forever.  Here a fiber
// queued in on_ready() — which runs as soon as the service thread starts, in
// parallel with window creation on the main thread — posts window:close-all
// to the app's own server.

#include <anyar/app.h>

#include <libasyik/http.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main() {
    using clock_type = std::chrono::steady_clock;
    const auto t0 = clock_type::now();
    auto elapsed_ms = [t0] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_type::now() - t0).count();
    };

    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.dist_path = "./dist-that-does-not-exist";
    anyar::App app(config);

    anyar::WindowConfig win;
    win.title = "Early close-all regression";
    win.width = 320;
    win.height = 180;
    app.create_window(win);

    std::atomic<long long> sent_ms{-1};
    app.on_ready([&]() {
        auto svc = app.service();
        int port = app.port();
        svc->execute([svc, port, &sent_ms, elapsed_ms]() {
            auto req = asyik::http_easy_request(
                svc, "POST",
                "http://127.0.0.1:" + std::to_string(port) + "/__anyar__/invoke",
                R"({"id":"1","cmd":"window:close-all","args":{}})",
                {{"Content-Type", "application/json"}});
            sent_ms.store(elapsed_ms());
            std::cerr << "[test] close-all answered " << static_cast<int>(req->response.result())
                      << " at " << sent_ms.load() << " ms\n";
        });
    });

    // Without the fix app.run() never returns; ctest TIMEOUT is 30 s.
    std::thread([&]() {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        std::cerr << "[FAIL] app.run() still running 20 s after start (close-all at "
                  << sent_ms.load() << " ms)\n";
        std::_Exit(3);
    }).detach();

    app.run();
    if (sent_ms.load() < 0) {
        std::cerr << "[FAIL] app.run() returned before close-all was sent\n";
        return 1;
    }
    std::cout << "[PASS] app.run() returned at " << elapsed_ms()
              << " ms (close-all at " << sent_ms.load() << " ms)\n";
    return 0;
}
