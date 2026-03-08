#pragma once

// LibAnyar — Main-Thread Dispatch Utility (Platform-Neutral)
//
// UI toolkits (GTK, Cocoa, Win32) are NOT thread-safe; all UI calls must
// happen on the thread running the platform event loop.  This header provides
// run_on_main_thread(fn) which schedules a callable on the UI main loop,
// blocks the calling fiber until it completes, and returns the result
// (or rethrows any exception).
//
// Platform implementations:
//   Linux   — g_idle_add()          (main_thread_linux.cpp)
//   Windows — PostMessage()         (main_thread_win32.cpp)   [Phase 7]
//   macOS   — dispatch_async(main)  (main_thread_macos.mm)    [Phase 7]

#include <functional>
#include <memory>
#include <type_traits>

#include <boost/fiber/future.hpp>

namespace anyar {

/// Schedule a type-erased void() callable on the platform main thread.
/// Implemented per-platform in main_thread_<platform>.cpp.
void post_to_main_thread(std::function<void()> fn);

/// Execute @p fn on the UI main thread and block the current fiber until it
/// completes.  Returns whatever @p fn returns.  Re-throws if @p fn throws.
///
/// IMPORTANT: Call this only when a platform main loop is running (i.e. after
/// webview_run() / gtk_main() has started on the main thread).
template<typename F>
auto run_on_main_thread(F&& fn) -> decltype(fn()) {
    using R = decltype(fn());

    auto promise = std::make_shared<boost::fibers::promise<R>>();
    auto future  = promise->get_future();

    // Wrap the user callable + promise into a type-erased std::function<void()>
    post_to_main_thread(
        [p = promise, f = std::forward<F>(fn)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    f();
                    p->set_value();
                } else {
                    p->set_value(f());
                }
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        });

    return future.get();
}

} // namespace anyar
