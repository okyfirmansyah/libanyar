#pragma once

/// @file window.h
/// @brief Platform-neutral window API for LibAnyar.
///
/// Each Window wraps a native platform window + webview pair.
/// On Linux: GtkWindow + WebKitWebView.
/// On Windows (Phase 7): HWND + WebView2.
/// On macOS (Phase 7): NSWindow + WKWebView.
///
/// All public methods use only platform-neutral types.
/// Platform-specific code lives in the pimpl (`Window::Impl`).

#include <anyar/app_config.h>
#include <anyar/types.h>

#include <functional>
#include <memory>
#include <string>

namespace anyar {

class Window {
public:
    /// Construct from legacy WindowConfig (single-window backward compat)
    Window(const WindowConfig& config, int server_port);

    /// Construct from WindowCreateOptions (multi-window)
    Window(const WindowCreateOptions& opts, int server_port);

    ~Window();

    // ── Identification ──────────────────────────────────────────────────

    /// Get this window's unique label
    const std::string& label() const;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /// Run the window event loop (blocks, must be called on main thread).
    /// Typically only the main window calls this — child windows are
    /// serviced by the same platform main loop.
    void run();

    /// Signal the event loop to quit (safe to call from any thread).
    void terminate();

    /// Destroy the native window. After this, the Window object is invalid.
    /// Must be called on the main thread.
    void destroy();

    /// Returns true if the native window has been destroyed.
    bool is_destroyed() const;

    /// Show the window on screen (set size + realize).
    /// Call this AFTER all setup (parent, modal, IPC, etc.) is complete.
    /// For main window this is called automatically; for child windows
    /// it must be called explicitly from WindowManager::create().
    void show();

    // ── Webview Operations ──────────────────────────────────────────────

    /// Evaluate JavaScript in the webview
    void eval(const std::string& js);

    /// Navigate to a URL
    void navigate(const std::string& url);

    /// Dispatch a function to the webview thread (thread-safe)
    void dispatch(std::function<void()> fn);

    /// Set window title
    void set_title(const std::string& title);

    /// Set window size
    void set_size(int width, int height);

    // ── Native Handle ───────────────────────────────────────────────────

    /// Returns the opaque native window handle.
    /// On Linux: GtkWindow*. On Windows: HWND. On macOS: NSWindow*.
    /// Callers must cast per-platform. Returns nullptr if window is destroyed.
    void* native_handle() const;

    // ── Parent / Child / Modal ──────────────────────────────────────────

    /// Establish parent/child (transient) relationship.
    /// Child stays on top of parent and minimizes with it.
    void set_parent(Window& parent);

    /// Set or clear modal mode (blocks interaction with parent).
    /// Requires set_parent() to have been called first.
    void set_modal(bool modal);

    /// Enable or disable user input on this window.
    /// Can be used for pseudo-modal patterns (disable parent manually).
    void set_enabled(bool enabled);

    // ── Window Appearance & Behavior ────────────────────────────────────

    /// Keep this window above all other windows.
    void set_always_on_top(bool on_top);

    /// Allow or prevent user closing via the title-bar button.
    void set_closable(bool closable);

    /// Move the window to an absolute screen position.
    void set_position(int x, int y);

    /// Center the window on its parent (or the screen if top-level).
    void center_on_parent();

    /// Bring this window to the front and give it focus.
    void focus();

    // ── Close Event Handlers ────────────────────────────────────────────

    /// Called when the window is destroyed (after close). Non-interceptable.
    using CloseHandler = std::function<void()>;
    void set_on_close(CloseHandler handler);

    /// Called when the user requests close (e.g. title-bar X button).
    /// Return true to allow the close, false to prevent it.
    /// If not set, close is always allowed (unless closable == false).
    using CloseRequestedHandler = std::function<bool()>;
    void set_on_close_requested(CloseRequestedHandler handler);

    // ── Native IPC (webview_bind / webview_return) ──────────────────────

    /// Bind a native C++ function as a global JS function `window.<name>(...)`,
    /// returning a Promise on the JS side.  The callback receives the
    /// sequence-id and a JSON array of the JS call arguments.
    /// Call return_result() with the same sequence-id to resolve the Promise.
    using BindCallback = std::function<void(const std::string& seq,
                                            const std::string& req)>;
    void bind(const std::string& name, BindCallback callback);

    /// Resolve (status==0) or reject (status!=0) a pending JS Promise
    /// created by a previous bind() call.  Thread-safe.
    void return_result(const std::string& seq, int status,
                       const std::string& result);

    /// Inject JavaScript that will be executed on every page load,
    /// before window.onload.  Can be called multiple times.
    void init(const std::string& js);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace anyar
