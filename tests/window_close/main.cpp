#include <anyar/app.h>
#include <anyar/window.h>

#ifdef __linux__
#include <gtk/gtk.h>
#endif

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main() {
#ifndef __linux__
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
        auto* native = static_cast<GtkWidget*>(window.native_handle());
        if (!native) {
            std::cerr << "[FAIL] native_handle() returned null\n";
            std::_Exit(2);
        }

        g_timeout_add(200, +[](gpointer data) -> gboolean {
            gtk_window_close(GTK_WINDOW(data));
            return G_SOURCE_REMOVE;
        }, native);
    });

    std::thread watchdog([]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cerr << "[FAIL] app.run() did not return after native window close\n";
        std::_Exit(3);
    });
    watchdog.detach();

    app.run();
    std::cout << "[PASS] app.run() returned after native window close\n";
    return 0;
#endif
}