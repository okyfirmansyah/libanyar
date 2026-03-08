#pragma once

// LibAnyar — Window Management
// Wraps webview/webview for native window creation

#include <anyar/app_config.h>
#include <anyar/types.h>

#include <functional>
#include <memory>
#include <string>

namespace anyar {

class Window {
public:
    Window(const WindowConfig& config, int server_port);
    ~Window();

    /// Run the window event loop (blocks, must be called on main thread)
    void run();

    /// Close the window
    void terminate();

    /// Evaluate JavaScript in the webview
    void eval(const std::string& js);

    /// Navigate to a URL
    void navigate(const std::string& url);

    /// Dispatch a function to the webview thread (thread-safe)
    void dispatch(std::function<void()> fn);

    /// Set window title
    void set_title(const std::string& title);

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
