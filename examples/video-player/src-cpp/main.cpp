// LibAnyar — Mini Video Player Example
//
// Demonstrates two parallel rendering paths for raw decoded video frames:
//
//   --mode=pinhole   (default on Linux + Windows) Native surface layered BELOW
//                              the transparent webview (Linux: GtkGLArea;
//                              Windows: DirectComposition + D3D11). HTML
//                              controls (timeline, panels) composite on top.
//   --mode=webgl     SharedBufferPool + buffer:ready event + JS WebGL
//                              renderer (zero-copy WebView2 shared buffers
//                              on Windows).
//
// If `Pinhole::is_native()` returns false (headless, sandbox, missing GL),
// the framework's built-in canvas-2D fallback transparently takes over.

#include <anyar/app.h>
#include <anyar/pinhole.h>
#include <anyar/window.h>
#include "video_plugin.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef __linux__
#include <anyar/main_thread.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#elif defined(_WIN32)
#include <windows.h>  // TerminateProcess (test:quit)
#endif
#include <memory>
#include <string>

#ifdef ANYAR_EMBED_FRONTEND
#include <anyar/embed.h>
#endif

using json = nlohmann::json;

namespace {

videoplayer::RenderMode parse_mode(int argc, char** argv) {
    using videoplayer::RenderMode;
#if defined(__linux__) || defined(_WIN32)
    constexpr RenderMode kDefault = RenderMode::Pinhole;
#else
    // Pinhole is still a stub here (no native overlay, no canvas fallback),
    // so frames would never reach the screen.
    constexpr RenderMode kDefault = RenderMode::WebGL;
#endif
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--mode=", 7) == 0) {
            std::string v(a + 7);
            if (v == "webgl")   return RenderMode::WebGL;
            if (v == "pinhole") return kDefault == RenderMode::Pinhole ? RenderMode::Pinhole
                                                                      : RenderMode::WebGL;
            std::cerr << "[video-player] Unknown --mode=" << v
                      << " (expected pinhole|webgl); using the default.\n";
            return kDefault;
        }
    }
    return kDefault;
}

#ifdef __linux__
GtkWidget* find_webview(GtkWidget* w) {
    if (WEBKIT_IS_WEB_VIEW(w)) return w;
    if (!GTK_IS_CONTAINER(w)) return nullptr;
    GtkWidget* found = nullptr;
    GList* kids = gtk_container_get_children(GTK_CONTAINER(w));
    for (GList* k = kids; k && !found; k = k->next) found = find_webview(GTK_WIDGET(k->data));
    g_list_free(kids);
    return found;
}

/// Inject a real left click (press + release) at CSS-ish pixel (x, y).
/// Unlike a JS-dispatched event this counts as a user gesture, so it can
/// start unmuted <audio> playback.
void inject_click(GtkWidget* webview, double x, double y) {
    GdkWindow* gw = gtk_widget_get_window(webview);
    if (!gw) return;
    GdkSeat* seat = gdk_display_get_default_seat(gdk_window_get_display(gw));
    for (GdkEventType t : {GDK_BUTTON_PRESS, GDK_BUTTON_RELEASE}) {
        GdkEvent* ev = gdk_event_new(t);
        ev->button.window = GDK_WINDOW(g_object_ref(gw));
        ev->button.send_event = TRUE;
        ev->button.time = GDK_CURRENT_TIME;
        ev->button.x = x;
        ev->button.y = y;
        ev->button.button = 1;
        gdk_event_set_device(ev, gdk_seat_get_pointer(seat));
        gtk_main_do_event(ev);
        gdk_event_free(ev);
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    const auto mode = parse_mode(argc, argv);

    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.debug = true;
    config.dist_path = "./dist";

    anyar::App app(config);

    auto plugin = std::make_shared<videoplayer::VideoPlugin>(mode);
    app.use(plugin);

    anyar::WindowConfig win;
    win.title = (mode == videoplayer::RenderMode::WebGL)
                    ? "LibAnyar — Video Player (webgl)"
                    : "LibAnyar — Video Player (pinhole)";
    win.width = 1100;
    win.height = 800;
    win.resizable = true;
    win.debug = true;

    app.create_window(win);

    // ── Test mode (VIDEO_PLAYER_TEST_SCRIPT=<file.js>) ──────────────────
    // Injects a driver script into the page and exposes test:* commands so
    // an automated run can exercise the real UI (see README "Automated
    // UI test").  Off unless the env var is set.
    std::string test_script;
    if (const char* ts = std::getenv("VIDEO_PLAYER_TEST_SCRIPT")) {
        std::ifstream f(ts);
        std::stringstream ss;
        ss << f.rdbuf();
        test_script = "window.__VIDEO_PLAYER_TEST__ = true;\n" + ss.str();
        app.command("test:log", [](const json& a) -> json {
            std::cout << "[test] " << a.value("msg", a.dump()) << std::endl;
            return nullptr;
        });
        app.command("test:quit", [](const json& a) -> json {
            std::cout << "[test] quit code=" << a.value("code", 0) << std::endl;
#ifdef _WIN32
            // _Exit() still runs DLL_PROCESS_DETACH, which can deadlock with
            // WebView2/FFmpeg worker threads — terminate outright.
            TerminateProcess(GetCurrentProcess(), static_cast<UINT>(a.value("code", 0)));
#endif
            std::_Exit(a.value("code", 0));
        });
#ifdef __linux__
        app.command("test:click", [](const json& a) -> json {
            const double x = a.value("x", 0.0), y = a.value("y", 0.0);
            anyar::post_to_main_thread([x, y] {
                for (GList* l = gtk_window_list_toplevels(); l; l = l->next) {
                    if (GtkWidget* wv = find_webview(GTK_WIDGET(l->data))) { inject_click(wv, x, y); break; }
                }
            });
            return nullptr;
        });
#endif
    }

    // on_window_ready fires on the main thread after the Window is created
    // but before the GTK event loop starts — the correct place for
    // create_pinhole().
    app.on_window_ready([&](anyar::Window& window) {
        if (!test_script.empty()) {
            window.init(test_script);
            std::cout << "[test] driver script injected (" << test_script.size() << " bytes)" << std::endl;
        }
        if (mode != videoplayer::RenderMode::Pinhole) return;
        {
            anyar::PinholeOptions pin_opts;
            pin_opts.format     = anyar::pixel_format::yuv420;  // hint; redetected per-frame
            pin_opts.continuous = false;                        // request_redraw on each frame
            auto pin = window.create_pinhole("video", pin_opts);

            if (!pin->is_native()) {
                std::cerr << "[video-player] WARNING: Pinhole native overlay unavailable. "
                          << "Falling back to built-in canvas-2D path inside the framework. "
                          << "Performance will be lower than --mode=webgl.\n";
            } else {
                std::cout << "[video-player] Pinhole native overlay active (id="
                          << pin->id() << ").\n";
            }

            plugin->set_pinhole(pin);
        }
    });

#ifdef ANYAR_EMBED_FRONTEND
    app.set_frontend_resolver(anyar::make_embedded_resolver());
#endif

    std::cout << "[video-player] Starting — mode="
              << (mode == videoplayer::RenderMode::WebGL ? "webgl" : "pinhole")
              << std::endl;

    return app.run();
}
