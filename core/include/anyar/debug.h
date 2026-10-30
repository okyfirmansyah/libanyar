#pragma once

/// @file debug.h
/// @brief Developer debugging and UI-automation aids (see docs/debugging.md).
///
/// Three tools, all off by default and all switchable by environment
/// variable, so a shipped binary can be debugged without rebuilding:
///
/// | Tool | AppConfig field | Environment |
/// |---|---|---|
/// | Console forwarding: page `console.*`, uncaught errors and unhandled rejections → process stdout/stderr | `debug_tools.console` | `ANYAR_CONSOLE=1` |
/// | Automation commands `debug:*`: screenshot of the real webview, synthetic OS-level input, exit with a code | `debug_tools.automation` | `ANYAR_DEBUG_TOOLS=1` |
/// | Test driver: a JS file injected into the main window, with an `anyarTest` helper | `debug_tools.test_script` | `ANYAR_TEST_SCRIPT=path` (+ `ANYAR_TEST_TIMEOUT=seconds`) |
///
/// Platform status: Linux (WebKitGTK) implements capture and input.
/// Windows (WebView2) and macOS (WKWebView) report "not supported" until
/// their platform hooks land; console forwarding and test scripts work on
/// every platform because they only use the JS bridge.
///
/// @code
/// anyar::AppConfig cfg;
/// cfg.debug_tools.console = true;          // or run with ANYAR_CONSOLE=1
/// anyar::App app(cfg);
/// @endcode

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace anyar {

/// @brief Debugging aids, configured through `AppConfig::debug_tools`.
struct DebugToolsConfig {
    /// Mirror the page's console output and uncaught errors to the process
    /// (warn/error → stderr, others → stdout), prefixed `[js:<window>]`.
    bool console = false;

    /// Register the `debug:*` automation commands (screenshot, input, exit,
    /// info).  They let page code (and test scripts) drive the real webview,
    /// so leave this off in release builds.
    bool automation = false;

    /// JavaScript file injected into the main window before page scripts and
    /// run after `load` (top-level `await` allowed).  Implies `console` and
    /// `automation`.  Empty = none.
    std::string test_script;

    /// Seconds before a running test script is aborted with exit code 124.
    /// 0 = no limit.
    int test_timeout_s = 0;
};

/// @brief A PNG capture of a webview's visible area.
struct WebviewSnapshot {
    int width = 0;              ///< pixels (device pixels on HiDPI)
    int height = 0;             ///< pixels
    std::vector<uint8_t> png;   ///< encoded PNG
};

/// @brief Completion of Window::capture(): the snapshot, or an error message.
using SnapshotCallback = std::function<void(WebviewSnapshot snapshot, std::string error)>;

/// @brief A synthetic input event delivered through the OS/toolkit event path,
/// so the page sees it as real user input (it counts as a user gesture).
struct InputEvent {
    /// Kind of event.  `Click` = Down + Up; `Text` = one key press per character.
    enum class Type { Move, Down, Up, Click, Scroll, Key, Text };
    Type type = Type::Click;
    double x = 0;               ///< CSS px from the webview's top-left (pointer events)
    double y = 0;               ///< CSS px
    int button = 1;             ///< 1 = left, 2 = middle, 3 = right
    double dx = 0;              ///< Scroll delta (smooth units ≈ wheel notches)
    double dy = 0;              ///< Scroll delta; positive = down
    std::string key;            ///< Key: DOM-style name ("Enter", "Escape", "ArrowDown", "a")
    std::string text;           ///< Text: characters to type
    bool shift = false, ctrl = false, alt = false, meta = false;   ///< modifiers held
};

/// @brief Parse an InputEvent::Type from its lower-case name ("click", "move", …).
/// @param name  Type name.
/// @param out   Receives the type.
/// @returns     false if @p name is unknown.
bool parse_input_type(const std::string& name, InputEvent::Type& out);

} // namespace anyar
