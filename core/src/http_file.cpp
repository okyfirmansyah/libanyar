#include <anyar/http_file.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

namespace anyar {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/// Parse a non-empty run of decimal digits; nullopt on anything else/overflow.
std::optional<int64_t> parse_uint(const std::string& s) {
    if (s.empty() || s.size() > 18) return std::nullopt;
    int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}

/// What to send for one request (computed off the service thread).
struct Plan {
    bool        found = false;
    int64_t     file_size = 0;
    RangeResult kind = RangeResult::none;
    ByteRange   range;               // valid when kind == ok
    int64_t     start = 0, length = 0;
};

Plan make_plan(const std::string& path, const std::string& range_hdr) {
    Plan p;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return p;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) return p;
    p.found = true;
    p.file_size = static_cast<int64_t>(size);
    p.kind = range_hdr.empty() ? RangeResult::none
                               : parse_range_header(range_hdr, p.file_size, p.range);
    if (p.kind == RangeResult::ok) {
        p.start  = p.range.start;
        p.length = p.range.length();
    } else if (p.kind == RangeResult::none) {
        p.length = p.file_size;
    }
    return p;
}

/// Run @p fn on the worker pool when inside a LibAsyik fiber, else inline.
template <typename F>
auto offload(F&& fn) -> decltype(fn()) {
    if (auto svc = asyik::get_current_service()) return svc->async(std::forward<F>(fn)).get();
    return fn();
}

std::string request_range(const asyik::http_request_ptr& req) {
    auto it = req->headers.find("Range");
    return it != req->headers.end() ? std::string(it->value()) : std::string();
}

/// Set status + headers for @p plan on any Beast response header.
template <typename Header>
void apply_headers(Header& h, const Plan& plan, const std::string& path,
                   const FileServeOptions& opts) {
    if (opts.cors_any_origin) h.set("Access-Control-Allow-Origin", "*");
    if (!plan.found) {
        h.result(404);
        h.set("Content-Type", "text/plain");
        return;
    }
    h.set("Accept-Ranges", "bytes");
    if (!opts.cache_control.empty()) h.set("Cache-Control", opts.cache_control);
    if (plan.kind == RangeResult::unsatisfiable) {
        h.result(416);
        h.set("Content-Range", "bytes */" + std::to_string(plan.file_size));
        return;
    }
    h.set("Content-Type", opts.content_type.empty() ? mime_type_for_path(path) : opts.content_type);
    if (plan.kind == RangeResult::ok) {
        h.result(206);
        h.set("Content-Range", "bytes " + std::to_string(plan.range.start) + "-" +
                               std::to_string(plan.range.end) + "/" +
                               std::to_string(plan.file_size));
    } else {
        h.result(200);
    }
}

} // namespace

RangeResult parse_range_header(const std::string& header, int64_t file_size,
                               ByteRange& out) {
    std::string h = trim(header);
    const std::string unit = "bytes=";
    if (h.size() <= unit.size()) return RangeResult::none;
    for (size_t i = 0; i < unit.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(h[i])) != unit[i]) return RangeResult::none;
    }
    std::string spec = h.substr(unit.size());
    if (auto comma = spec.find(','); comma != std::string::npos) spec = spec.substr(0, comma);
    spec = trim(spec);

    auto dash = spec.find('-');
    if (dash == std::string::npos) return RangeResult::none;
    std::string a = trim(spec.substr(0, dash));
    std::string b = trim(spec.substr(dash + 1));

    if (a.empty()) {
        // Suffix range: last n bytes.
        auto n = parse_uint(b);
        if (!n) return RangeResult::none;
        if (*n == 0 || file_size == 0) return RangeResult::unsatisfiable;
        out.start = std::max<int64_t>(0, file_size - *n);
        out.end   = file_size - 1;
        return RangeResult::ok;
    }

    auto start = parse_uint(a);
    if (!start) return RangeResult::none;
    std::optional<int64_t> end;
    if (!b.empty()) {
        end = parse_uint(b);
        if (!end || *end < *start) return RangeResult::none;   // invalid → ignore
    }
    if (*start >= file_size) return RangeResult::unsatisfiable;
    out.start = *start;
    out.end   = end ? std::min(*end, file_size - 1) : file_size - 1;
    return RangeResult::ok;
}

std::string mime_type_for_path(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::pair<const char*, const char*> table[] = {
        {".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"},
        {".js", "text/javascript"}, {".mjs", "text/javascript"},
        {".json", "application/json"}, {".txt", "text/plain"},
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}, {".webp", "image/webp"}, {".svg", "image/svg+xml"},
        {".mp4", "video/mp4"}, {".m4v", "video/mp4"}, {".webm", "video/webm"},
        {".mkv", "video/x-matroska"}, {".mka", "audio/x-matroska"}, {".avi", "video/x-msvideo"},
        {".mov", "video/quicktime"}, {".ogg", "video/ogg"}, {".ogv", "video/ogg"},
        {".flv", "video/x-flv"}, {".wmv", "video/x-ms-wmv"},
        {".mp3", "audio/mpeg"}, {".m4a", "audio/mp4"}, {".wav", "audio/wav"},
        {".oga", "audio/ogg"}, {".flac", "audio/flac"}, {".opus", "audio/ogg"},
        {".pdf", "application/pdf"}, {".wasm", "application/wasm"},
    };
    for (auto& [e, m] : table) {
        if (ext == e) return m;
    }
    return "application/octet-stream";
}

void serve_file(asyik::http_request_ptr req, const std::string& path,
                const FileServeOptions& opts) {
    const std::string range_hdr = request_range(req);
    std::string body;
    const Plan plan = offload([&] {
        Plan p = make_plan(path, range_hdr);
        if (p.found && p.length > 0) {
            std::ifstream ifs(path, std::ios::binary);
            body.resize(static_cast<size_t>(p.length));
            ifs.seekg(p.start);
            ifs.read(body.data(), p.length);
            body.resize(static_cast<size_t>(std::max<std::streamsize>(0, ifs.gcount())));
        }
        return p;
    });

    auto& res = req->response;
    apply_headers(res.beast_response, plan, path, opts);
    if (!plan.found) { res.body = "File not found"; return; }
    if (plan.kind == RangeResult::unsatisfiable) { res.body.clear(); return; }
    res.headers.set("Content-Length", std::to_string(body.size()));
    res.body = std::move(body);
}

void serve_file(const asyik::http_server_ptr<asyik::http_stream_type>& server,
                asyik::http_request_ptr req, const std::string& path,
                const FileServeOptions& opts) {
    namespace bhttp = boost::beast::http;

    const std::string range_hdr = request_range(req);
    const Plan plan = offload([&] { return make_plan(path, range_hdr); });

    // Errors and empty bodies: a normal (buffered) response is simplest.
    auto conn = server ? req->get_connection_handle(server) : nullptr;
    if (!conn || !plan.found || plan.kind == RangeResult::unsatisfiable || plan.length == 0) {
        auto& res = req->response;
        apply_headers(res.beast_response, plan, path, opts);
        res.body = plan.found ? "" : "File not found";
        return;
    }

    auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*file) {
        req->response.result(404);
        req->response.body = "File not found";
        return;
    }

    // Take over the connection: header first, then the body in chunks.
    req->activate_direct_response_handling();
    auto& stream = conn->get_stream();
    try {
        bhttp::response<bhttp::empty_body> head;
        head.version(req->beast_request.version());
        apply_headers(head, plan, path, opts);
        head.content_length(static_cast<uint64_t>(plan.length));
        head.keep_alive(false);
        bhttp::response_serializer<bhttp::empty_body> sr{head};
        bhttp::async_write_header(stream, sr, asyik::use_fiber_future).get();

        const size_t chunk = std::max<size_t>(opts.chunk_bytes, 4096);
        std::vector<char> buf(chunk);
        file->seekg(plan.start);
        int64_t remaining = plan.length;
        while (remaining > 0) {
            const auto want = static_cast<std::streamsize>(std::min<int64_t>(remaining, chunk));
            const auto got = offload([&] {
                file->read(buf.data(), want);
                return file->gcount();
            });
            if (got <= 0) break;   // file shrank: client sees a short body
            boost::asio::async_write(stream, boost::asio::buffer(buf.data(), static_cast<size_t>(got)),
                                     asyik::use_fiber_future).get();
            remaining -= got;
        }
    } catch (const std::exception&) {
        // Client went away mid-body (seek/abort) or server shutting down.
        // The server closes this connection after a direct response.
    }
}

} // namespace anyar
