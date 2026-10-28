// LibAnyar — debug tooling end-to-end test (needs a display).
//
// Generates a tiny page and a test script, runs the app with
// AppConfig::debug_tools.test_script, and checks:
//   - run() returns the code passed to anyarTest.done();
//   - console output and unhandled rejections are forwarded ([js:main] …);
//   - real (synthetic OS-level) clicks and typing reach the page   (if input);
//   - debug:screenshot writes a PNG of the real webview            (if capture).
// Capture/input are skipped where the platform reports them unsupported.

#include <anyar/app.h>
#include <anyar/path.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

/// Thread-safe capturing streambuf (console lines arrive on the service thread).
class CaptureBuf : public std::streambuf {
public:
    std::string text() {
        std::lock_guard<std::mutex> l(mu_);
        return data_;
    }
protected:
    int overflow(int ch) override {
        if (ch != EOF) {
            std::lock_guard<std::mutex> l(mu_);
            data_.push_back(char(ch));
        }
        return ch;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> l(mu_);
        data_.append(s, size_t(n));
        return n;
    }
private:
    std::mutex mu_;
    std::string data_;
};

void write(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

int fail(const std::string& why, const std::string& log) {
    std::cerr << "[test] FAIL: " << why << "\n---- captured output ----\n" << log << std::endl;
    return 1;
}

} // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("anyar_debug_tools_" + std::to_string(stamp));
    fs::create_directories(dir / "dist");
    const fs::path shot = dir / "shot.png";

    write(dir / "dist" / "index.html", R"HTML(<!doctype html><html><body style="margin:0">
<button id="b" style="position:absolute;left:20px;top:20px;width:120px;height:40px">0</button>
<input id="t" style="position:absolute;left:20px;top:90px;width:200px;height:30px">
<script>document.getElementById('b').onclick = function () { this.textContent = String(+this.textContent + 1); };</script>
</body></html>)HTML");

    write(dir / "test.js", R"JS(
console.log('hello from page');
const info = await anyarTest.info();
anyarTest.assert(info.automation && info.console, 'tools enabled');
if (info.input) {
  await anyarTest.click('#b');
  await anyarTest.click('#b');
  await anyarTest.waitFor(() => document.getElementById('b').textContent === '2', 5000);
  await anyarTest.click('#t');
  await anyarTest.type('Hi!');
  await anyarTest.waitFor(() => document.getElementById('t').value === 'Hi!', 5000);
  console.log('input ok');
}
if (info.capture) {
  const s = await anyarTest.screenshot(SHOT);
  anyarTest.assert(s.width > 0 && s.height > 0 && s.bytes > 100, 'screenshot size');
  console.log('capture ok');
}
Promise.reject(new Error('expected rejection'));
await anyarTest.sleep(300);
await anyarTest.done(7);
)JS");
    {   // Inject the screenshot path as a JS string literal.
        std::ifstream in(dir / "test.js");
        std::string js((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string lit = anyar::path_to_utf8(shot);
        for (size_t i = 0; (i = lit.find('\\', i)) != std::string::npos; i += 2) lit.insert(i, "\\");
        js = "const SHOT = '" + lit + "';\n" + js;
        write(dir / "test.js", js);
    }

    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.dist_path = anyar::path_to_utf8(dir / "dist");
    config.debug_tools.test_script = anyar::path_to_utf8(dir / "test.js");
    config.debug_tools.test_timeout_s = 40;

    CaptureBuf cap;
    auto* old_out = std::cout.rdbuf(&cap);
    auto* old_err = std::cerr.rdbuf(&cap);
    int code = -1;
    {
        anyar::App app(config);
        anyar::WindowConfig win;
        win.title = "debug tools test";
        win.width = 400;
        win.height = 300;
        app.create_window(win);
        code = app.run();
    }
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    const std::string log = cap.text();

    if (code != 7) return fail("run() returned " + std::to_string(code) + ", expected 7", log);
    if (log.find("[js:main] log: hello from page") == std::string::npos) return fail("console.log not forwarded", log);
    if (log.find("Unhandled rejection: Error: expected rejection") == std::string::npos)
        return fail("unhandled rejection not forwarded", log);
    const bool input = log.find("input ok") != std::string::npos;
    const bool capture = log.find("capture ok") != std::string::npos;
#ifdef __linux__
    if (!input) return fail("synthetic input not verified on Linux", log);
    if (!capture) return fail("capture not verified on Linux", log);
#endif
    if (capture) {
        std::ifstream png(shot, std::ios::binary);
        char magic[4] = {};
        png.read(magic, 4);
        if (std::string(magic, 4) != "\x89PNG") return fail("screenshot is not a PNG", log);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::cout << "[test] OK (input " << (input ? "verified" : "unsupported") << ", capture "
              << (capture ? "verified" : "unsupported") << ")" << std::endl;
    return 0;
}
