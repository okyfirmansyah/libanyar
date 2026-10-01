// LibAnyar — Main-Thread Dispatch (Windows / Win32 implementation)
//
// post_to_main_thread() posts to a message-only window owned by the UI
// thread.  Any message loop on that thread dispatches it — webview_run(),
// webview's internal nested loops, and modal loops (file dialogs, message
// boxes) alike — mirroring g_idle_add() semantics on GTK.

#include <anyar/main_thread.h>
#include "platform.h"
#include "win32_util.h"

#include <atomic>
#include <exception>
#include <iostream>
#include <mutex>
#include <vector>

namespace anyar {

namespace {

constexpr UINT kRunFunctionMsg = WM_APP + 0x41;  // 'A'nyar
constexpr wchar_t kClassName[] = L"anyar_main_thread_dispatch";

using Thunk = std::function<void()>;

std::mutex g_mutex;
HWND g_hwnd = nullptr;          // guarded by g_mutex
DWORD g_thread_id = 0;          // guarded by g_mutex
std::vector<Thunk*> g_pending;  // posts that arrived before attach
std::atomic<bool> g_quit_requested{false};

void run_thunk(Thunk* fn) {
    // Never let an exception unwind through a Win32 window procedure.
    try {
        (*fn)();
    } catch (const std::exception& e) {
        std::cerr << "[LibAnyar] main-thread task threw: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[LibAnyar] main-thread task threw an unknown exception" << std::endl;
    }
    delete fn;
}

LRESULT CALLBACK dispatch_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kRunFunctionMsg) {
        run_thunk(reinterpret_cast<Thunk*>(lp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // anonymous namespace

void post_to_main_thread(std::function<void()> fn) {
    auto* thunk = new Thunk(std::move(fn));
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_hwnd) {
        g_pending.push_back(thunk);
        return;
    }
    if (!PostMessageW(g_hwnd, kRunFunctionMsg, 0, reinterpret_cast<LPARAM>(thunk))) {
        std::cerr << "[LibAnyar] post_to_main_thread failed: "
                  << win32::error_message(GetLastError()) << std::endl;
        delete thunk;
    }
}

namespace platform {

bool is_main_thread() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_hwnd && g_thread_id == GetCurrentThreadId();
}

void attach_main_thread() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_hwnd) return;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = dispatch_wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // fails harmlessly if already registered

    g_hwnd = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, inst, nullptr);
    if (!g_hwnd) {
        std::cerr << "[LibAnyar] cannot create main-thread dispatch window: "
                  << win32::error_message(GetLastError()) << std::endl;
        return;
    }
    g_thread_id = GetCurrentThreadId();

    for (auto* thunk : g_pending) {
        PostMessageW(g_hwnd, kRunFunctionMsg, 0, reinterpret_cast<LPARAM>(thunk));
    }
    g_pending.clear();
}

void drain_main_thread(int max_iterations) {
    MSG msg;
    for (int i = 0; i < max_iterations &&
                    PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));  // keep it for the caller
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void request_quit() {
    g_quit_requested.store(true);
    if (is_main_thread()) {
        PostQuitMessage(0);
    } else {
        post_to_main_thread([] { PostQuitMessage(0); });
    }
}

void repost_quit_if_requested() {
    if (g_quit_requested.load()) {
        PostQuitMessage(0);
    }
}

void clear_quit_request() {
    g_quit_requested.store(false);
}

} // namespace platform

} // namespace anyar
