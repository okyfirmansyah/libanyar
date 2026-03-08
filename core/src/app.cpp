#include <anyar/app.h>

#include <anyar/plugins/fs_plugin.h>
#include <anyar/plugins/dialog_plugin.h>
#include <anyar/plugins/shell_plugin.h>
#include <anyar/plugins/clipboard_plugin.h>
#include <anyar/plugins/db_plugin.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>

namespace anyar {

// ── Platform-Specific Initialization ────────────────────────────────────────

#ifdef __linux__
// Snap Environment Sanitisation (Linux-only)
//
// When the process is launched from a snap-confined host (e.g. VS Code snap),
// several GTK/GLib environment variables point into the snap's private
// library tree.  WebKitGTK spawns auxiliary processes (WebKitWebProcess,
// WebKitNetworkProcess) that inherit these variables, causing them to load
// the snap's incompatible glibc/libpthread and crash immediately with:
//
//   symbol lookup error: .../libpthread.so.0: undefined symbol:
//   __libc_pthread_init, version GLIBC_PRIVATE
//
// We remove the offending variables early, before any GTK/WebKit code runs.

static void platform_init() {
    static const char* snap_gtk_vars[] = {
        "GTK_EXE_PREFIX",
        "GTK_PATH",
        "GTK_IM_MODULE_FILE",
        "GIO_MODULE_DIR",
        "LOCPATH",
        "GSETTINGS_SCHEMA_DIR",
        nullptr
    };
    for (const char** v = snap_gtk_vars; *v; ++v) {
        const char* val = std::getenv(*v);
        if (val && std::string(val).find("/snap/") != std::string::npos) {
            ::unsetenv(*v);
        }
    }
}
#else
// No-op on other platforms (Windows/macOS init will go here in Phase 7)
static void platform_init() {}
#endif

App::App() : App(AppConfig{}) {}

App::App(AppConfig config) : config_(std::move(config)) {
    // Platform-specific initialization (snap env cleanup on Linux, etc.)
    platform_init();

    // Default dist path
    if (config_.dist_path.empty()) {
        config_.dist_path = "./dist";
    }
}

App::~App() {
    // Ensure service thread is joined
    if (service_thread_.joinable()) {
        if (service_) {
            service_->stop();
        }
        service_thread_.join();
    }
}

// ── Command Registration ────────────────────────────────────────────────────

void App::command(const std::string& name, CommandHandler handler) {
    commands_.add(name, std::move(handler));
}

void App::command_async(const std::string& name, AsyncCommandHandler handler) {
    commands_.add_async(name, std::move(handler));
}

// ── Event System ────────────────────────────────────────────────────────────

void App::emit(const std::string& event, const json& payload) {
    events_.emit(event, payload);
}

UnsubscribeFn App::on(const std::string& event, EventHandler handler) {
    return events_.on(event, std::move(handler));
}

// ── Window Management ───────────────────────────────────────────────────────

void App::create_window(WindowConfig config) {
    window_config_ = std::move(config);
    window_config_.debug = window_config_.debug || config_.debug;
    has_window_ = true;
}

// ── Plugin System ───────────────────────────────────────────────────────────

void App::use(std::shared_ptr<IAnyarPlugin> plugin) {
    plugins_.push_back(std::move(plugin));
}

// ── Internal ────────────────────────────────────────────────────────────────

int App::find_available_port() {
    if (config_.port > 0) {
        return config_.port;
    }
    // Pick a random port in the ephemeral range
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(49152, 65535);
    return dist(gen);
}

void App::start_server() {
    port_ = find_available_port();

    service_ = asyik::make_service();
    server_ = asyik::make_http_server(service_, config_.host, static_cast<uint16_t>(port_));

    // ── IPC Router (commands + events) — must be registered before catch-all ──
    ipc_router_ = std::make_unique<IpcRouter>(commands_, events_);
    ipc_router_->setup(server_);

    // ── Register built-in commands ──────────────────────────────────────────
    commands_.add("anyar:ping", [](const json& args) -> json {
        return {{"pong", true}};
    });

    commands_.add("anyar:version", [](const json& args) -> json {
        return {{"version", "0.1.0"}, {"framework", "LibAnyar"}};
    });

    // ── JS→C++ event forwarding (used by native IPC emit) ───────────────
    commands_.add("anyar:emit_event", [this](const json& args) -> json {
        std::string event = args.at("event").get<std::string>();
        json payload = args.value("payload", json::object());
        events_.emit_local(event, payload);   // C++ subscribers only, no echo
        return nullptr;
    });

    // ── Auto-register built-in plugins ─────────────────────────────────────
    std::vector<std::shared_ptr<IAnyarPlugin>> builtins;
    builtins.push_back(std::make_shared<FsPlugin>());
    builtins.push_back(std::make_shared<DialogPlugin>());
    builtins.push_back(std::make_shared<ShellPlugin>());
    builtins.push_back(std::make_shared<ClipboardPlugin>());
    builtins.push_back(std::make_shared<DbPlugin>());

    // ── Initialize plugins BEFORE serve_static ─────────────────────────────
    // Plugin routes (e.g. /video/stream) must be registered before the
    // catch-all static-file handler, because libasyik uses first-match
    // routing.  If serve_static("/") is registered first it shadows every
    // plugin route and the request gets served as a static file (or 404),
    // which can crash GStreamer/WebKitGTK when a <video> element receives
    // HTML instead of a media stream.
    PluginContext ctx{service_, server_, commands_, events_, config_};
    for (auto& plugin : builtins) {
        plugin->initialize(ctx);
    }
    for (auto& plugin : plugins_) {
        plugin->initialize(ctx);
    }

    // Keep builtins alive for shutdown
    plugins_.insert(plugins_.begin(), builtins.begin(), builtins.end());

    // ── Static file serving (frontend assets) — uses LibAsyik serve_static ──
    // Registered AFTER plugin routes so that specific routes take precedence
    // over the catch-all static file handler.
    std::string dist_abs = std::filesystem::absolute(config_.dist_path).string();
    if (std::filesystem::exists(dist_abs)) {
        asyik::static_file_config cfg;
        cfg.cache_control = "no-cache";          // dev-friendly default
        cfg.index_file    = "index.html";         // SPA entry point
        server_->serve_static("/", dist_abs, cfg);
    } else {
        // If no dist folder, serve a fallback page
        server_->on_http_request("/", "GET",
            [](asyik::http_request_ptr req, asyik::http_route_args args) {
                req->response.body =
                    "<!DOCTYPE html><html><head><title>LibAnyar</title></head>"
                    "<body style='font-family:system-ui;padding:40px'>"
                    "<h1>LibAnyar</h1>"
                    "<p>No frontend build found. Place your built frontend in <code>./dist</code>.</p>"
                    "</body></html>";
                req->response.headers.set("Content-Type", "text/html");
                req->response.result(200);
            }
        );
    }
}

// ── Run ─────────────────────────────────────────────────────────────────────

int App::run() {
    start_server();

    std::cout << "[LibAnyar] Server listening on http://"
              << config_.host << ":" << port_ << "/" << std::endl;

    // Start LibAsyik service on a separate thread
    service_thread_ = std::thread([this]() {
        service_->run();
    });

    if (has_window_) {
        // Create and run the window on the main thread (OS requirement)
        window_ = std::make_unique<Window>(window_config_, port_);

        // Set up native IPC (webview_bind) before the event loop starts
        setup_native_ipc();

        window_->run();  // Blocks until window is closed

        // Window closed — stop service
        if (service_) {
            service_->stop();
        }
    } else {
        // No window — run headless (useful for testing)
        service_thread_.join();
        return 0;
    }

    if (service_thread_.joinable()) {
        service_thread_.join();
    }

    // Shutdown plugins
    for (auto& plugin : plugins_) {
        plugin->shutdown();
    }

    // Clean up native event sink
    if (native_event_sink_id_) {
        events_.remove_ws_sink(native_event_sink_id_);
    }

    return 0;
}

// ── Native IPC Setup ────────────────────────────────────────────────────────
//
// Binds `window.__anyar_ipc__(json)` → CommandRegistry::dispatch.
// Also registers a native event push sink so that C++ events reach the
// webview via webview_eval() instead of WebSocket.

void App::setup_native_ipc() {
    if (!window_) return;

    // ── Bind __anyar_ipc__ for direct command invocation ─────────────────
    window_->bind("__anyar_ipc__",
        [this](const std::string& seq, const std::string& req) {
            // webview_bind passes args as a JSON array: ["<stringified_body>"]
            // This callback runs on the GTK main thread.  Dispatch the
            // real work to a libasyik fiber so that command handlers can
            // use run_on_gtk_main() without deadlocking.
            std::string seq_copy = seq;
            std::string req_copy = req;

            service_->execute([this, seq_copy, req_copy]() {
                std::string result;
                try {
                    auto args_arr = json::parse(req_copy);
                    std::string payload_str = args_arr[0].get<std::string>();
                    auto body = json::parse(payload_str);

                    IpcRequest ipc_req;
                    ipc_req.id  = body.value("id", "");
                    ipc_req.cmd = body.at("cmd").get<std::string>();
                    ipc_req.args = body.value("args", json::object());

                    auto resp = commands_.dispatch(ipc_req);
                    result = resp.to_json().dump();
                } catch (const std::exception& e) {
                    IpcResponse err_resp;
                    err_resp.error = std::string("Native IPC error: ") + e.what();
                    result = err_resp.to_json().dump();
                }

                // Resolve the JS promise on the GTK main thread
                window_->dispatch([this, seq_copy, result]() {
                    window_->return_result(seq_copy, 0, result);
                });
            });
        }
    );

    // ── Register a native event push sink ────────────────────────────────
    // When C++ emits an event, push it directly into the webview via eval()
    // instead of going through a WebSocket connection.
    native_event_sink_id_ = events_.add_ws_sink(
        [this](const std::string& message) {
            if (window_) {
                window_->dispatch([this, message]() {
                    window_->eval(
                        "window.__anyar_dispatch_event__&&"
                        "window.__anyar_dispatch_event__(" + message + ")");
                });
            }
        }
    );

    // ── Inject the JS-side event dispatcher (runs before window.onload) ─
    window_->init(R"JS(
        window.__LIBANYAR_NATIVE__ = true;
        window.__anyar_event_listeners__ = {};
        window.__anyar_dispatch_event__ = function(msg) {
            if (!msg || !msg.event) return;
            var ls = window.__anyar_event_listeners__[msg.event] || [];
            for (var i = 0; i < ls.length; i++) {
                try { ls[i](msg.payload); } catch(e) { console.error('[LibAnyar] event error:', e); }
            }
            var ws = window.__anyar_event_listeners__['*'] || [];
            for (var i = 0; i < ws.length; i++) {
                try { ws[i](msg.payload); } catch(e) { console.error('[LibAnyar] event error:', e); }
            }
        };
    )JS");

    // ── Native-app behavior: disable browser defaults in production ────
    //
    // Per-component pinch-zoom: Add data-pinch-zoom attribute to any
    // element that should receive pinch/zoom events.  The global
    // blockers below will skip those elements so they can handle
    // wheel(ctrlKey) / touch gestures themselves.
    //
    if (!window_config_.debug) {
        window_->init(R"JS(
            // Helper: check if an element or ancestor has data-pinch-zoom
            function __anyar_allows_zoom(el) {
                while (el && el !== document) {
                    if (el.hasAttribute && el.hasAttribute('data-pinch-zoom')) return true;
                    el = el.parentNode;
                }
                return false;
            }

            // Disable browser context menu (right-click)
            document.addEventListener('contextmenu', function(e) {
                e.preventDefault();
            });

            // Prevent Ctrl+wheel zoom UNLESS inside data-pinch-zoom element
            document.addEventListener('wheel', function(e) {
                if (e.ctrlKey && !__anyar_allows_zoom(e.target)) {
                    e.preventDefault();
                }
            }, { passive: false, capture: true });

            // Prevent keyboard zoom (Ctrl +/-/0)
            document.addEventListener('keydown', function(e) {
                if ((e.ctrlKey || e.metaKey) &&
                    (e.key === '+' || e.key === '-' || e.key === '=' || e.key === '0')) {
                    if (!__anyar_allows_zoom(document.activeElement)) {
                        e.preventDefault();
                    }
                }
            }, { capture: true });

            // Prevent multi-touch zoom UNLESS inside data-pinch-zoom element
            document.addEventListener('touchstart', function(e) {
                if (e.touches.length > 1 && !__anyar_allows_zoom(e.target)) {
                    e.preventDefault();
                }
            }, { passive: false });
        )JS");
    }

    if (config_.debug) {
        std::cout << "[LibAnyar] Native IPC bound (__anyar_ipc__ + event push)" << std::endl;
    }
}

} // namespace anyar
