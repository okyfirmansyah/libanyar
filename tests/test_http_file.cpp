// LibAnyar — HTTP Range parsing + serve_file() tests

#define CATCH_CONFIG_NO_POSIX_SIGNALS
#include <catch2/catch.hpp>

#include <anyar/http_file.h>
#include <anyar/path.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <atomic>
#include <string>
#include <thread>

#include <boost/fiber/operations.hpp>
#include "test_port.h"

using namespace anyar;

TEST_CASE("parse_range_header: forms and edge cases", "[http_file]") {
    ByteRange r;
    SECTION("closed range") {
        REQUIRE(parse_range_header("bytes=0-99", 1000, r) == RangeResult::ok);
        CHECK(r.start == 0); CHECK(r.end == 99); CHECK(r.length() == 100);
    }
    SECTION("open-ended") {
        REQUIRE(parse_range_header("bytes=500-", 1000, r) == RangeResult::ok);
        CHECK(r.start == 500); CHECK(r.end == 999);
    }
    SECTION("suffix") {
        REQUIRE(parse_range_header("bytes=-100", 1000, r) == RangeResult::ok);
        CHECK(r.start == 900); CHECK(r.end == 999);
    }
    SECTION("suffix longer than file") {
        REQUIRE(parse_range_header("bytes=-5000", 1000, r) == RangeResult::ok);
        CHECK(r.start == 0); CHECK(r.end == 999);
    }
    SECTION("end clamped to file") {
        REQUIRE(parse_range_header("bytes=10-99999", 1000, r) == RangeResult::ok);
        CHECK(r.end == 999);
    }
    SECTION("multi-range uses the first") {
        REQUIRE(parse_range_header("bytes=0-9, 20-29", 1000, r) == RangeResult::ok);
        CHECK(r.start == 0); CHECK(r.end == 9);
    }
    SECTION("whitespace and case tolerated") {
        REQUIRE(parse_range_header("  Bytes= 5 - 6 ", 1000, r) == RangeResult::ok);
        CHECK(r.start == 5); CHECK(r.end == 6);
    }
    SECTION("start beyond EOF is unsatisfiable") {
        CHECK(parse_range_header("bytes=1000-", 1000, r) == RangeResult::unsatisfiable);
        CHECK(parse_range_header("bytes=-0", 1000, r) == RangeResult::unsatisfiable);
        CHECK(parse_range_header("bytes=0-", 0, r) == RangeResult::unsatisfiable);
    }
    SECTION("malformed is ignored") {
        CHECK(parse_range_header("", 1000, r) == RangeResult::none);
        CHECK(parse_range_header("items=0-9", 1000, r) == RangeResult::none);
        CHECK(parse_range_header("bytes=abc-", 1000, r) == RangeResult::none);
        CHECK(parse_range_header("bytes=9-1", 1000, r) == RangeResult::none);
        CHECK(parse_range_header("bytes=-", 1000, r) == RangeResult::none);
        CHECK(parse_range_header("bytes=5", 1000, r) == RangeResult::none);
        CHECK(parse_range_header("bytes=99999999999999999999-", 1000, r) == RangeResult::none);
    }
}

TEST_CASE("mime_type_for_path", "[http_file]") {
    CHECK(mime_type_for_path("/a/b.MP4") == "video/mp4");
    CHECK(mime_type_for_path("x.mkv") == "video/x-matroska");
    CHECK(mime_type_for_path("x.unknown") == "application/octet-stream");
    CHECK(mime_type_for_path("noext") == "application/octet-stream");
}

namespace {

struct TempFile {
    std::string path;
    std::string content;
    explicit TempFile(size_t n) {
        path = (std::filesystem::temp_directory_path() /
                ("anyar_http_file_" + std::to_string(std::random_device{}()) + ".bin")).string();
        content.resize(n);
        for (size_t i = 0; i < n; ++i) content[i] = static_cast<char>(i * 31 + 7);
        std::ofstream(path, std::ios::binary).write(content.data(), content.size());
    }
    ~TempFile() { std::error_code ec; std::filesystem::remove(path, ec); }
};

int pick_port() {
    return anyar_test::free_port();
}

struct Resp { int status; std::string body; std::string content_range; };

Resp get(asyik::service_ptr svc, int port, const std::string& route, const std::string& range) {
    std::map<asyik::string_view, asyik::string_view> headers;
    if (!range.empty()) headers.emplace("Range", range);
    auto req = asyik::http_easy_request(
        svc, "GET", "http://127.0.0.1:" + std::to_string(port) + route, "", headers);
    Resp r{static_cast<int>(req->response.result()), req->response.body, ""};
    auto it = req->response.headers.find("Content-Range");
    if (it != req->response.headers.end()) r.content_range = std::string(it->value());
    return r;
}

} // namespace

static void run_serve_file_suite(bool streaming) {
    TempFile file(3 * 1024 * 1024 + 123);   // several streaming chunks
    auto svc = asyik::make_service();
    int port = pick_port();
    std::vector<Resp> results;

    svc->execute([&] {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        FileServeOptions opts;
        opts.chunk_bytes = 64 * 1024;
        auto serve = [&, server](asyik::http_request_ptr req, const std::string& path) {
            if (streaming) serve_file(server, req, path, opts);
            else           serve_file(req, path, opts);
        };
        server->on_http_request("/f", "GET", [&](asyik::http_request_ptr req, asyik::http_route_args) {
            serve(req, file.path);
        });
        server->on_http_request("/missing", "GET", [&](asyik::http_request_ptr req, asyik::http_route_args) {
            serve(req, file.path + ".nope");
        });

        results.push_back(get(svc, port, "/f", ""));                  // 0 full
        results.push_back(get(svc, port, "/f", "bytes=100-199"));     // 1 closed range
        results.push_back(get(svc, port, "/f", "bytes=2000-"));       // 2 open range → to EOF
        results.push_back(get(svc, port, "/f", "bytes=-10"));         // 3 suffix
        results.push_back(get(svc, port, "/f", "bytes=99999999-"));   // 4 unsatisfiable
        results.push_back(get(svc, port, "/missing", ""));            // 5 404
        svc->stop();
    });
    svc->run();

    const auto size = std::to_string(file.content.size());
    REQUIRE(results.size() == 6);
    CHECK(results[0].status == 200);
    CHECK(results[0].body == file.content);

    CHECK(results[1].status == 206);
    CHECK(results[1].body == file.content.substr(100, 100));
    CHECK(results[1].content_range == "bytes 100-199/" + size);

    // An open-ended range MUST be answered in full — WebKitGTK's media loader
    // does not ask for the remainder of a shortened 206 (playback stalls).
    CHECK(results[2].status == 206);
    CHECK(results[2].body == file.content.substr(2000));
    CHECK(results[2].content_range ==
          "bytes 2000-" + std::to_string(file.content.size() - 1) + "/" + size);

    CHECK(results[3].status == 206);
    CHECK(results[3].body == file.content.substr(file.content.size() - 10));

    CHECK(results[4].status == 416);
    CHECK(results[4].content_range == "bytes */" + size);

    CHECK(results[5].status == 404);
}

TEST_CASE("serve_file (buffered): full-range semantics over real HTTP", "[http_file][integration]") {
    run_serve_file_suite(false);
}

TEST_CASE("serve_file (streaming): full-range semantics and chunked body", "[http_file][integration]") {
    run_serve_file_suite(true);
}

TEST_CASE("serve_file (streaming): client abort mid-body does not wedge the server", "[http_file][integration]") {
    TempFile file(8 * 1024 * 1024);
    auto svc = asyik::make_service();
    int port = pick_port();
    int after_status = 0;

    svc->execute([&] {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        server->on_http_request("/f", "GET", [&, server](asyik::http_request_ptr req, asyik::http_route_args) {
            FileServeOptions o; o.chunk_bytes = 16 * 1024;
            serve_file(server, req, file.path, o);
        });
        // Raw client on its own thread (blocking I/O must not run on the
        // service thread): send a request, read a little, then hang up.
        std::atomic<bool> client_done{false};
        std::thread client([&] {
            boost::asio::io_context io;
            boost::asio::ip::tcp::socket sock(io);
            sock.connect({boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
            std::string rq = "GET /f HTTP/1.1\r\nHost: x\r\nRange: bytes=0-\r\n\r\n";
            boost::asio::write(sock, boost::asio::buffer(rq));
            char buf[4096];
            sock.read_some(boost::asio::buffer(buf));
            sock.close();
            client_done = true;
        });
        while (!client_done) boost::this_fiber::sleep_for(std::chrono::milliseconds(2));
        client.join();
        boost::this_fiber::sleep_for(std::chrono::milliseconds(50));
        after_status = get(svc, port, "/f", "bytes=0-9").status;   // server still serves
        svc->stop();
    });
    svc->run();
    CHECK(after_status == 206);
}

TEST_CASE("percent_decode", "[http_file]") {
    CHECK(percent_decode("plain.txt") == "plain.txt");
    CHECK(percent_decode("my%20video.mp4") == "my video.mp4");
    CHECK(percent_decode("%E6%97%A5%E6%9C%AC.txt") == "\xE6\x97\xA5\xE6\x9C\xAC.txt");  // 日本
    CHECK(percent_decode("a%2Fb") == "a/b");
    CHECK(percent_decode("a+b") == "a+b");        // '+' is literal in paths
    CHECK(percent_decode("bad%zzx") == "bad%zzx"); // malformed kept verbatim
    CHECK(percent_decode("trail%4") == "trail%4");
    CHECK(percent_decode("%2e%2e/x") == "../x");   // callers check ".." AFTER decoding
}

// Non-ASCII file names must round-trip as UTF-8 (on Windows,
// std::string → path would otherwise go through the ANSI code page).
TEST_CASE("serve_file: non-ASCII (UTF-8) path", "[http_file][integration]") {
    const std::string name = u8"anyar_ünïcødé_日本_"
                             + std::to_string(std::random_device{}()) + ".txt";
    const auto dir = std::filesystem::temp_directory_path();
    const std::string utf8_path = path_to_utf8(dir) + "/" + name;
    const std::string content = "hello from a non-ASCII file";
    std::ofstream(path_from_utf8(utf8_path), std::ios::binary) << content;

    auto svc = asyik::make_service();
    int port = pick_port();
    Resp buffered{}, streamed{};
    svc->execute([&] {
        auto server = asyik::make_http_server(svc, "127.0.0.1", port);
        server->on_http_request("/b", "GET", [&](asyik::http_request_ptr req, asyik::http_route_args) {
            serve_file(req, utf8_path);
        });
        server->on_http_request("/s", "GET", [&, server](asyik::http_request_ptr req, asyik::http_route_args) {
            serve_file(server, req, utf8_path);
        });
        buffered = get(svc, port, "/b", "");
        streamed = get(svc, port, "/s", "");
        svc->stop();
    });
    svc->run();

    std::error_code ec;
    std::filesystem::remove(path_from_utf8(utf8_path), ec);
    CHECK(buffered.status == 200);
    CHECK(buffered.body == content);
    CHECK(streamed.status == 200);
    CHECK(streamed.body == content);
}
