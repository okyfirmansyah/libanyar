#include <anyar/shared_buffer.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
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
        if (closed_.load()) {
            throw SharedBufferPoolClosed();
        }
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

void SharedBufferPool::close() {
    closed_.store(true);
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
    // reference our mmap'd pointer.
    // IMPORTANT: capture the shared_ptr in the GBytes destroy callback
    // so the buffer stays alive as long as the GInputStream uses it.
    auto* buf_ref = new std::shared_ptr<SharedBuffer>(buf);
    GBytes* bytes = g_bytes_new_with_free_func(
        buf->data(), buf->size(),
        +[](gpointer data) {
            delete static_cast<std::shared_ptr<SharedBuffer>*>(data);
        },
        buf_ref);
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

// ── anyar-file:// URI Scheme ────────────────────────────────────────────────

// Store allowed roots in a file-static so the C callback can access them.
static std::vector<std::string> g_allowed_file_roots;

static const char* mime_for_extension(const std::string& ext) {
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".webm") return "video/webm";
    if (ext == ".mp3")  return "audio/mpeg";
    if (ext == ".wav")  return "audio/wav";
    if (ext == ".ogg")  return "audio/ogg";
    if (ext == ".pdf")  return "application/pdf";
    if (ext == ".json") return "application/json";
    if (ext == ".txt")  return "text/plain";
    if (ext == ".html") return "text/html";
    if (ext == ".css")  return "text/css";
    if (ext == ".js")   return "text/javascript";
    return "application/octet-stream";
}

static void handle_file_uri_request(WebKitURISchemeRequest* request,
                                     gpointer /*user_data*/) {
    const char* uri = webkit_uri_scheme_request_get_uri(request);
    // URI format: anyar-file:///absolute/path/to/file
    std::string uri_str(uri);
    std::string prefix = "anyar-file://";
    std::string file_path;

    if (uri_str.size() > prefix.size()) {
        file_path = uri_str.substr(prefix.size());
        // Remove trailing slashes
        while (file_path.size() > 1 && file_path.back() == '/') {
            file_path.pop_back();
        }
    }

    if (file_path.empty()) {
        GError* error = g_error_new(
            g_quark_from_string("anyar-file"), 400,
            "Missing file path in URI: %s", uri);
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    // Reject path traversal attempts
    if (file_path.find("..") != std::string::npos) {
        GError* error = g_error_new(
            g_quark_from_string("anyar-file"), 403,
            "Path traversal denied: %s", uri);
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    // Resolve to canonical and verify it's under an allowed root
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path canonical = fs::canonical(fs::path(file_path), ec);
    if (ec || !fs::is_regular_file(canonical, ec)) {
        GError* error = g_error_new(
            g_quark_from_string("anyar-file"), 404,
            "File not found: %s", file_path.c_str());
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    std::string canon_str = canonical.string();
    bool allowed = false;
    for (const auto& root : g_allowed_file_roots) {
        if (canon_str.rfind(root, 0) == 0) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        GError* error = g_error_new(
            g_quark_from_string("anyar-file"), 403,
            "Access denied (not in allowed roots): %s", file_path.c_str());
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    // Read the file
    std::ifstream ifs(canonical, std::ios::binary | std::ios::ate);
    if (!ifs) {
        GError* error = g_error_new(
            g_quark_from_string("anyar-file"), 500,
            "Failed to read file: %s", canon_str.c_str());
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    auto size = ifs.tellg();
    ifs.seekg(0);
    std::string body(static_cast<size_t>(size), '\0');
    ifs.read(body.data(), size);

    const char* content_type = mime_for_extension(canonical.extension().string());

    GBytes* bytes = g_bytes_new(body.data(), body.size());
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    g_bytes_unref(bytes);

    WebKitURISchemeResponse* response =
        webkit_uri_scheme_response_new(stream, static_cast<gint64>(body.size()));
    webkit_uri_scheme_response_set_content_type(response, content_type);

    SoupMessageHeaders* headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    soup_message_headers_append(headers, "Access-Control-Allow-Origin", "*");
    soup_message_headers_append(headers, "Cache-Control", "no-store");
    webkit_uri_scheme_response_set_http_headers(response, headers);

    webkit_uri_scheme_request_finish_with_response(request, response);

    g_object_unref(response);
    g_object_unref(stream);
}

void register_file_uri_scheme(const std::vector<std::string>& allowed_roots) {
    g_allowed_file_roots = allowed_roots;

    if (g_allowed_file_roots.empty()) return;

    WebKitWebContext* context = webkit_web_context_get_default();

    webkit_web_context_register_uri_scheme(
        context,
        "anyar-file",
        handle_file_uri_request,
        nullptr,
        nullptr);

    WebKitSecurityManager* security_mgr =
        webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_cors_enabled(
        security_mgr, "anyar-file");
}

#else

// Stub for non-Linux platforms (Phase 7)
void register_shm_uri_scheme() {
    // No-op — Windows uses PostSharedBufferToScript, macOS uses WKURLSchemeHandler
}

void register_file_uri_scheme(const std::vector<std::string>& /*allowed_roots*/) {
    // No-op — Phase 7
}

#endif // __linux__

} // namespace anyar
