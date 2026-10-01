#pragma once

/// @file shared_buffer.h
/// @brief Platform-neutral shared memory buffer API for LibAnyar.
///
/// Provides zero-copy (or near-zero-copy) binary data transfer between
/// the C++ backend and the webview frontend.
///
/// On Linux:   POSIX shared memory (shm_open + mmap), served via `anyar-shm://`
/// On Windows: a WebView2 shared buffer (zero-copy: the page reads the same
///             memory via `buffer:attach` + `sharedbufferreceived`) once a
///             window exists; otherwise a pagefile-backed file mapping
///             served over HTTP (`/__anyar__/buffer/<name>`)
/// On macOS (Phase 7): POSIX shared memory + WKURLSchemeHandler

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/fiber/mutex.hpp>

namespace anyar {

// Forward declarations
class Window;
class App;

// ── SharedBuffer ────────────────────────────────────────────────────────────

/// A named shared memory region accessible from both C++ and the webview.
///
/// Usage:
///   auto buf = SharedBuffer::create("video-frame", width * height * 4);
///   memcpy(buf->data(), pixels, buf->size());
///   buf->notify(window, R"({"width":1920,"height":1080})");
///
class SharedBuffer {
public:
    /// Create a new shared buffer with the given name and byte size.
    /// The name must be unique across all active buffers.
    static std::shared_ptr<SharedBuffer> create(const std::string& name,
                                                 size_t size);

    ~SharedBuffer();

    // Non-copyable
    SharedBuffer(const SharedBuffer&) = delete;
    SharedBuffer& operator=(const SharedBuffer&) = delete;

    /// Raw pointer to the mapped memory region.
    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }

    /// Size of the buffer in bytes.
    size_t size() const { return size_; }

    /// The buffer's name (used for URI scheme lookup and IPC notifications).
    const std::string& name() const { return name_; }

    /// Process-unique generation id (never reused).  Distinguishes a buffer
    /// from a later one created under the same name.
    uint64_t id() const { return id_; }

    /// True when the webview can read this memory directly (Windows:
    /// allocated as a WebView2 shared buffer — see `buffer:attach`).
    bool is_webview_shared() const { return native_ != nullptr; }

    /// Opaque platform object behind the memory (Windows:
    /// ICoreWebView2SharedBuffer*), or nullptr.  Callers must cast per-platform.
    void* native_handle() const { return native_; }

private:
    SharedBuffer(const std::string& name, size_t size);

    std::string name_;
    std::string shm_path_;  // platform-specific path (e.g. /anyar_<pid>_<name>)
    size_t size_ = 0;
    uint8_t* data_ = nullptr;
    uint64_t id_ = 0;
    int fd_ = -1;             // file descriptor (POSIX)
    void* mapping_ = nullptr; // file-mapping HANDLE (Windows)
    void* native_ = nullptr;  // ICoreWebView2SharedBuffer* (Windows)
};

// ── SharedBufferRegistry ────────────────────────────────────────────────────

/// Global registry of active shared buffers.
/// The URI scheme handler looks up buffers by name from here.
class SharedBufferRegistry {
public:
    static SharedBufferRegistry& instance();

    /// Register a buffer (called by SharedBuffer::create)
    void add(std::shared_ptr<SharedBuffer> buf);

    /// Remove a buffer by name
    void remove(const std::string& name);

    /// Look up a buffer by name (returns nullptr if not found)
    std::shared_ptr<SharedBuffer> get(const std::string& name) const;

    /// List all active buffer names
    std::vector<std::string> names() const;

    /// Remove all buffers
    void clear();

private:
    SharedBufferRegistry() = default;
    mutable boost::fibers::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<SharedBuffer>> buffers_;
};

// ── SharedBufferPool ────────────────────────────────────────────────────────

/// A ring of N shared buffers for streaming use cases (e.g. video frames).
///
/// The producer (C++) acquires a write slot, fills it, and posts it to JS.
/// The consumer (JS) reads the buffer and calls release when done.
/// Back-pressure: if all buffers are in use, acquire_write() blocks.
///
class SharedBufferPoolClosed : public std::runtime_error {
public:
    SharedBufferPoolClosed()
        : std::runtime_error("SharedBufferPool closed") {}
};

class SharedBufferPool {
public:
    /// Create a pool of `count` buffers, each of `buffer_size` bytes.
    /// Buffer names will be `base_name_0`, `base_name_1`, etc.  Buffer names
    /// are process-global: creating a pool whose names are still in use
    /// (e.g. the previous pool with the same base name has not been
    /// destroyed yet) throws std::runtime_error.  Destroy the old pool first,
    /// or give each pool generation its own base name.
    SharedBufferPool(const std::string& base_name, size_t buffer_size,
                     size_t count = 3);

    ~SharedBufferPool();

    /// Producer: get the next writable buffer.
    /// Blocks (spins/yields) if all buffers are currently held by the consumer.
    /// Throws SharedBufferPoolClosed if the pool is closed while waiting.
    SharedBuffer& acquire_write();

    /// Producer: non-blocking variant of acquire_write().  Returns nullptr
    /// if no slot is FREE right now (live producers should drop the frame
    /// instead of stalling).  Throws SharedBufferPoolClosed if closed.
    SharedBuffer* try_acquire_write();

    /// Producer: mark a WRITING buffer READY for the consumer.
    /// The caller is responsible for notifying the frontend (e.g. emitting
    /// `buffer:ready`).  No-op unless the slot is currently WRITING.
    /// @param metadata_json  Reserved (kept for API compatibility).
    void release_write(SharedBuffer& buf, const std::string& metadata_json);

    /// Producer: give back a WRITING buffer that will NOT be published
    /// (dropped frame, seek, shutdown).  WRITING → FREE.  No-op otherwise.
    void release_unpublished(SharedBuffer& buf);

    /// Consumer (called from IPC): mark a READY/READING buffer as available
    /// for reuse.  Never frees a slot the producer is writing, so a stale or
    /// duplicate release cannot corrupt an in-progress frame.
    void release_read(const std::string& buffer_name);

    /// Shared ownership of a slot's buffer — keeps the mapping alive even if
    /// the pool is destroyed.  Returns null if @p buffer_name is not in this pool.
    std::shared_ptr<SharedBuffer> buffer(const std::string& buffer_name) const;

    /// Cancel any blocked producers and reject future acquire_write() calls.
    void close();

    /// Get the pool size
    size_t count() const { return buffers_.size(); }

    /// Get the base name
    const std::string& base_name() const { return base_name_; }

    /// Get the buffer size
    size_t buffer_size() const { return buffer_size_; }

private:
    std::string base_name_;
    size_t buffer_size_;

    struct Slot {
        std::shared_ptr<SharedBuffer> buffer;
        enum State { FREE, WRITING, READY, READING };
        std::atomic<State> state{FREE};

        Slot() = default;
        Slot(Slot&& other) noexcept
            : buffer(std::move(other.buffer)),
              state(other.state.load()) {}
    };

    std::vector<Slot> buffers_;
    std::atomic<size_t> write_idx_{0};
    std::atomic<bool> closed_{false};
};

// ── URI Scheme Registration ─────────────────────────────────────────────────

/// Register the `anyar-shm://` URI scheme handler.
/// Must be called from the main thread BEFORE any webview is created.
/// On Linux, this uses webkit_web_context_register_uri_scheme().
/// No-op on Windows (HTTP fallback is used).
void register_shm_uri_scheme();

/// Register the `anyar-file://` URI scheme handler for local file access.
/// Must be called from the main thread BEFORE any webview is created.
/// No-op on Windows — use `/__anyar__/file/<path>` there.
/// @param allowed_roots Canonical directory paths that may be accessed.
void register_file_uri_scheme(const std::vector<std::string>& allowed_roots);

} // namespace anyar
