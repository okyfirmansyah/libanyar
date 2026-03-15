// LibAnyar — Hello World Example
// Demonstrates basic window, IPC commands, and events

#include <anyar/app.h>
#include <iostream>

#ifdef ANYAR_EMBED_FRONTEND
#include <anyar/embed.h>
#endif

int main() {
    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.port = 0;    // auto-select ephemeral port
    config.debug = true; // enable DevTools in webview
    config.dist_path = "./dist";

    anyar::App app(config);

    // ── Register commands ───────────────────────────────────────────────────

    // Simple greeting command
    app.command("greet", [](const anyar::json& args) -> anyar::json {
        std::string name = args.value("name", "World");
        return {
            {"message", "Hello, " + name + "! 🎉"},
            {"from", "LibAnyar C++ Backend"}
        };
    });

    // Command that returns system info
    app.command("get_info", [](const anyar::json& args) -> anyar::json {
        return {
            {"framework", "LibAnyar"},
            {"version", "0.1.0"},
            {"backend", "LibAsyik + webview/webview"},
            {"language", "C++17"}
        };
    });

    // Counter command — demonstrates state via static variable
    app.command("increment", [](const anyar::json& args) -> anyar::json {
        static int counter = 0;
        counter++;
        return {{"count", counter}};
    });

    // ── Create the application window ───────────────────────────────────────

    anyar::WindowConfig win;
    win.title = "LibAnyar — Hello World";
    win.width = 900;
    win.height = 650;
    win.resizable = true;
    win.debug = true;

    app.create_window(win);

#ifdef ANYAR_EMBED_FRONTEND
    app.set_frontend_resolver(anyar::make_embedded_resolver());
    std::cout << "[hello-world] Frontend embedded in binary" << std::endl;
#endif

    std::cout << "[hello-world] Starting LibAnyar app..." << std::endl;

    return app.run();
}
