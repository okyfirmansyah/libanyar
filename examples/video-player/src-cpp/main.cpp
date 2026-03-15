// LibAnyar — Mini Video Player Example
// Demonstrates multimedia analysis via FFmpeg + polished Svelte UI

#include <anyar/app.h>
#include "video_plugin.h"
#include <iostream>

#ifdef ANYAR_EMBED_FRONTEND
#include <anyar/embed.h>
#endif

int main() {
    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.debug = true;
    config.dist_path = "./dist";

    anyar::App app(config);

    // Register the video plugin (FFmpeg-based analysis + HTTP streaming)
    app.use(std::make_shared<videoplayer::VideoPlugin>());

    // ── Create the application window ───────────────────────────────────────

    anyar::WindowConfig win;
    win.title = "LibAnyar — Video Player";
    win.width = 1100;
    win.height = 800;
    win.resizable = true;
    win.debug = true;

    app.create_window(win);

#ifdef ANYAR_EMBED_FRONTEND
    app.set_frontend_resolver(anyar::make_embedded_resolver());
#endif

    std::cout << "[video-player] Starting LibAnyar Video Player..." << std::endl;

    return app.run();
}
