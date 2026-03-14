// LibAnyar — WebGL Canvas E2E Test
//
// Creates a shared buffer with known RGBA pixel data on the C++ side,
// opens a webview window that:
//   1) Fetches the buffer via anyar-shm://
//   2) Uploads it to a WebGL texture
//   3) Renders a fullscreen quad
//   4) Reads pixels back with gl.readPixels()
//   5) Reports results to C++ via IPC
//
// Exit code 0 = all assertions passed, non-zero = failure.

#include <anyar/app.h>
#include <anyar/shared_buffer.h>

#include <cstring>
#include <iostream>
#include <atomic>
#include <chrono>

using json = anyar::json;

int main() {
    // ── Test parameters ────────────────────────────────────────────────────
    constexpr int W = 4;       // 4×4 pixels (tiny — enough for verification)
    constexpr int H = 4;
    constexpr size_t BUF_SIZE = W * H * 4;  // RGBA

    // ── Configure the app ──────────────────────────────────────────────────
    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.debug = false;
    config.dist_path = "./dist-webgl-test";

    anyar::App app(config);

    // ── Track test result ──────────────────────────────────────────────────
    std::atomic<int> exit_code{1};  // 1 = fail (overwritten on success)
    std::string failure_reason = "Test did not complete (timeout?)";

    // ── Register test:report command — JS calls this with results ────────
    app.command("test:report", [&](const json& args) -> json {
        bool passed = args.value("passed", false);
        std::string message = args.value("message", "");
        int num_checks = args.value("checks", 0);

        if (passed) {
            std::cout << "[PASS] WebGL canvas E2E — " << num_checks
                      << " pixel checks passed" << std::endl;
            if (!message.empty()) {
                std::cout << "       " << message << std::endl;
            }
            exit_code.store(0);
        } else {
            std::cout << "[FAIL] WebGL canvas E2E — " << message << std::endl;
            failure_reason = message;
            exit_code.store(1);
        }

        // Close the window after reporting
        return json{{"ok", true}};
    });

    // ── When the window is created, fill the shared buffer ──────────────
    app.on("window:created", [&](const json& payload) {
        auto label = payload.value("label", "");
        if (label != "main") return;

        // Create the shared buffer and fill with known pattern
        auto buf = anyar::SharedBuffer::create("test-frame", BUF_SIZE);
        if (!buf) {
            std::cerr << "[FAIL] Could not create shared buffer" << std::endl;
            return;
        }

        // Fill with a recognizable pattern:
        //   Row 0: Red     (255,   0,   0, 255) × 4
        //   Row 1: Green   (  0, 255,   0, 255) × 4
        //   Row 2: Blue    (  0,   0, 255, 255) × 4
        //   Row 3: White   (255, 255, 255, 255) × 4
        uint8_t* d = buf->data();
        for (int x = 0; x < W; ++x) {
            int i = (0 * W + x) * 4;
            d[i] = 255; d[i+1] = 0; d[i+2] = 0; d[i+3] = 255;  // Red
        }
        for (int x = 0; x < W; ++x) {
            int i = (1 * W + x) * 4;
            d[i] = 0; d[i+1] = 255; d[i+2] = 0; d[i+3] = 255;  // Green
        }
        for (int x = 0; x < W; ++x) {
            int i = (2 * W + x) * 4;
            d[i] = 0; d[i+1] = 0; d[i+2] = 255; d[i+3] = 255;  // Blue
        }
        for (int x = 0; x < W; ++x) {
            int i = (3 * W + x) * 4;
            d[i] = 255; d[i+1] = 255; d[i+2] = 255; d[i+3] = 255;  // White
        }

        std::cout << "[test] Shared buffer 'test-frame' created ("
                  << BUF_SIZE << " bytes, " << W << "×" << H << " RGBA)"
                  << std::endl;
    });

    // ── Create window ──────────────────────────────────────────────────────
    anyar::WindowConfig win;
    win.title = "WebGL Canvas Test";
    win.width = 200;
    win.height = 200;
    win.resizable = false;
    win.debug = false;

    app.create_window(win);

    std::cout << "[test] Starting WebGL canvas E2E test..." << std::endl;

    // ── Run (blocks until window closes) ────────────────────────────────
    app.run();

    // Clean up shared buffer AFTER app has fully shut down
    // (the anyar-shm:// URI handler may still reference buffer data during
    // WebKitGTK teardown if we clear too early)
    anyar::SharedBufferRegistry::instance().clear();

    if (exit_code.load() != 0) {
        std::cerr << "[FAIL] " << failure_reason << std::endl;
    }

    return exit_code.load();
}
