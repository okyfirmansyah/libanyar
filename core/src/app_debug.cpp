// LibAnyar — debug tooling: console forwarding, debug:* automation commands,
// injected test scripts (see <anyar/debug.h> and docs/debugging.md).
//
// Platform-neutral.  The two native pieces (webview capture, synthetic input)
// go through platform::capture_webview / inject_webview_input, implemented in
// debug_linux.cpp and stubbed in debug_unsupported.cpp (Windows, macOS).

#include <anyar/app.h>
#include <anyar/main_thread.h>
#include <anyar/path.h>
#include <anyar/task.h>

#include "platform.h"

#include <boost/fiber/future.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace anyar {

namespace {

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0;
}

std::string base64(const std::vector<uint8_t>& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
        out += {t[n >> 18], t[(n >> 12) & 63], t[(n >> 6) & 63], t[n & 63]};
    }
    if (i + 1 == in.size()) {
        uint32_t n = uint32_t(in[i]) << 16;
        out += {t[n >> 18], t[(n >> 12) & 63], '=', '='};
    } else if (i + 2 == in.size()) {
        uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out += {t[n >> 18], t[(n >> 12) & 63], t[(n >> 6) & 63], '='};
    }
    return out;
}

const char* platform_name() {
#if defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

/// The window a debug command targets: `label`, else the caller, else main.
std::string target_label(const json& args) {
    if (args.contains("label") && args["label"].is_string()) return args["label"].get<std::string>();
    if (args.contains("_caller_label") && args["_caller_label"].is_string())
        return args["_caller_label"].get<std::string>();
    return "main";
}

// Mirrors console.* + uncaught errors + unhandled rejections to the backend.
// Uses the native binding directly (no bundle needed) and never throws.
const char* kConsoleForwardJs = R"JS(
(function () {
  if (window.__anyar_console_forward__ || typeof window.__anyar_ipc__ !== 'function') return;
  window.__anyar_console_forward__ = true;
  var seq = 0;
  function text(a) {
    if (a instanceof Error) {
      var head = a.name + ': ' + a.message, st = a.stack || '';
      return st.indexOf(head) === 0 ? st : head + (st ? '\n' + st : '');
    }
    if (typeof a === 'object' && a !== null) { try { return JSON.stringify(a); } catch (e) { return String(a); } }
    return String(a);
  }
  function send(level, args) {
    try {
      var parts = [];
      for (var i = 0; i < args.length; i++) parts.push(text(args[i]));
      var msg = parts.join(' ');
      if (msg.length > 4000) msg = msg.slice(0, 4000) + '…';
      var p = window.__anyar_ipc__(JSON.stringify({ id: 'console-' + (++seq), cmd: 'debug:console',
                                                    args: { level: level, message: msg } }));
      if (p && p.catch) p.catch(function () {});
    } catch (e) {}
  }
  ['log', 'info', 'warn', 'error', 'debug'].forEach(function (level) {
    var orig = console[level];
    console[level] = function () { send(level, arguments); return orig.apply(console, arguments); };
  });
  window.addEventListener('error', function (e) {
    var where = e.filename ? ' (' + e.filename + ':' + e.lineno + ':' + e.colno + ')' : '';
    send('error', ['Uncaught ' + (e.error ? text(e.error) : e.message) + where]);
  });
  window.addEventListener('unhandledrejection', function (e) {
    send('error', ['Unhandled rejection: ' + text(e.reason)]);
  });
})();
)JS";

// `window.anyarTest`: small helpers for test drivers and for poking at the
// app from DevTools.  Pointer helpers accept a CSS selector, an Element or
// {x, y} in CSS px.
const char* kTestPreludeJs = R"JS(
(function () {
  if (window.anyarTest || typeof window.__anyar_ipc__ !== 'function') return;
  var seq = 0;
  function invoke(cmd, args) {
    return Promise.resolve(window.__anyar_ipc__(JSON.stringify({ id: 'test-' + (++seq), cmd: cmd, args: args || {} })))
      .then(function (r) {
        if (r && r.error) throw new Error(r.error.message || String(r.error));
        return r ? r.data : undefined;
      });
  }
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
  async function waitFor(what, timeoutMs) {
    var t0 = Date.now(), limit = timeoutMs || 10000;
    for (;;) {
      var v = typeof what === 'string' ? document.querySelector(what) : what();
      if (v) return v;
      if (Date.now() - t0 > limit) throw new Error('waitFor timed out after ' + limit + ' ms: ' + String(what));
      await sleep(50);
    }
  }
  function point(target) {
    if (target && typeof target.x === 'number' && typeof target.y === 'number' && !(target instanceof Element)) return target;
    var el = typeof target === 'string' ? document.querySelector(target) : target;
    if (!el) throw new Error('no element: ' + target);
    var r = el.getBoundingClientRect();
    return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
  }
  function input(ev) { return invoke('debug:input', ev); }
  window.anyarTest = {
    label: window.__LIBANYAR_WINDOW_LABEL__ || 'main',
    invoke: invoke, sleep: sleep, waitFor: waitFor,
    click: function (t, opts) { var p = point(t); return input(Object.assign({ type: 'click', x: p.x, y: p.y }, opts || {})); },
    move: function (t) { var p = point(t); return input({ type: 'move', x: p.x, y: p.y }); },
    down: function (t, opts) { var p = point(t); return input(Object.assign({ type: 'down', x: p.x, y: p.y }, opts || {})); },
    up: function (t, opts) { var p = point(t); return input(Object.assign({ type: 'up', x: p.x, y: p.y }, opts || {})); },
    scroll: function (t, dy, dx) { var p = point(t); return input({ type: 'scroll', x: p.x, y: p.y, dy: dy || 1, dx: dx || 0 }); },
    key: function (key, opts) { return input(Object.assign({ type: 'key', key: key }, opts || {})); },
    type: function (text) { return input({ type: 'text', text: text }); },
    screenshot: function (path) { return invoke('debug:screenshot', path ? { path: path } : {}); },
    info: function () { return invoke('debug:info'); },
    log: function () { console.log.apply(console, arguments); },
    assert: function (cond, msg) { if (!cond) throw new Error('assertion failed' + (msg ? ': ' + msg : '')); },
    done: function (code) { return invoke('debug:exit', { code: code || 0 }); },
    fail: function (msg) { console.error('[anyarTest] FAIL: ' + msg); return invoke('debug:exit', { code: 1 }); }
  };
})();
)JS";

} // namespace

bool parse_input_type(const std::string& name, InputEvent::Type& out) {
    using T = InputEvent::Type;
    static const std::pair<const char*, T> names[] = {
        {"move", T::Move}, {"down", T::Down}, {"up", T::Up}, {"click", T::Click},
        {"scroll", T::Scroll}, {"key", T::Key}, {"text", T::Text},
    };
    for (const auto& [n, t] : names)
        if (name == n) {
            out = t;
            return true;
        }
    return false;
}

void App::apply_debug_env() {
    auto& d = config_.debug_tools;
    if (env_flag("ANYAR_CONSOLE")) d.console = true;
    if (env_flag("ANYAR_DEBUG_TOOLS")) d.automation = true;
    if (const char* s = std::getenv("ANYAR_TEST_SCRIPT"); s && *s) d.test_script = s;
    if (const char* s = std::getenv("ANYAR_TEST_TIMEOUT"); s && *s) d.test_timeout_s = std::atoi(s);

    if (!d.test_script.empty()) {
        std::ifstream in(path_from_utf8(d.test_script), std::ios::binary);
        if (!in) throw std::runtime_error("cannot read test script: " + d.test_script);
        test_script_source_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        d.console = d.automation = true;
    }
    if (d.console || d.automation) {
        std::cout << "[LibAnyar] debug tools:" << (d.console ? " console" : "")
                  << (d.automation ? " automation" : "")
                  << (d.test_script.empty() ? "" : " test-script=" + d.test_script) << std::endl;
    }
}

void App::inject_debug_scripts(Window* window) {
    const auto& d = config_.debug_tools;
    if (d.console) window->init(kConsoleForwardJs);
    if (d.automation) window->init(kTestPreludeJs);
    if (!test_script_source_.empty() && window->label() == "main") {
        // Run after `load`, as an async function so the script can use await.
        window->init("window.addEventListener('load', function () {\n"
                     "  (async function () {\n" + test_script_source_ + "\n  })().catch(function (e) {\n"
                     "    console.error('[anyarTest] uncaught:', e);\n"
                     "    window.anyarTest.done(1);\n"
                     "  });\n"
                     "});\n");
    }
}

void App::request_exit(int code) {
    exit_code_.store(code);
    close_all_requested_.store(true);
    if (Window* main_win = window_mgr_.main_window()) main_win->terminate();
}

void App::start_test_watchdog() {
    const int limit = config_.debug_tools.test_timeout_s;
    if (test_script_source_.empty() || limit <= 0) return;
    test_watchdog_ = std::thread([this, limit] {
        std::unique_lock<std::mutex> lock(watchdog_mu_);
        if (watchdog_cv_.wait_for(lock, std::chrono::seconds(limit), [this] { return watchdog_stop_; })) return;
        std::cerr << "[LibAnyar] test script timed out after " << limit << " s" << std::endl;
        // Same path as window:close-all, from a fiber (window_mgr_ is not thread-safe).
        service_->execute([this] { request_exit(124); });
    });
}

void App::stop_test_watchdog() {
    {
        std::lock_guard<std::mutex> lock(watchdog_mu_);
        watchdog_stop_ = true;
    }
    watchdog_cv_.notify_all();
    if (test_watchdog_.joinable()) test_watchdog_.join();
}

void App::register_debug_commands() {
    const auto& d = config_.debug_tools;

    if (d.console) {
        // debug:console {level, message} — mirrored page console output
        commands_.add("debug:console", [](const json& args) -> json {
            const std::string level = args.value("level", "log");
            const std::string label = target_label(args);
            std::ostream& os = (level == "error" || level == "warn") ? std::cerr : std::cout;
            os << "[js:" << label << "] " << level << ": " << args.value("message", "") << std::endl;
            return {{"ok", true}};
        });
    }
    if (!d.automation) return;

    // debug:info — which tools are available here
    commands_.add("debug:info", [this](const json&) -> json {
        const auto& c = config_.debug_tools;
        return {{"platform", platform_name()},
                {"console", c.console},
                {"automation", c.automation},
                {"test_script", c.test_script},
                {"capture", platform::can_capture_webview()},
                {"input", platform::can_inject_webview_input()}};
    });

    // debug:screenshot {label?, path?} → {width, height, bytes, path | png_base64}
    commands_.add("debug:screenshot", [this](const json& args) -> json {
        const std::string label = target_label(args);
        using Result = std::pair<WebviewSnapshot, std::string>;
        auto promise = std::make_shared<boost::fibers::promise<Result>>();
        auto future = promise->get_future();
        post_to_main_thread([this, label, promise] {
            Window* w = window_mgr_.get(label);
            if (!w || w->is_destroyed()) {
                promise->set_value({{}, "no window '" + label + "'"});
                return;
            }
            w->capture([promise](WebviewSnapshot s, std::string err) {
                promise->set_value({std::move(s), std::move(err)});
            });
        });
        if (future.wait_for(std::chrono::seconds(15)) != boost::fibers::future_status::ready)
            throw std::runtime_error("screenshot timed out");
        auto [snap, err] = future.get();
        if (!err.empty()) throw std::runtime_error(err);

        json out = {{"width", snap.width}, {"height", snap.height}, {"bytes", snap.png.size()}};
        if (args.contains("path") && args["path"].is_string()) {
            const auto path = path_from_utf8(args["path"].get<std::string>());
            run_blocking(service_, [&] {
                std::error_code ec;
                if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                if (!f) throw std::runtime_error("cannot write " + path_to_utf8(path));
                f.write(reinterpret_cast<const char*>(snap.png.data()), std::streamsize(snap.png.size()));
            });
            out["path"] = path_to_utf8(std::filesystem::absolute(path));
        } else {
            out["png_base64"] = base64(snap.png);
        }
        return out;
    });

    // debug:input {label?, type, x, y, button?, dx?, dy?, key?, text?, shift?, ctrl?, alt?, meta?}
    commands_.add("debug:input", [this](const json& args) -> json {
        InputEvent ev;
        if (!parse_input_type(args.value("type", "click"), ev.type))
            throw std::invalid_argument("unknown input type: " + args.value("type", ""));
        ev.x = args.value("x", 0.0);
        ev.y = args.value("y", 0.0);
        ev.button = args.value("button", 1);
        ev.dx = args.value("dx", 0.0);
        ev.dy = args.value("dy", 0.0);
        ev.key = args.value("key", "");
        ev.text = args.value("text", "");
        ev.shift = args.value("shift", false);
        ev.ctrl = args.value("ctrl", false);
        ev.alt = args.value("alt", false);
        ev.meta = args.value("meta", false);
        const std::string label = target_label(args);
        std::string err = run_on_main_thread([this, label, ev]() -> std::string {
            Window* w = window_mgr_.get(label);
            if (!w || w->is_destroyed()) return "no window '" + label + "'";
            return w->inject_input(ev);
        });
        if (!err.empty()) throw std::runtime_error(err);
        return {{"ok", true}};
    });

    // debug:exit {code?} — close every window; App::run() returns `code`
    commands_.add("debug:exit", [this](const json& args) -> json {
        request_exit(args.value("code", 0));
        return {{"ok", true}};
    });
}

} // namespace anyar
