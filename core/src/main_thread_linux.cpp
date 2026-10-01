// LibAnyar — Main-Thread Dispatch (Linux / GTK implementation)
//
// Implements post_to_main_thread() using g_idle_add() to schedule work
// on the GTK main loop.

#include <anyar/main_thread.h>
#include "platform.h"

#include <gtk/gtk.h>

namespace anyar {

namespace {

/// Plain C callback compatible with GSourceFunc.
gboolean idle_trampoline(gpointer ptr) {
    auto* fn = static_cast<std::function<void()>*>(ptr);
    (*fn)();
    delete fn;
    return G_SOURCE_REMOVE;  // run once
}

} // anonymous namespace

void post_to_main_thread(std::function<void()> fn) {
    // Heap-allocate so it survives until the idle callback fires
    auto* thunk = new std::function<void()>(std::move(fn));
    g_idle_add(idle_trampoline, thunk);
}

namespace platform {

// GTK's default main context is process-wide; nothing to bind.
void attach_main_thread() {}

void drain_main_thread(int max_iterations) {
    // Bounded: under xvfb WebKitGTK may generate events indefinitely.
    for (int i = 0; i < max_iterations && g_main_context_pending(nullptr); ++i) {
        g_main_context_iteration(nullptr, FALSE);
    }
}

} // namespace platform

} // namespace anyar
