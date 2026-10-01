// LibAnyar — SharedBuffer factory, registry and pool (platform-neutral).
// The memory mapping itself lives in shared_buffer_<os>.cpp.

#include <anyar/shared_buffer.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>

#include <boost/fiber/operations.hpp>

namespace anyar {

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
            "SharedBuffer: buffer '" + name + "' already exists (buffer names "
            "are process-global; destroy the previous owner first)");
    }

    // Use the private constructor via a helper since make_shared needs public ctor
    auto buf = std::shared_ptr<SharedBuffer>(new SharedBuffer(name, size));
    static std::atomic<uint64_t> next_id{0};
    buf->id_ = ++next_id;

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

SharedBuffer* SharedBufferPool::try_acquire_write() {
    if (closed_.load()) {
        throw SharedBufferPoolClosed();
    }
    const size_t n = buffers_.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (write_idx_.load() + i) % n;
        Slot::State expected = Slot::FREE;
        if (buffers_[idx].state.compare_exchange_strong(expected, Slot::WRITING)) {
            write_idx_.store((idx + 1) % n);
            return buffers_[idx].buffer.get();
        }
    }
    return nullptr;
}

SharedBuffer& SharedBufferPool::acquire_write() {
    for (int spins = 0; ; ++spins) {
        if (SharedBuffer* buf = try_acquire_write()) {
            return *buf;
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
                                     const std::string& /*metadata_json*/) {
    for (auto& slot : buffers_) {
        if (slot.buffer.get() == &buf) {
            Slot::State expected = Slot::WRITING;
            slot.state.compare_exchange_strong(expected, Slot::READY);
            return;
        }
    }
}

void SharedBufferPool::release_unpublished(SharedBuffer& buf) {
    for (auto& slot : buffers_) {
        if (slot.buffer.get() == &buf) {
            Slot::State expected = Slot::WRITING;
            slot.state.compare_exchange_strong(expected, Slot::FREE);
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

std::shared_ptr<SharedBuffer> SharedBufferPool::buffer(const std::string& buffer_name) const {
    for (auto& slot : buffers_) {
        if (slot.buffer && slot.buffer->name() == buffer_name) return slot.buffer;
    }
    return nullptr;
}

void SharedBufferPool::close() {
    closed_.store(true);
}

} // namespace anyar
