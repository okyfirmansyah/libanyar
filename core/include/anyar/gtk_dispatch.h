#pragma once

// DEPRECATED — use <anyar/main_thread.h> instead.
// This header is kept for backward compatibility only.

#include <anyar/main_thread.h>

namespace anyar {

/// @deprecated Use run_on_main_thread() instead.
template<typename F>
auto run_on_gtk_main(F&& fn) -> decltype(fn()) {
    return run_on_main_thread(std::forward<F>(fn));
}

} // namespace anyar
