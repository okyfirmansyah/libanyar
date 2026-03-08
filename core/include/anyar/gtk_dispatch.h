#pragma once

// LibAnyar — GTK Main-Thread Dispatch Utility
// GTK is NOT thread-safe; all GTK calls must happen on the thread running
// gtk_main().  This header provides run_on_gtk_main(fn) which schedules a
// callable on the GTK main loop via g_idle_add(), then blocks until it
// finishes and returns the result (or rethrows any exception).

#include <functional>
#include <memory>
#include <type_traits>

#include <boost/fiber/future.hpp>
#include <gtk/gtk.h>

namespace anyar {
namespace detail {

/// Plain C callback compatible with GSourceFunc (no captures → decays to fn ptr)
inline gboolean gtk_idle_trampoline(gpointer ptr) {
    auto* fn = static_cast<std::function<void()>*>(ptr);
    (*fn)();
    delete fn;
    return G_SOURCE_REMOVE;  // run once
}

} // namespace detail

/// Execute @p fn on the GTK main thread and block until it completes.
/// Returns whatever @p fn returns.  Re-throws if @p fn throws.
///
/// IMPORTANT: Call this only when a GTK main loop is running (i.e. after
/// webview_run() / gtk_main() has started on the main thread).
template<typename F>
auto run_on_gtk_main(F&& fn) -> decltype(fn()) {
    using R = decltype(fn());

    auto promise = std::make_shared<boost::fibers::promise<R>>();
    auto future  = promise->get_future();

    // Wrap the user callable + promise into a type-erased std::function<void()>
    auto* thunk = new std::function<void()>(
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

    g_idle_add(detail::gtk_idle_trampoline, thunk);

    return future.get();
}

} // namespace anyar
