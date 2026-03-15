#pragma once

// LibAnyar — Embedded Frontend Resources
//
// When ANYAR_EMBED_FRONTEND is defined, this header provides
// make_embedded_resolver() which creates a FileResolver backed by cmrc
// (CMake Resource Compiler) embedded resources.
//
// Usage in your main.cpp:
//
//   #ifdef ANYAR_EMBED_FRONTEND
//   #include <anyar/embed.h>
//   #endif
//
//   // ... after app creation ...
//   #ifdef ANYAR_EMBED_FRONTEND
//   app.set_frontend_resolver(anyar::make_embedded_resolver());
//   #endif
//
// The CMake helper anyar_embed_frontend() handles all the wiring:
//   include(AnyarEmbed)
//   anyar_embed_frontend(my_target "${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist")

#ifdef ANYAR_EMBED_FRONTEND

#include <cmrc/cmrc.hpp>
#include <anyar/app.h>
#include <string>
#include <unordered_map>
#include <algorithm>

CMRC_DECLARE(anyar_embedded);

namespace anyar {

namespace detail {

/// Detect MIME type from file extension (covers common web assets)
inline std::string mime_from_ext(const std::string& path) {
    static const std::unordered_map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"},
        {".htm",  "text/html; charset=utf-8"},
        {".css",  "text/css; charset=utf-8"},
        {".js",   "application/javascript; charset=utf-8"},
        {".mjs",  "application/javascript; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".svg",  "image/svg+xml"},
        {".ico",  "image/x-icon"},
        {".webp", "image/webp"},
        {".avif", "image/avif"},
        {".woff", "font/woff"},
        {".woff2","font/woff2"},
        {".ttf",  "font/ttf"},
        {".otf",  "font/otf"},
        {".eot",  "application/vnd.ms-fontobject"},
        {".wasm", "application/wasm"},
        {".map",  "application/json"},
        {".txt",  "text/plain; charset=utf-8"},
        {".xml",  "application/xml; charset=utf-8"},
        {".webm", "video/webm"},
        {".mp4",  "video/mp4"},
        {".mp3",  "audio/mpeg"},
        {".ogg",  "audio/ogg"},
    };

    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto it = types.find(ext);
    return (it != types.end()) ? it->second : "application/octet-stream";
}

} // namespace detail

/// Create a FileResolver backed by cmrc embedded resources.
///
/// The resolver serves files embedded at build time via the
/// `anyar_embed_frontend()` CMake function.
/// It handles:
///   - Direct file lookups (e.g., /assets/index-abc.js)
///   - index.html serving for "/" requests
///   - SPA fallback: non-file paths → index.html
inline FileResolver make_embedded_resolver() {
    return [](const std::string& request_path,
              std::string& out_body,
              std::string& out_content_type) -> bool {

        auto fs = cmrc::anyar_embedded::get_filesystem();

        // Normalise path: strip leading "/"
        std::string path = request_path;
        if (!path.empty() && path[0] == '/') {
            path = path.substr(1);
        }

        // Root → index.html
        if (path.empty()) {
            path = "index.html";
        }

        // Try exact match
        if (fs.exists(path) && fs.is_file(path)) {
            auto file = fs.open(path);
            out_body.assign(file.begin(), file.end());
            out_content_type = detail::mime_from_ext(path);
            return true;
        }

        // SPA fallback: if path has no file extension, serve index.html
        // This supports client-side routing (e.g., /settings, /about)
        if (path.find('.') == std::string::npos && fs.exists("index.html")) {
            auto file = fs.open("index.html");
            out_body.assign(file.begin(), file.end());
            out_content_type = "text/html; charset=utf-8";
            return true;
        }

        return false; // 404
    };
}

} // namespace anyar

#endif // ANYAR_EMBED_FRONTEND
