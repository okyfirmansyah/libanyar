// LibAnyar — Anatomy Atlas Example
// Whole-body 3D anatomy explorer: a C++ plugin decodes BodyParts3D meshes
// and streams them, system by system and level by level, to three.js through
// SharedBuffers.  For education and demonstration only — not a medical device.

#include <anyar/app.h>
#include "atlas_plugin.h"

#include <cstdlib>
#include <iostream>

#ifdef ANYAR_EMBED_FRONTEND
#include <anyar/embed.h>
#endif

int main() {
    anyar::AppConfig config;
    config.host = "127.0.0.1";
    // ATLAS_PORT pins the port (handy for opening the UI in a browser).
    const char* port = std::getenv("ATLAS_PORT");
    config.port = port ? std::atoi(port) : 0;
    config.debug = std::getenv("ATLAS_DEBUG") != nullptr;
    config.dist_path = "./dist";

    anyar::App app(config);
    app.use(std::make_shared<atlas::AtlasPlugin>());

    anyar::WindowConfig win;
    win.title = "Anatomy Atlas — LibAnyar";
    win.width = 1440;
    win.height = 900;
    win.resizable = true;
    win.debug = config.debug;
    app.create_window(win);

#ifdef ANYAR_EMBED_FRONTEND
    app.set_frontend_resolver(anyar::make_embedded_resolver());
#endif

    std::cout << "[anatomy-atlas] Starting..." << std::endl;
    return app.run();
}
