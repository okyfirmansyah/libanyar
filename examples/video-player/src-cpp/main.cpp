// LibAnyar — Mini Video Player Example
//
// Demonstrates two parallel rendering paths for raw decoded video frames:
//
//   --mode=pinhole   (default) Native GtkGLArea layered BELOW a transparent
//                              WebKitWebView. HTML controls (timeline, panels)
//                              composite freely on top of the GL surface.
//   --mode=webgl               Legacy SharedBufferPool + buffer:ready event
//                              + JS WebGL renderer.
//
// If `Pinhole::is_native()` returns false (headless, sandbox, missing GL),
// the framework's built-in canvas-2D fallback transparently takes over.

#include <anyar/app.h>
#include <anyar/pinhole.h>
#include <anyar/window.h>
#include "video_plugin.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#ifdef ANYAR_EMBED_FRONTEND
#include <anyar/embed.h>
#endif

namespace {

videoplayer::RenderMode parse_mode(int argc, char** argv) {
    using videoplayer::RenderMode;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--mode=", 7) == 0) {
            std::string v(a + 7);
            if (v == "webgl")   return RenderMode::WebGL;
            if (v == "pinhole") return RenderMode::Pinhole;
            std::cerr << "[video-player] Unknown --mode=" << v
                      << " (expected pinhole|webgl); using pinhole.\n";
            return RenderMode::Pinhole;
        }
    }
    return RenderMode::Pinhole;
}

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

    if (mode == videoplayer::RenderMode::Pinhole) {
        // on_window_ready fires on the main thread after the Window is created
        // but before the GTK event loop starts — the correct place for
        // create_pinhole().
        app.on_window_ready([&](anyar::Window& window) {
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
        });
    }

#ifdef ANYAR_EMBED_FRONTEND
    app.set_frontend_resolver(anyar::make_embedded_resolver());
#endif

    std::cout << "[video-player] Starting — mode="
              << (mode == videoplayer::RenderMode::WebGL ? "webgl" : "pinhole")
              << std::endl;

    return app.run();
}
