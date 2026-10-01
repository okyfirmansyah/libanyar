#include <anyar/app.h>
#include <anyar/window.h>

#ifdef __linux__
#include <gtk/gtk.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
using clock_type = std::chrono::steady_clock;
const auto t_start = clock_type::now();
// Set when the native close is issued; the watchdog times shutdown only,
// so slow WebKit startup (CI xvfb + software GL) cannot eat the budget.
std::atomic<long long> close_issued_ms{-1};

long long elapsed_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        clock_type::now() - t_start).count();
}

// Ask the window manager to close the window ~200 ms from now, exactly as
// the title-bar X button would.
void schedule_native_close(void* native) {
#ifdef __linux__
    g_timeout_add(200, +[](gpointer data) -> gboolean {
        close_issued_ms.store(elapsed_ms());
        std::cerr << "[test] native close issued at " << close_issued_ms.load() << " ms\n";
        gtk_window_close(GTK_WINDOW(data));
        return G_SOURCE_REMOVE;
    }, native);
#elif defined(_WIN32)
    HWND hwnd = static_cast<HWND>(native);
    std::thread([hwnd]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        close_issued_ms.store(elapsed_ms());
        std::cerr << "[test] native close issued at " << close_issued_ms.load() << " ms\n";
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }).detach();
#endif
}
}  // namespace

int main() {
#if !defined(__linux__) && !defined(_WIN32)
    return 0;
#else
    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.debug = false;
    config.dist_path = "./dist-that-does-not-exist";

    anyar::App app(config);

    anyar::WindowConfig win;
    win.title = "Window Close Regression";
    win.width = 320;
    win.height = 180;
    win.resizable = false;
    win.debug = false;
    app.create_window(win);

    app.on_window_ready([](anyar::Window& window) {
        std::cerr << "[test] window ready at " << elapsed_ms() << " ms\n";
        void* native = window.native_handle();
        if (!native) {
            std::cerr << "[FAIL] native_handle() returned null\n";
            std::_Exit(2);
        }
        schedule_native_close(native);
    });

    // Watchdog: fail if shutdown takes > 6 s after the close, or if the
    // window never gets closed within 12 s (ctest TIMEOUT is 15 s).
    std::thread watchdog([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            long long closed = close_issued_ms.load();
            long long now = elapsed_ms();
            if (closed >= 0 && now - closed > 6000) {
                std::cerr << "[FAIL] app.run() did not return " << (now - closed)
                          << " ms after native window close (closed at " << closed << " ms)\n";
                std::_Exit(3);
            }
            if (closed < 0 && now > 12000) {
                std::cerr << "[FAIL] native close never issued within " << now << " ms\n";
                std::_Exit(4);
            }
        }
    });
    watchdog.detach();

    app.run();
    std::cout << "[PASS] app.run() returned " << (elapsed_ms() - close_issued_ms.load())
              << " ms after native window close\n";
    return 0;
#endif
}
