#pragma once

/// @file path.h
/// @brief UTF-8 ⇄ std::filesystem::path conversion.
///
/// Every path that crosses IPC/JSON (and every path LibAnyar hands back) is
/// UTF-8.  `std::filesystem::path(std::string)` and `path::string()` use the
/// *native narrow* encoding instead — the ANSI code page on Windows — which
/// mangles (or throws on) non-ASCII names.  Always convert through these
/// helpers.  On Linux/macOS they are plain byte copies.
///
/// @code
/// auto p = anyar::path_from_utf8(args.at("path").get<std::string>());
/// std::ifstream in(p, std::ios::binary);       // opens "日本.txt" correctly
/// return anyar::path_to_utf8(p.filename());     // back to JSON as UTF-8
/// @endcode

#include <filesystem>
#include <string>

namespace anyar {

/// Build a filesystem path from a UTF-8 string.
/// @param utf8 Path text encoded as UTF-8 (e.g. from JSON / IPC).
/// @returns    The path, correctly encoded for the native filesystem API.
inline std::filesystem::path path_from_utf8(const std::string& utf8) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::u8path(utf8);
#endif
}

/// Render a filesystem path as UTF-8 (for JSON, logs, messages).
/// @param p Any path.
/// @returns UTF-8 text of @p p (never throws on non-ASCII, unlike string()).
inline std::string path_to_utf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

/// True if @p p is @p root or lies beneath it, compared component-wise —
/// unlike a string prefix, root "/data" does not admit "/database/x".
/// Both paths should already be canonical (std::filesystem::canonical).
/// @param root Allowed directory.
/// @param p    Candidate path.
/// @returns    Whether @p p is inside @p root.
/// @example `if (!anyar::is_path_within(root, fs::canonical(p))) deny();`
inline bool is_path_within(const std::filesystem::path& root,
                           const std::filesystem::path& p) {
    auto pi = p.begin();
    for (const auto& part : root) {
        // A trailing separator yields an empty last component — ignore it.
        if (part.empty()) continue;
        if (pi == p.end() || part != *pi) return false;
        ++pi;
    }
    return true;
}

} // namespace anyar
