/// @file main.cpp — pinhole-hello example
///
/// Demonstrates the Pinhole native overlay API:
///   - Creates a Window and calls window->create_pinhole()
///   - Draws a time-animated solid colour rectangle using ctx.clear()
///   - Also demonstrates draw_image() by uploading a small RGBA bitmap
///   - Uses set_continuous(true) so the overlay renders at vsync rate
///
/// The HTML side shows a transparent placeholder div at the same CSS position.
/// On Linux, a native GtkGLArea is positioned over that div.

#include <anyar/app.h>
#include <anyar/pinhole.h>
#include <anyar/window.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using json = anyar::json;

int main() {
    // ── App configuration ────────────────────────────────────────────────
    anyar::AppConfig config;
    config.host      = "127.0.0.1";
    config.port      = 0;
    config.debug     = true;
    config.dist_path = "./dist";

    anyar::App app(config);

    // ── Create the application window via App ────────────────────────────
    // app.create_window() stores the config; the actual Window object is
    // created inside app.run().  Use app.on_window_ready() to access the
    // Window and call create_pinhole() once it exists.
    anyar::WindowConfig win;
    win.title     = "Pinhole Hello";
    win.width     = 800;
    win.height    = 600;
    win.resizable = true;
    win.debug     = config.debug;

    app.create_window(win);

    // ── Position: centred-ish in the 800×600 window ──────────────────────
    constexpr int PX = 200, PY = 150, PW = 400, PH = 300;

    // ── Build a small static 8×8 RGBA checkerboard for draw_image demo ──
    constexpr int CHECKER_SIZE = 8;
    std::vector<uint8_t> checker(CHECKER_SIZE * CHECKER_SIZE * 4);
    for (int y = 0; y < CHECKER_SIZE; ++y) {
        for (int x = 0; x < CHECKER_SIZE; ++x) {
            bool white = ((x + y) % 2 == 0);
            size_t off = (y * CHECKER_SIZE + x) * 4;
            checker[off + 0] = white ? 0xff : 0x33;  // R
            checker[off + 1] = white ? 0xff : 0x33;  // G
            checker[off + 2] = white ? 0xff : 0x33;  // B
            checker[off + 3] = 0xcc;                 // A (semi-transparent)
        }
    }

    // ── Shared pinhole handle — populated inside on_window_ready ─────────
    std::shared_ptr<anyar::Pinhole> pin;

    // ── on_window_ready: window exists; GTK loop not yet running ─────────
    // This is the correct place to call create_pinhole().
    app.on_window_ready([&](anyar::Window& window) {
        anyar::PinholeOptions pin_opts;
        pin_opts.continuous = true;  // vsync render loop

        pin = window.create_pinhole("animated-rect", pin_opts);
        pin->set_rect(PX, PY, PW, PH);

        std::cout << "[pinhole-hello] Pinhole is_native=" << pin->is_native()
                  << " id=" << pin->id() << "\n";

        // ── Render callback ───────────────────────────────────────────────
        auto start = std::chrono::steady_clock::now();

        pin->on_render([&checker, start](anyar::PinholeRenderContext& ctx) {
            auto now = std::chrono::steady_clock::now();
            float t  = std::chrono::duration<float>(now - start).count();

            // Animate hue: cycle through red → green → blue → red
            float r = 0.5f + 0.5f * std::sin(t * 1.0f);
            float g = 0.5f + 0.5f * std::sin(t * 1.0f + 2.094f);  // 2π/3
            float b = 0.5f + 0.5f * std::sin(t * 1.0f + 4.189f);  // 4π/3

            // Every ~2s alternate between solid colour and checkerboard texture
            bool use_texture = static_cast<int>(t / 2.0f) % 2 == 1;

            if (use_texture) {
                ctx.draw_image(checker.data(), checker.size(),
                               CHECKER_SIZE, CHECKER_SIZE,
                               anyar::pixel_format::rgba);
            } else {
                ctx.clear(r, g, b, 0.85f);
            }
        });
    });

    // ── Register IPC command so JS can query pinhole state ───────────────
    // `pin` will be populated by the time any IPC command fires (GTK loop
    // starts only after on_window_ready returns).
    app.command("pinhole:info", [&pin](const json&) -> json {
        return {
            {"id",        pin ? pin->id() : std::string("(not ready)")},
            {"is_native", pin ? pin->is_native() : false},
        };
    });

    // ── Show window and run ──────────────────────────────────────────────
    app.run();

    return 0;
}
