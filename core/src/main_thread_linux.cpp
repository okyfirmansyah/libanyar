// LibAnyar — Main-Thread Dispatch (Linux / GTK implementation)
//
// Implements post_to_main_thread() using g_idle_add() to schedule work
// on the GTK main loop.

#include <anyar/main_thread.h>

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

} // namespace anyar
