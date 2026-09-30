// LibAnyar — Performance Benchmark Harness
//
// Measures, in one real webview session:
//   1) Startup      — main() → window created → frontend script running → first IPC
//   2) IPC latency  — native (`__anyar_ipc__`) and HTTP fallback round-trips
//   3) SharedBuffer — 1080p RGBA fetch via `anyar-shm://` and HTTP fallback
//   4) Pinhole      — on_render CPU time for a 1080p RGBA draw_image + frame interval
//   5) Memory       — RSS of this process + WebKit child processes (Linux /proc)
//
// The frontend (dist/index.html) drives steps 2–3 and reports back via IPC.
// Results are printed as a table and as one JSON line prefixed `BENCH_RESULT `.
//
// Usage: ./anyar_bench [--json out.json]   (build with CMAKE_BUILD_TYPE=Release)

#include <anyar/app.h>
#include <anyar/pinhole.h>
#include <anyar/shared_buffer.h>
#include <anyar/window.h>

#include <webkit2/webkit2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using json = anyar::json;
using clock_type = std::chrono::steady_clock;

namespace {

const auto t_main = clock_type::now();

double ms_since_main() {
    return std::chrono::duration<double, std::milli>(clock_type::now() - t_main).count();
}

json summarize(std::vector<double> v) {
    if (v.empty()) return json{{"n", 0}};
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[std::min(v.size() - 1, size_t(p * v.size()))]; };
    double sum = 0;
    for (double x : v) sum += x;
    return {{"n", v.size()}, {"mean", sum / v.size()}, {"p50", pct(0.50)},
            {"p95", pct(0.95)}, {"p99", pct(0.99)}, {"max", v.back()}};
}

// ── Memory: RSS of this process and all descendants (WebKit web/network procs)
long rss_kb(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) return std::atol(line.c_str() + 6);
    return 0;
}

json memory_snapshot() {
    std::map<int, std::pair<int, std::string>> procs;  // pid → {ppid, comm}
    if (DIR* d = opendir("/proc")) {
        while (dirent* e = readdir(d)) {
            int pid = std::atoi(e->d_name);
            if (pid <= 0) continue;
            std::ifstream f(std::string("/proc/") + e->d_name + "/stat");
            std::string s;
            if (!std::getline(f, s)) continue;
            auto l = s.find('('), r = s.rfind(')');
            if (l == std::string::npos || r == std::string::npos) continue;
            std::istringstream rest(s.substr(r + 2));
            char state; int ppid;
            rest >> state >> ppid;
            procs[pid] = {ppid, s.substr(l + 1, r - l - 1)};
        }
        closedir(d);
    }
    int self = getpid();
    json children = json::array();
    long total = rss_kb(self);
    std::vector<int> frontier{self};
    while (!frontier.empty()) {
        int parent = frontier.back();
        frontier.pop_back();
        for (auto& [pid, info] : procs) {
            if (info.first != parent) continue;
            long kb = rss_kb(pid);
            total += kb;
            children.push_back({{"pid", pid}, {"name", info.second}, {"rss_mb", kb / 1024.0}});
            frontier.push_back(pid);
        }
    }
    return {{"self_rss_mb", rss_kb(self) / 1024.0},
            {"total_rss_mb", total / 1024.0},
            {"children", children}};
}

}  // namespace

int main(int argc, char** argv) {
    std::string json_out;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--json" && i + 1 < argc) json_out = argv[++i];

    constexpr int FRAME_W = 1920, FRAME_H = 1080;
    constexpr size_t FRAME_BYTES = size_t(FRAME_W) * FRAME_H * 4;
    constexpr int PINHOLE_FRAMES = 240;

    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.debug = false;
    config.dist_path = "./dist-bench";

    anyar::App app(config);

    // PROTOTYPE: load the zero-copy shm WebProcess extension (webext/ next to
    // the binary) unless --no-webext.  Must happen before any webview exists.
    bool use_webext = true;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--no-webext") use_webext = false;
    if (use_webext) {
        char exe[4096] = {};
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        std::string dir = n > 0 ? std::string(exe, size_t(n)) : std::string("./anyar_bench");
        dir = dir.substr(0, dir.rfind('/')) + "/webext";
        WebKitWebContext* wctx = webkit_web_context_get_default();
        webkit_web_context_set_web_extensions_directory(wctx, dir.c_str());
        webkit_web_context_set_web_extensions_initialization_user_data(
            wctx, g_variant_new_int32(getpid()));
    }

    json result;
    std::mutex result_mtx;
    std::atomic<int> exit_code{1};

    // ── Startup milestones ───────────────────────────────────────────────
    app.on("window:created", [&](const json& payload) {
        if (payload.value("label", "") != "main") return;
        std::lock_guard<std::mutex> lk(result_mtx);
        result["startup"]["window_created_ms"] = ms_since_main();
    });

    app.command("bench:ready", [&](const json& args) -> json {
        std::lock_guard<std::mutex> lk(result_mtx);
        result["startup"]["first_ipc_ms"] = ms_since_main();
        result["startup"]["js_nav_timing"] = args;
        return json{{"ok", true}};
    });

    // ── IPC echo target ──────────────────────────────────────────────────
    app.command("bench:echo", [](const json& args) -> json { return args; });

    // ── SharedBuffer: 1080p RGBA frame ───────────────────────────────────
    auto frame = anyar::SharedBuffer::create("bench-frame", FRAME_BYTES);
    if (!frame) {
        std::cerr << "[bench] SharedBuffer::create failed" << std::endl;
        return 1;
    }
    for (size_t i = 0; i < FRAME_BYTES; ++i) frame->data()[i] = uint8_t(i * 31);

    // Freshness check for the webext path: JS asks C++ to change one byte and
    // verifies the next read (through the cached mapping) sees it.
    app.command("bench:poke", [&](const json& args) -> json {
        size_t off = args.value("offset", 0);
        frame->data()[off] = uint8_t(args.value("value", 0));
        return json{{"ok", true}};
    });

    // Size sweep buffers — separates per-request cost from per-byte cost.
    std::vector<std::shared_ptr<anyar::SharedBuffer>> sweep;
    for (size_t kb : {64, 1024, 32400}) {  // 64 KB, 1 MB, ~4K RGBA (33 MB)
        auto b = anyar::SharedBuffer::create("bench-" + std::to_string(kb) + "k", kb * 1024);
        if (b) sweep.push_back(b);
    }

    // ── Pinhole: time on_render for a full-frame draw_image ─────────────
    std::shared_ptr<anyar::Pinhole> pin;
    std::mutex pin_mtx;
    std::vector<double> render_ms, interval_ms;
    std::atomic<bool> pin_recording{false};
    std::atomic<int> total_renders{0};
    clock_type::time_point last_frame{};

    app.on_window_ready([&](anyar::Window& window) {
        {
            std::lock_guard<std::mutex> lk(result_mtx);
            result["startup"]["window_ready_ms"] = ms_since_main();
        }
        pin = window.create_pinhole("bench");
        pin->set_rect(20, 300, 640, 360);
        pin->on_render([&](anyar::PinholeRenderContext& ctx) {
            total_renders.fetch_add(1);
            if (!pin_recording.load()) {
                ctx.clear(0.1f, 0.1f, 0.1f);
                return;
            }
            auto t0 = clock_type::now();
            ctx.draw_image(frame->data(), FRAME_BYTES, FRAME_W, FRAME_H,
                           anyar::pixel_format::rgba);
            auto t1 = clock_type::now();
            bool more;
            {
                std::lock_guard<std::mutex> lk(pin_mtx);
                render_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                if (last_frame != clock_type::time_point{})
                    interval_ms.push_back(
                        std::chrono::duration<double, std::milli>(t0 - last_frame).count());
                last_frame = t0;
                more = render_ms.size() < size_t(PINHOLE_FRAMES);
            }
            if (more) pin->request_redraw();
            else pin_recording.store(false);
        });
    });

    app.command("bench:pinhole_start", [&](const json&) -> json {
        {
            std::lock_guard<std::mutex> lk(pin_mtx);
            render_ms.clear();
            interval_ms.clear();
            last_frame = {};
        }
        pin_recording.store(true);
        pin->request_redraw();
        return json{{"frames", PINHOLE_FRAMES}};
    });

    app.command("bench:pinhole_stats", [&](const json&) -> json {
        std::lock_guard<std::mutex> lk(pin_mtx);
        return {{"is_native", pin->is_native()},
                {"done", !pin_recording.load()},
                {"total_renders", total_renders.load()},
                {"frame", std::to_string(FRAME_W) + "x" + std::to_string(FRAME_H) + " rgba"},
                {"render_cpu_ms", summarize(render_ms)},
                {"frame_interval_ms", summarize(interval_ms)}};
    });

    app.command("bench:memory", [](const json&) -> json { return memory_snapshot(); });

    // ── Final report from JS ─────────────────────────────────────────────
    app.command("bench:report", [&](const json& args) -> json {
        {
            std::lock_guard<std::mutex> lk(result_mtx);
            for (auto& [k, v] : args.items()) result[k] = v;
        }
        exit_code.store(args.contains("error") ? 1 : 0);
        // Safety net only: if teardown hangs, still exit with the result.
        std::thread([&exit_code]() {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            std::cerr << "[bench] watchdog: teardown exceeded 10s" << std::endl;
            _exit(exit_code.load());
        }).detach();
        return json{{"ok", true}};
    });

    anyar::WindowConfig win;
    win.title = "LibAnyar Benchmark";
    win.width = 700;
    win.height = 700;
    win.debug = false;
    app.create_window(win);

    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        std::cerr << "[bench] global timeout (120s) — frontend never reported" << std::endl;
        _exit(2);
    }).detach();

    result["startup"]["before_run_ms"] = ms_since_main();
    app.run();
    pin.reset();
    frame.reset();
    sweep.clear();
    anyar::SharedBufferRegistry::instance().clear();

    result["meta"] = {{"frame_bytes", FRAME_BYTES}};
    std::cout << "BENCH_RESULT " << result.dump() << std::endl;
    if (!json_out.empty()) std::ofstream(json_out) << result.dump(2) << std::endl;

    // ── Human-readable summary ───────────────────────────────────────────
    auto get = [&](std::initializer_list<const char*> path) -> std::string {
        const json* j = &result;
        for (auto k : path) {
            if (!j->is_object() || !j->contains(k)) return "n/a";
            j = &(*j)[k];
        }
        if (j->is_number()) {
            std::ostringstream o;
            o << std::fixed << std::setprecision(3) << j->get<double>();
            return o.str();
        }
        return j->dump();
    };
    std::cout << "\n=== LibAnyar benchmark ===\n"
              << "startup: before run()    " << get({"startup", "before_run_ms"}) << " ms\n"
              << "startup: window ready    " << get({"startup", "window_ready_ms"}) << " ms\n"
              << "startup: window created  " << get({"startup", "window_created_ms"}) << " ms\n"
              << "startup: first IPC       " << get({"startup", "first_ipc_ms"}) << " ms\n"
              << "IPC native small p50/p99 " << get({"ipc", "native_small", "p50"}) << " / "
              << get({"ipc", "native_small", "p99"}) << " ms\n"
              << "IPC native 64KB  p50/p99 " << get({"ipc", "native_64k", "p50"}) << " / "
              << get({"ipc", "native_64k", "p99"}) << " ms\n"
              << "IPC HTTP small   p50/p99 " << get({"ipc", "http_small", "p50"}) << " / "
              << get({"ipc", "http_small", "p99"}) << " ms\n"
              << "SharedBuffer shm 1080p p50  " << get({"buffer", "shm", "p50"}) << " ms\n"
              << "SharedBuffer HTTP 1080p p50 " << get({"buffer", "http", "p50"}) << " ms\n"
              << "SharedBuffer webext 1080p p50 " << get({"buffer", "webext_read", "p50"}) << " ms\n"
              << "SharedBuffer webext 33MB p50  " << get({"buffer", "webext_read_33m", "p50"}) << " ms\n"
              << "SharedBuffer webext check    " << get({"buffer", "webext_check"}) << "\n"
              << "Pinhole native           " << get({"pinhole", "is_native"}) << "\n"
              << "Pinhole render CPU p50/p95  " << get({"pinhole", "render_cpu_ms", "p50"})
              << " / " << get({"pinhole", "render_cpu_ms", "p95"}) << " ms\n"
              << "Pinhole frame interval p50  " << get({"pinhole", "frame_interval_ms", "p50"})
              << " ms\n"
              << "Memory total RSS         " << get({"memory", "total_rss_mb"}) << " MB (self "
              << get({"memory", "self_rss_mb"}) << " MB)\n";
    if (result.contains("error")) std::cout << "ERROR: " << result["error"].dump() << "\n";
    return exit_code.load();
}
