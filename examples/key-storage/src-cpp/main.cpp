// LibAnyar — Secure Key Storage Example
// Encrypted password manager with SQLite backend + AES-256-GCM encryption

#include <anyar/app.h>
#include "keystore_plugin.h"
#include <iostream>

int main() {
    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.debug = false;
    config.dist_path = "./dist";

    anyar::App app(config);

    // Register the keystore plugin (SQLite + AES-256-GCM encryption)
    app.use(std::make_shared<keystore::KeystorePlugin>());

    // ── Create the application window ───────────────────────────────────────

    anyar::WindowConfig win;
    win.title = "LibAnyar — Key Storage";
    win.width = 1050;
    win.height = 700;
    win.resizable = true;
    win.debug = false;

    app.create_window(win);

    std::cout << "[key-storage] Starting LibAnyar Key Storage..." << std::endl;

    return app.run();
}
