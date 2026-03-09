#include <anyar/shared_buffer.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

#include <boost/fiber/operations.hpp>

// Linux: POSIX shared memory
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

// Linux: WebKitGTK for URI scheme registration
#ifdef __linux__
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#endif

namespace anyar {

// ── SharedBuffer (Linux: POSIX shm) ────────────────────────────────────────

SharedBuffer::SharedBuffer(const std::string& name, size_t size)
    : name_(name), size_(size)
{
    // Generate a unique shm path: /anyar_<pid>_<name>
    shm_path_ = "/anyar_" + std::to_string(::getpid()) + "_" + name_;

    // Create shared memory object
    fd_ = ::shm_open(shm_path_.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
        throw std::runtime_error(
            "SharedBuffer: shm_open failed for '" + shm_path_ + "': " +
            std::strerror(errno));
    }

    // Set size
    if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
        ::close(fd_);
        ::shm_unlink(shm_path_.c_str());
        throw std::runtime_error(
            "SharedBuffer: ftruncate failed: " + std::string(std::strerror(errno)));
    }

    // Map into process address space
    void* ptr = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (ptr == MAP_FAILED) {
        ::close(fd_);
        ::shm_unlink(shm_path_.c_str());
        throw std::runtime_error(
            "SharedBuffer: mmap failed: " + std::string(std::strerror(errno)));
    }

    data_ = static_cast<uint8_t*>(ptr);

    // Zero-initialize
    std::memset(data_, 0, size_);
}

SharedBuffer::~SharedBuffer() {
    if (data_ && data_ != reinterpret_cast<uint8_t*>(MAP_FAILED)) {
        ::munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!shm_path_.empty()) {
        ::shm_unlink(shm_path_.c_str());
    }
}

std::shared_ptr<SharedBuffer> SharedBuffer::create(const std::string& name,
                                                    size_t size) {
    if (name.empty()) {
        throw std::invalid_argument("SharedBuffer name cannot be empty");
    }
    if (size == 0) {
        throw std::invalid_argument("SharedBuffer size must be > 0");
    }

    // Check for duplicate
    if (SharedBufferRegistry::instance().get(name)) {
        throw std::runtime_error(
            "SharedBuffer: buffer '" + name + "' already exists");
    }

    // Use the private constructor via a helper since make_shared needs public ctor
    auto buf = std::shared_ptr<SharedBuffer>(new SharedBuffer(name, size));

    // Register in the global registry
    SharedBufferRegistry::instance().add(buf);

    return buf;
}

// ── SharedBufferRegistry ────────────────────────────────────────────────────

SharedBufferRegistry& SharedBufferRegistry::instance() {
    static SharedBufferRegistry reg;
    return reg;
}

void SharedBufferRegistry::add(std::shared_ptr<SharedBuffer> buf) {
    std::lock_guard<boost::fibers::mutex> lock(mu_);
    buffers_[buf->name()] = std::move(buf);
}

void SharedBufferRegistry::remove(const std::string& name) {
    std::lock_guard<boost::fibers::mutex> lock(mu_);
    buffers_.erase(name);
}

std::shared_ptr<SharedBuffer> SharedBufferRegistry::get(const std::string& name) const {
    std::lock_guard<boost::fibers::mutex> lock(mu_);
    auto it = buffers_.find(name);
    return (it != buffers_.end()) ? it->second : nullptr;
}

std::vector<std::string> SharedBufferRegistry::names() const {
    std::lock_guard<boost::fibers::mutex> lock(mu_);
    std::vector<std::string> result;
    result.reserve(buffers_.size());
    for (auto& [name, _] : buffers_) {
        result.push_back(name);
    }
    return result;
}

void SharedBufferRegistry::clear() {
    std::lock_guard<boost::fibers::mutex> lock(mu_);
    buffers_.clear();
}

// ── SharedBufferPool ────────────────────────────────────────────────────────

SharedBufferPool::SharedBufferPool(const std::string& base_name,
                                   size_t buffer_size, size_t count)
    : base_name_(base_name), buffer_size_(buffer_size)
{
    if (count == 0) {
        throw std::invalid_argument("SharedBufferPool count must be > 0");
    }

    buffers_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        std::string slot_name = base_name_ + "_" + std::to_string(i);
        buffers_[i].buffer = SharedBuffer::create(slot_name, buffer_size);
        buffers_[i].state.store(Slot::FREE);
    }
}

SharedBufferPool::~SharedBufferPool() {
    // Remove buffers from registry
    for (auto& slot : buffers_) {
        if (slot.buffer) {
            SharedBufferRegistry::instance().remove(slot.buffer->name());
        }
    }
}

SharedBuffer& SharedBufferPool::acquire_write() {
    // Try to find a FREE slot, starting from write_idx_
    const size_t n = buffers_.size();
    for (int spins = 0; ; ++spins) {
        for (size_t i = 0; i < n; ++i) {
            size_t idx = (write_idx_.load() + i) % n;
            Slot::State expected = Slot::FREE;
            if (buffers_[idx].state.compare_exchange_strong(expected, Slot::WRITING)) {
                write_idx_.store((idx + 1) % n);
                return *buffers_[idx].buffer;
            }
        }
        // All slots busy — yield the FIBRE (not the thread) so that
        // other fibres (e.g. IPC handlers that release buffers) can run.
        if (spins < 100) {
            boost::this_fiber::yield();
        } else {
            boost::this_fiber::sleep_for(std::chrono::microseconds(100));
        }
    }
}

void SharedBufferPool::release_write(SharedBuffer& buf,
                                     const std::string& metadata_json) {
    for (auto& slot : buffers_) {
        if (slot.buffer.get() == &buf) {
            slot.state.store(Slot::READY);
            return;
        }
    }
}

void SharedBufferPool::release_read(const std::string& buffer_name) {
    for (auto& slot : buffers_) {
        if (slot.buffer && slot.buffer->name() == buffer_name) {
            Slot::State expected = Slot::READING;
            if (!slot.state.compare_exchange_strong(expected, Slot::FREE)) {
                // Also handle READY → FREE (if consumer releases without reading)
                expected = Slot::READY;
                slot.state.compare_exchange_strong(expected, Slot::FREE);
            }
            return;
        }
    }
}

// ── URI Scheme Registration (Linux) ─────────────────────────────────────────

#ifdef __linux__

static void handle_shm_uri_request(WebKitURISchemeRequest* request,
                                    gpointer /*user_data*/) {
    const char* uri = webkit_uri_scheme_request_get_uri(request);
    // URI format: anyar-shm://<buffer-name>
    // Parse out the buffer name (everything after "anyar-shm://")
    std::string uri_str(uri);
    std::string prefix = "anyar-shm://";
    std::string buffer_name;

    if (uri_str.size() > prefix.size()) {
        buffer_name = uri_str.substr(prefix.size());
        // Remove trailing slashes
        while (!buffer_name.empty() && buffer_name.back() == '/') {
            buffer_name.pop_back();
        }
    }

    if (buffer_name.empty()) {
        GError* error = g_error_new(
            g_quark_from_string("anyar-shm"), 404,
            "Missing buffer name in URI: %s", uri);
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    auto buf = SharedBufferRegistry::instance().get(buffer_name);
    if (!buf) {
        GError* error = g_error_new(
            g_quark_from_string("anyar-shm"), 404,
            "Shared buffer not found: %s", buffer_name.c_str());
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    // Mark the pool slot as READING if it's in a pool
    // (standalone buffers are always readable)
    // We use GBytes to wrap the data WITHOUT copying — GBytes will just
    // reference our mmap'd pointer. The shared_ptr ref keeps it alive.
    auto buf_ref = buf;  // prevent premature destruction
    GBytes* bytes = g_bytes_new_static(buf->data(), buf->size());
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    g_bytes_unref(bytes);  // stream holds a ref

    // Create response with CORS headers for cross-origin fetch
    WebKitURISchemeResponse* response =
        webkit_uri_scheme_response_new(stream, static_cast<gint64>(buf->size()));
    webkit_uri_scheme_response_set_content_type(response, "application/octet-stream");

    // Add CORS headers
    SoupMessageHeaders* headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    soup_message_headers_append(headers, "Access-Control-Allow-Origin", "*");
    soup_message_headers_append(headers, "Access-Control-Allow-Methods", "GET");
    soup_message_headers_append(headers, "Cache-Control", "no-store");
    webkit_uri_scheme_response_set_http_headers(response, headers);

    webkit_uri_scheme_request_finish_with_response(request, response);

    g_object_unref(response);
    g_object_unref(stream);
}

void register_shm_uri_scheme() {
    WebKitWebContext* context = webkit_web_context_get_default();

    webkit_web_context_register_uri_scheme(
        context,
        "anyar-shm",
        handle_shm_uri_request,
        nullptr,    // user_data
        nullptr);   // destroy_notify

    // Register the scheme as CORS-enabled so fetch() from http:// works
    WebKitSecurityManager* security_mgr =
        webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_cors_enabled(
        security_mgr, "anyar-shm");
}

#else

// Stub for non-Linux platforms (Phase 7)
void register_shm_uri_scheme() {
    // No-op — Windows uses PostSharedBufferToScript, macOS uses WKURLSchemeHandler
}

#endif // __linux__

} // namespace anyar
