#pragma once

// LibAnyar — private per-platform hooks used by platform-neutral sources.
//
// NOT a public header: never include from core/include/.  Implemented in
// platform_<os>.cpp and main_thread_<os>.cpp so app.cpp / window.cpp stay
// free of #ifdef blocks for these concerns.

#include <filesystem>
#include <string>

namespace anyar { class SharedBuffer; }

namespace anyar::platform {

/// One-time process setup, called from App::App() before any UI code runs
/// (Linux: strip snap GTK env vars).
void init_process();

/// Absolute path of the running executable, or empty if unknown.
std::filesystem::path executable_path();

/// Bind main-thread dispatch to the calling thread.  Called by App::run() on
/// the UI thread before any window is created.  Idempotent.
void attach_main_thread();

/// Process at most @p max_iterations pending UI events without blocking, so
/// run_on_main_thread() work queued by fibers completes before shutdown.
void drain_main_thread(int max_iterations);

/// Whether the webview serves `anyar-shm://` natively.  When false the JS
/// bridge fetches buffers over HTTP (`/__anyar__/buffer/<name>`) instead.
bool has_shm_uri_scheme();

/// Whether the webview can map SharedBuffer memory directly via
/// `buffer:attach` (Windows / WebView2 shared buffers).  Injected into pages
/// as `window.__LIBANYAR_SHARED_BUFFERS__`.
bool has_webview_shared_buffers();

#ifdef _WIN32
/// Ask the UI thread's message loop to exit.  Thread-safe (webview's own
/// terminate() is a bare PostQuitMessage, which only targets the CALLING
/// thread).  The request is sticky until clear_quit_request(): webview's
/// nested loops (e.g. inside webview_destroy) swallow WM_QUIT, so callers
/// that pump such loops re-post it with repost_quit_if_requested().
void request_quit();

/// Re-post WM_QUIT if request_quit() was called.  UI thread only.
void repost_quit_if_requested();

/// Forget a pending quit request (after the main loop has returned).
void clear_quit_request();

/// True on the thread bound by attach_main_thread().
bool is_main_thread();

/// Record a window's WebView2 controller (ICoreWebView2Controller*) so
/// SharedBuffers can be allocated as WebView2 shared buffers.  The first
/// environment that supports them wins.  UI thread only.
void note_webview_controller(void* controller);

/// Post @p buf's WebView2 shared buffer to the page hosted by
/// @p controller (read-only), tagged with @p additional_json.  Returns false
/// if @p buf is not WebView2-backed or the runtime refuses.  UI thread only.
bool post_shared_buffer(void* controller, SharedBuffer& buf,
                        const std::string& additional_json);
#endif

} // namespace anyar::platform
