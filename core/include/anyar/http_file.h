#pragma once

/// @file http_file.h
/// @brief Serve local files over LibAnyar's HTTP server with HTTP Range
///        support — suitable for `<audio>`/`<video>` sources and large
///        downloads.
///
/// @code
///   std::weak_ptr<asyik::http_server<asyik::http_stream_type>> weak = ctx.server;
///   ctx.server->on_http_request("/media/stream", "GET",
///       [this, weak](asyik::http_request_ptr req, asyik::http_route_args) {
///           // weak_ptr: the route is owned by the server — avoid a cycle
///           anyar::serve_file(weak.lock(), req, current_path());   // streamed
///       });
/// @endcode
///
/// Responses always cover the FULL requested range.  Never answer an
/// open-ended `bytes=a-` with a shorter 206: WebKitGTK's media loader does
/// not request the remainder — playback stalls at the end of the short
/// response and a subsequent seek can fail with MEDIA_ERR_DECODE.

#include <libasyik/http.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace anyar {

/// Inclusive byte range [start, end].
struct ByteRange {
    int64_t start = 0;
    int64_t end   = 0;
    int64_t length() const { return end - start + 1; }
};

/// Result of parse_range_header().
enum class RangeResult {
    none,           ///< No (usable) Range header → serve the whole file (200).
    ok,             ///< @p out holds a satisfiable range → 206.
    unsatisfiable,  ///< Syntactically valid but outside the file → 416.
};

/// Parse an RFC 9110 `Range` header value for a file of @p file_size bytes.
/// Supports `bytes=a-b`, `bytes=a-` and suffix `bytes=-n`.  Multi-range
/// requests use their first range.  Malformed or non-`bytes` headers yield
/// RangeResult::none (the header is ignored, as the RFC allows).
/// @p end is clamped to the last byte of the file.
RangeResult parse_range_header(const std::string& header, int64_t file_size,
                               ByteRange& out);

/// Best-effort MIME type from a path's extension (lowercase-insensitive);
/// "application/octet-stream" when unknown.
std::string mime_type_for_path(const std::string& path);

/// Decode `%XX` escapes in a URL path segment (route args are NOT decoded
/// by LibAsyik).  `+` is left as-is (it is literal in paths); malformed
/// escapes are kept verbatim.  The result is raw bytes — UTF-8 for
/// browser-encoded non-ASCII names.  Validate (e.g. reject "..") AFTER
/// decoding.
/// @param encoded Path text as received, e.g. "my%20video%E2%9C%93.mp4".
/// @returns Decoded text, e.g. "my video✓.mp4".
/// @example `auto rel = anyar::percent_decode(args[1]);`
std::string percent_decode(const std::string& encoded);

/// Options for serve_file().
struct FileServeOptions {
    /// Content-Type; empty → mime_type_for_path().
    std::string content_type;
    /// Cache-Control header value (empty → header omitted).
    std::string cache_control = "no-store";
    /// Emit `Access-Control-Allow-Origin: *`.
    bool        cors_any_origin = true;
    /// Streaming variant only: bytes read from disk and written per step.
    std::size_t chunk_bytes = 256 * 1024;
};

/// Buffered: fill @p req's response with @p path, honouring `Range`
/// (206 / 416; `Accept-Ranges: bytes` always set; no Range → 200 whole
/// file).  The whole requested range is read into memory (on the worker
/// pool when called from a fiber) — fine for small files; use the
/// streaming overload for media and large files.  Missing file → 404.
/// @p path is UTF-8 (see `<anyar/path.h>`).
void serve_file(asyik::http_request_ptr req, const std::string& path,
                const FileServeOptions& opts = {});

/// Streaming: same semantics as the buffered overload, but the body is
/// written directly to the connection in `chunk_bytes` pieces read on the
/// worker pool — memory stays bounded and the first bytes go out
/// immediately, whatever the file size.  Must be called from the route
/// handler fiber of @p server.  A null @p server falls back to the buffered
/// overload's behaviour.  The connection is closed after the body
/// (`Connection: close`).  A client that disconnects mid-body (e.g. a media
/// element seeking) just ends the transfer.
void serve_file(const asyik::http_server_ptr<asyik::http_stream_type>& server,
                asyik::http_request_ptr req, const std::string& path,
                const FileServeOptions& opts = {});

} // namespace anyar
