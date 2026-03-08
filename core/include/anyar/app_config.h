#pragma once

/// @file app_config.h
/// @brief Application and window configuration structures for LibAnyar.
///
/// These structs control how the application server and webview window
/// are initialized. Typically set in `main()` before calling `app.run()`.
///
/// ## Example
/// ```cpp
/// anyar::AppConfig config;
/// config.port = 3000;          // fixed port (0 = auto)
/// config.debug = true;         // enable devtools
/// config.dist_path = "./dist"; // frontend build output
///
/// anyar::WindowConfig win;
/// win.title = "My App";
/// win.width = 1200;
/// win.height = 800;
/// ```

#include <string>

namespace anyar {

/// @brief Configuration for the webview window.
///
/// Controls the window's appearance and behavior. When `debug` is `false`,
/// native-app-feel features are enabled: right-click is disabled, zoom/pinch
/// is blocked, and the viewport is locked.
struct WindowConfig {
    /// @brief Window title shown in the title bar.
    std::string title = "LibAnyar App";

    /// @brief Initial window width in pixels.
    int width = 800;

    /// @brief Initial window height in pixels.
    int height = 600;

    /// @brief Whether the window can be resized by the user.
    bool resizable = true;

    /// @brief Whether window decorations (title bar, borders) are shown.
    bool decorations = true;

    /// @brief Enable WebView DevTools (inspector).
    /// When `false`, right-click context menu and keyboard shortcuts
    /// for zoom (Ctrl±, Ctrl+0) are also disabled.
    bool debug = false;
};

/// @brief Extended options for creating a window.
///
/// Inherits all `WindowConfig` fields plus multi-window specific options
/// like parent/child relationships, modality, URL path, and positioning.
///
/// ## Example
/// ```cpp
/// anyar::WindowCreateOptions opts;
/// opts.label  = "settings";
/// opts.title  = "App Settings";
/// opts.parent = "main";     // child of main window
/// opts.modal  = true;       // blocks parent until closed
/// opts.url    = "/settings"; // frontend route
/// opts.width  = 500;
/// opts.height = 400;
/// ```
struct WindowCreateOptions : WindowConfig {
    /// @brief Unique window identifier (alphanumeric + `-/:_`).
    std::string label = "main";

    /// @brief Label of the parent window. Empty string = top-level window.
    std::string parent;

    /// @brief Whether this window blocks its parent (requires `parent`).
    bool modal = false;

    /// @brief URL path or full URL to load. Default `"/"` loads the app root.
    std::string url = "/";

    /// @brief Center the window on screen (or on parent if set).
    bool center = true;

    /// @brief Keep the window above all other windows.
    bool always_on_top = false;

    /// @brief Allow the user to close the window via the title-bar button.
    bool closable = true;

    /// @brief Allow the user to minimize the window.
    bool minimizable = true;
};

/// @brief Configuration for the LibAnyar application server.
///
/// Controls the HTTP/WebSocket server that serves the frontend and
/// handles IPC communication between the frontend and C++ backend.
struct AppConfig {
    /// @brief Server bind address.
    std::string host = "127.0.0.1";

    /// @brief Server port. `0` = automatically pick an available port.
    int port = 0;

    /// @brief Enable debug mode globally.
    /// Merged with `WindowConfig::debug` via OR — if either is `true`,
    /// DevTools and debug features are enabled.
    bool debug = false;

    /// @brief Path to the frontend build output directory.
    /// Served as static files by the HTTP server. Typically `"./dist"`.
    std::string dist_path;
};

} // namespace anyar
