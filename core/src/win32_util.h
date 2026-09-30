#pragma once

// LibAnyar — private Win32 helpers (UTF-8 <-> UTF-16, error text).
// Only include from *_win32.cpp files.

#ifndef _WIN32
#error "win32_util.h is Windows-only"
#endif

#include <windows.h>

#include <string>

namespace anyar::win32 {

/// UTF-8 → UTF-16 (the encoding every *W Win32 API expects).
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

/// UTF-16 → UTF-8.
inline std::string narrow(const wchar_t* s, size_t len) {
    if (!s || len == 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len),
                        out.data(), n, nullptr, nullptr);
    return out;
}

inline std::string narrow(const std::wstring& s) {
    return narrow(s.data(), s.size());
}

/// Human-readable text for a GetLastError() / HRESULT code.
inline std::string error_message(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string msg = len ? narrow(buf, len) : "error " + std::to_string(code);
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ')) {
        msg.pop_back();
    }
    return msg;
}

} // namespace anyar::win32
