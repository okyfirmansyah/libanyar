// LibAnyar — SharedBuffer memory (Windows)
//
// Preferred: a WebView2 shared buffer (ICoreWebView2Environment12::
// CreateSharedBuffer).  C++ writes through data(); the page receives the SAME
// memory as an ArrayBuffer via PostSharedBufferToScript (`buffer:attach`) —
// zero copies, no IPC for the payload.
//
// Fallback (no window yet, runtime too old, UI loop not pumping): an
// anonymous pagefile-backed file mapping, which the JS bridge fetches over
// HTTP GET /__anyar__/buffer/<name>.
//
// WebView2 objects are STA: they are created, posted and released on the UI
// thread only.  Buffers are usually created from fibers, so creation hops to
// the UI thread (bounded wait).  The memory pointer itself is usable from any
// thread.

#include <anyar/shared_buffer.h>
#include <anyar/main_thread.h>
#include "platform.h"
#include "win32_util.h"

#include <objbase.h>  // COM `interface` keyword (WIN32_LEAN_AND_MEAN omits it)
#include <WebView2.h>

#include <boost/fiber/future.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace anyar {

namespace {

using win32::ComPtr;

std::mutex g_env_mutex;
ICoreWebView2Environment12* g_env = nullptr;  // AddRef'd; used on the UI thread

struct NativeBuffer {
    ICoreWebView2SharedBuffer* buffer = nullptr;  // owned reference
    BYTE* data = nullptr;
};

bool have_environment() {
    std::lock_guard<std::mutex> lock(g_env_mutex);
    return g_env != nullptr;
}

/// UI thread: allocate a WebView2 shared buffer, or {} on failure.
NativeBuffer create_on_ui(size_t size) {
    ComPtr<ICoreWebView2Environment12> env;
    {
        std::lock_guard<std::mutex> lock(g_env_mutex);
        if (!g_env) return {};
        g_env->AddRef();
        *env.put() = g_env;
    }
    ComPtr<ICoreWebView2SharedBuffer> buf;
    HRESULT hr = env->CreateSharedBuffer(static_cast<UINT64>(size), buf.put());
    if (FAILED(hr) || !buf) {
        std::cerr << "[LibAnyar] CreateSharedBuffer(" << size << ") failed: "
                  << win32::error_message(static_cast<DWORD>(hr))
                  << " — using a file mapping (HTTP transport)" << std::endl;
        return {};
    }
    BYTE* data = nullptr;
    if (FAILED(buf->get_Buffer(&data)) || !data) return {};
    return {buf.detach(), data};
}

void release_on_ui(ICoreWebView2SharedBuffer* buf) {
    if (!buf) return;
    if (platform::is_main_thread()) {
        buf->Release();
    } else {
        // If the UI loop never runs again (shutdown), the reference leaks
        // with the process — harmless.
        post_to_main_thread([buf] { buf->Release(); });
    }
}

/// Any thread: WebView2 shared buffer, or {} if unavailable right now.
NativeBuffer create_webview_buffer(size_t size) {
    if (!have_environment()) return {};  // no window yet
    if (platform::is_main_thread()) return create_on_ui(size);

    // Hop to the UI thread.  boost::fibers futures suspend only the calling
    // fiber (or block a plain thread).  Bounded: the UI thread may not be
    // pumping (startup, shutdown, joined from a plugin's shutdown()).
    struct State {
        std::mutex mu;
        bool abandoned = false;
        boost::fibers::promise<NativeBuffer> promise;
    };
    auto st = std::make_shared<State>();
    auto fut = st->promise.get_future();
    post_to_main_thread([st, size] {
        NativeBuffer nb = create_on_ui(size);
        std::lock_guard<std::mutex> lock(st->mu);
        if (st->abandoned) {
            if (nb.buffer) nb.buffer->Release();
            return;
        }
        st->promise.set_value(nb);
    });
    if (fut.wait_for(std::chrono::seconds(2)) != boost::fibers::future_status::ready) {
        std::lock_guard<std::mutex> lock(st->mu);
        if (fut.wait_for(std::chrono::seconds(0)) != boost::fibers::future_status::ready) {
            st->abandoned = true;
            std::cerr << "[LibAnyar] UI thread busy — SharedBuffer falls back to a file mapping"
                      << std::endl;
            return {};
        }
    }
    return fut.get();
}

} // namespace

// ── SharedBuffer ────────────────────────────────────────────────────────────

SharedBuffer::SharedBuffer(const std::string& name, size_t size)
    : name_(name), size_(size)
{
    NativeBuffer nb = create_webview_buffer(size_);
    if (nb.buffer) {
        native_ = nb.buffer;
        data_ = nb.data;
        std::memset(data_, 0, size_);
        return;
    }

    const auto size64 = static_cast<unsigned long long>(size_);
    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(size64 >> 32), static_cast<DWORD>(size64 & 0xFFFFFFFFull),
        nullptr);
    if (!mapping) {
        throw std::runtime_error("SharedBuffer: CreateFileMapping failed for '" +
                                 name_ + "': " + win32::error_message(GetLastError()));
    }

    void* ptr = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size_);
    if (!ptr) {
        DWORD err = GetLastError();
        CloseHandle(mapping);
        throw std::runtime_error("SharedBuffer: MapViewOfFile failed for '" +
                                 name_ + "': " + win32::error_message(err));
    }

    mapping_ = mapping;
    data_ = static_cast<uint8_t*>(ptr);
    // Fresh pagefile-backed pages are already zeroed; keep parity with POSIX.
    std::memset(data_, 0, size_);
}

SharedBuffer::~SharedBuffer() {
    if (native_) {
        // Not Close(): pages that hold the ArrayBuffer keep the memory alive
        // until they release it; we just drop our reference.
        release_on_ui(static_cast<ICoreWebView2SharedBuffer*>(native_));
        native_ = nullptr;
        data_ = nullptr;
    }
    if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
}

void register_shm_uri_scheme() {}

void register_file_uri_scheme(const std::vector<std::string>& /*allowed_roots*/) {}

// ── WebView2 plumbing (UI thread) ───────────────────────────────────────────

namespace platform {

void note_webview_controller(void* controller) {
    if (!controller || have_environment()) return;
    auto* ctl = static_cast<ICoreWebView2Controller*>(controller);

    ComPtr<ICoreWebView2> wv;
    ComPtr<ICoreWebView2_2> wv2;
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Environment12> env12;
    if (FAILED(ctl->get_CoreWebView2(wv.put())) || !wv ||
        FAILED(wv->QueryInterface(IID_PPV_ARGS(wv2.put()))) ||
        FAILED(wv2->get_Environment(env.put())) ||
        FAILED(env->QueryInterface(IID_PPV_ARGS(env12.put())))) {
        std::cerr << "[LibAnyar] WebView2 runtime lacks shared buffers "
                     "(needs 1.0.1661+) — SharedBuffers use HTTP" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(g_env_mutex);
    if (!g_env) g_env = env12.detach();
}

bool post_shared_buffer(void* controller, SharedBuffer& buf,
                        const std::string& additional_json) {
    auto* sb = static_cast<ICoreWebView2SharedBuffer*>(buf.native_handle());
    if (!controller || !sb) return false;
    auto* ctl = static_cast<ICoreWebView2Controller*>(controller);

    ComPtr<ICoreWebView2> wv;
    ComPtr<ICoreWebView2_17> wv17;
    if (FAILED(ctl->get_CoreWebView2(wv.put())) || !wv ||
        FAILED(wv->QueryInterface(IID_PPV_ARGS(wv17.put())))) {
        return false;
    }
    // Read-only: the page must never scribble on producer memory.  Fails
    // (and the page falls back to HTTP) e.g. for a window whose environment
    // differs from the one that allocated the buffer.
    HRESULT hr = wv17->PostSharedBufferToScript(
        sb, COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY,
        win32::widen(additional_json).c_str());
    return SUCCEEDED(hr);
}

} // namespace platform

} // namespace anyar
