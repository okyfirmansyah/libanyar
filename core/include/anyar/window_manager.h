#pragma once

/// @file window_manager.h
/// @brief Multi-window manager for LibAnyar.
///
/// Owns all open windows and provides label-based lookup, creation,
/// and lifecycle management. Typically one WindowManager per App.

#include <anyar/app_config.h>
#include <anyar/window.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace anyar {

class WindowManager {
public:
    WindowManager() = default;
    ~WindowManager() = default;

    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    // ── Creation ────────────────────────────────────────────────────────

    /// Create and register a new window.
    /// Must be called on the main thread (GTK requirement).
    /// @param opts  Window creation options (label, parent, modal, etc.)
    /// @param port  HTTP server port for the webview to connect to
    /// @returns     The window's label
    /// @throws std::runtime_error if label already exists
    std::string create(const WindowCreateOptions& opts, int port);

    // ── Lookup ──────────────────────────────────────────────────────────

    /// Get a window by label. Returns nullptr if not found.
    Window* get(const std::string& label);

    /// Shortcut for `get("main")`.
    Window* main_window() { return get("main"); }

    /// List all open window labels.
    std::vector<std::string> labels() const;

    /// Number of open windows.
    size_t count() const;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /// Close and remove a specific window. Safe to call from any thread
    /// (dispatches destruction to main thread if needed).
    void close(const std::string& label);

    /// Close all windows.
    void close_all();

    /// Remove a window from the map without destroying it (used when
    /// the native window was already destroyed by the platform).
    void remove(const std::string& label);

    // ── Callbacks ───────────────────────────────────────────────────────

    /// Called after any window is closed. Receives the closed window's label.
    using WindowClosedCallback = std::function<void(const std::string& label)>;
    void set_on_window_closed(WindowClosedCallback cb);

    /// Called after a window is created. Receives the new window.
    using WindowCreatedCallback = std::function<void(Window& window, const WindowCreateOptions& opts)>;
    void set_on_window_created(WindowCreatedCallback cb);

private:
    std::map<std::string, std::shared_ptr<Window>> windows_;
    mutable std::mutex mutex_;
    WindowClosedCallback on_closed_;
    WindowCreatedCallback on_created_;
};

} // namespace anyar
