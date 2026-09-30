#pragma once

// LibAnyar — private per-platform hooks used by platform-neutral sources.
//
// NOT a public header: never include from core/include/.  Implemented in
// platform_<os>.cpp and main_thread_<os>.cpp so app.cpp / window.cpp stay
// free of #ifdef blocks for these concerns.

#include <filesystem>

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
#endif

} // namespace anyar::platform
