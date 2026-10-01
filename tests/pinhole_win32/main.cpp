// LibAnyar — Pinhole on Windows: end-to-end pixel test (requires a desktop).
//
// Captures its own window with PrintWindow(PW_RENDERFULLCONTENT) — which
// includes DirectComposition content — and checks pixel colours:
//   A (red,  z 0) and B (green, z 1) overlap → B on top; set_z_index(A, 2) →
//   A on top; B hidden → gone; A moved with set_rect → follows;
//   C (blue, force_fallback) drawn by the 2D-canvas fallback at the position
//   reported by the DOM tracking JS.  Then a clean shutdown with live
//   pinholes.

// LibAsyik defines NOGDI for its consumers; this test needs GDI capture.
#ifdef NOGDI
#undef NOGDI
#endif

#include <anyar/app.h>
#include <anyar/main_thread.h>
#include <anyar/pinhole.h>
#include <anyar/window.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

struct Rgb { int r, g, b; };

const char* kPage = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>pinhole win32 test</title></head>
<body style="margin:0;background:transparent">
<div data-anyar-pinhole="C"
     style="position:absolute;left:420px;top:260px;width:120px;height:80px"></div>
</body></html>)HTML";

int g_failures = 0;
HWND g_hwnd = nullptr;

/// Pixel at CSS coordinates (x, y) of the client area, from a fresh capture.
Rgb pixel_css(int x, int y) {
    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    POINT origin{0, 0};
    ClientToScreen(g_hwnd, &origin);
    const double dpr = GetDpiForWindow(g_hwnd) / 96.0;
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(g_hwnd, mem, 2 /*PW_RENDERFULLCONTENT*/);
    COLORREF c = GetPixel(mem, (origin.x - wr.left) + static_cast<int>(x * dpr),
                          (origin.y - wr.top) + static_cast<int>(y * dpr));
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return {GetRValue(c), GetGValue(c), GetBValue(c)};
}

bool close_enough(const Rgb& a, const Rgb& b) {
    return std::abs(a.r - b.r) < 40 && std::abs(a.g - b.g) < 40 && std::abs(a.b - b.b) < 40;
}

/// Poll until the pixel matches (or not) — rendering is asynchronous.
void expect_pixel(const char* what, int x, int y, Rgb want, bool equal = true) {
    Rgb got{};
    for (int i = 0; i < 50; ++i) {
        got = pixel_css(x, y);
        if (close_enough(got, want) == equal) {
            std::cout << "[ok]   " << what << "\n";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "[FAIL] " << what << ": pixel (" << x << "," << y << ") = " << got.r << ","
              << got.g << "," << got.b << (equal ? " expected " : " expected NOT ") << want.r
              << "," << want.g << "," << want.b << "\n";
    ++g_failures;
}

void expect(const char* what, bool cond) {
    std::cout << (cond ? "[ok]   " : "[FAIL] ") << what << "\n";
    if (!cond) ++g_failures;
}

const Rgb kRed{255, 0, 0}, kGreen{0, 255, 0}, kBlue{0, 0, 255};

std::shared_ptr<anyar::Pinhole> solid(anyar::Window& w, const std::string& id, float r, float g,
                                      float b, anyar::PinholeOptions opts = {}) {
    auto pin = w.create_pinhole(id, opts);
    pin->on_render([r, g, b](anyar::PinholeRenderContext& ctx) { ctx.clear(r, g, b, 1.f); });
    return pin;
}

}  // namespace

int main() {
    fs::path dist = fs::temp_directory_path() /
                    ("anyar-pinhole-win32-" + std::to_string(std::random_device{}()));
    fs::create_directories(dist);
    std::ofstream(dist / "index.html") << kPage;

    std::thread([dist] {  // watchdog
        std::this_thread::sleep_for(std::chrono::seconds(45));
        std::cerr << "[FAIL] timed out\n";
        std::error_code ec;
        fs::remove_all(dist, ec);
        std::_Exit(3);
    }).detach();

    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.dist_path = dist.string();
    anyar::App app(config);

    anyar::WindowConfig win;
    win.title = "Pinhole Win32 Test";
    win.width = 640;
    win.height = 420;
    app.create_window(win);

    std::shared_ptr<anyar::Pinhole> a, b, c;
    anyar::Window* window = nullptr;
    std::thread driver;

    app.on_window_ready([&](anyar::Window& w) {
        window = &w;
        g_hwnd = static_cast<HWND>(w.native_handle());

        a = solid(w, "A", 1.f, 0.f, 0.f);
        a->set_rect(40, 40, 200, 150);
        b = solid(w, "B", 0.f, 1.f, 0.f);
        b->set_z_index(1);
        b->set_rect(140, 90, 200, 150);
        anyar::PinholeOptions fb;
        fb.force_fallback = true;
        c = solid(w, "C", 0.f, 0.f, 1.f, fb);  // rect comes from the DOM tracker
        for (auto& p : {a, b, c}) p->request_redraw();

        driver = std::thread([&] {
            expect("A is native", a->is_native());
            expect("C (force_fallback) is not native", !c->is_native());

            expect_pixel("A visible (red)", 60, 60, kRed);
            expect_pixel("B over A in the overlap (green)", 180, 120, kGreen);
            expect_pixel("B alone (green)", 320, 220, kGreen);
            expect_pixel("C via canvas fallback (blue)", 480, 300, kBlue);
            expect_pixel("outside every pinhole is not red", 600, 30, kRed, false);

            a->set_z_index(2);
            expect_pixel("set_z_index(A, 2) puts A on top (red)", 180, 120, kRed);

            b->set_visible(false);
            expect_pixel("hidden B is gone", 320, 220, kGreen, false);
            b->set_visible(true);
            expect_pixel("B shown again", 320, 220, kGreen);

            // Move A (set_rect from a non-UI thread): old spot clears.
            a->set_rect(40, 250, 120, 100);
            expect_pixel("moved A: old spot no longer red", 60, 60, kRed, false);
            expect_pixel("moved A: new spot red", 60, 300, kRed);

            window->terminate();  // thread-safe; pinholes B and C still alive
        });
    });

    app.run();
    if (driver.joinable()) driver.join();
    b.reset();
    c.reset();

    std::error_code ec;
    fs::remove_all(dist, ec);
    std::cout << (g_failures == 0 ? "[PASS]" : "[FAIL]") << " pinhole win32 ("
              << g_failures << " failure(s))\n";
    return g_failures == 0 ? 0 : 1;
}
