// LibAnyar — WiFi Channel Analyzer Example
// Demonstrates native nl80211 WiFi scanning + real-time SharedBuffer WebGL spectrum

#include <anyar/app.h>
#include "wifi_plugin.h"
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

    // Register the WiFi analyzer plugin (nl80211-based scanning)
    app.use(std::make_shared<wifianalyzer::WifiPlugin>());

    // ── Create the application window ───────────────────────────────────────

    anyar::WindowConfig win;
    win.title = "LibAnyar — WiFi Channel Analyzer";
    win.width = 960;
    win.height = 720;
    win.resizable = true;
    win.debug = true;

    app.create_window(win);

#ifdef ANYAR_EMBED_FRONTEND
    app.set_frontend_resolver(anyar::make_embedded_resolver());
#endif

    std::cout << "[wifi-analyzer] Starting LibAnyar WiFi Channel Analyzer..."
              << std::endl;

    return app.run();
}
