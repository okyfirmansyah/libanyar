// LibAnyar — SharedBuffer memory mapping (Windows)
//
// Each buffer is an anonymous pagefile-backed file mapping.  The webview
// cannot address it directly yet (WebView2 custom schemes / shared buffers
// need hooks into environment creation), so the JS bridge fetches bytes over
// HTTP GET /__anyar__/buffer/<name>; see platform::has_shm_uri_scheme().

#include <anyar/shared_buffer.h>
#include "win32_util.h"

#include <cstring>
#include <stdexcept>

namespace anyar {

SharedBuffer::SharedBuffer(const std::string& name, size_t size)
    : name_(name), size_(size)
{
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

} // namespace anyar
