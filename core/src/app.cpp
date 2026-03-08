#include <anyar/app.h>
#include <anyar/main_thread.h>

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
    WindowCreateOptions opts;
    // Copy fields from WindowConfig
    opts.title = std::move(config.title);
    opts.width = config.width;
    opts.height = config.height;
    opts.resizable = config.resizable;
    opts.decorations = config.decorations;
    opts.debug = config.debug;
    opts.label = "main";
    create_window(std::move(opts));
}

void App::create_window(WindowCreateOptions opts) {
    opts.debug = opts.debug || config_.debug;
    main_window_opts_ = std::move(opts);
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

    // ── Register window management commands ─────────────────────────────
    register_window_commands();

    // ── Auto-register built-in plugins ─────────────────────────────────────
    std::vector<std::shared_ptr<IAnyarPlugin>> builtins;
    builtins.push_back(std::make_shared<FsPlugin>());
    builtins.push_back(std::make_shared<DialogPlugin>());
    builtins.push_back(std::make_shared<ShellPlugin>());
    builtins.push_back(std::make_shared<ClipboardPlugin>());
    builtins.push_back(std::make_shared<DbPlugin>());

    // ── Initialize plugins BEFORE serve_static ─────────────────────────────
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
    std::string dist_abs = std::filesystem::absolute(config_.dist_path).string();
    if (std::filesystem::exists(dist_abs)) {
        asyik::static_file_config cfg;
        cfg.cache_control = "no-cache";
        cfg.index_file    = "index.html";
        server_->serve_static("/", dist_abs, cfg);
    } else {
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
        // ── Window creation callback — sets up IPC for every new window ──
        window_mgr_.set_on_window_created(
            [this](Window& window, const WindowCreateOptions& opts) {
                setup_native_ipc(&window);
            });

        // ── Window closed callback — cleanup sinks, stop app on last ──
        window_mgr_.set_on_window_closed(
            [this](const std::string& label) {
                // Remove the event sink for this window
                auto it = native_event_sinks_.find(label);
                if (it != native_event_sinks_.end()) {
                    events_.remove_ws_sink(it->second);
                    native_event_sinks_.erase(it);
                }

                // Emit window:closed event
                events_.emit("window:closed", {{"label", label}});

                if (config_.debug) {
                    std::cout << "[LibAnyar] Window closed: " << label
                              << " (" << window_mgr_.count() << " remaining)"
                              << std::endl;
                }

                // If main window closed or no windows left, stop app
                if (label == "main" || window_mgr_.count() == 0) {
                    // Close remaining child windows
                    window_mgr_.close_all();
                    if (service_) {
                        service_->stop();
                    }
                }
            });

        // Create the main window on the main thread
        window_mgr_.create(main_window_opts_, port_);

        Window* main_win = window_mgr_.main_window();
        if (!main_win) {
            std::cerr << "[LibAnyar] Failed to create main window" << std::endl;
            if (service_) service_->stop();
            if (service_thread_.joinable()) service_thread_.join();
            return 1;
        }

        // Emit window:created event for main
        events_.emit("window:created", {
            {"label", "main"},
            {"title", main_window_opts_.title}
        });

        // Block on the main window's event loop.
        // This also processes events for all child windows.
        main_win->run();

        // Main window closed — stop service
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

    // Clean up remaining event sinks
    for (auto& [label, sink_id] : native_event_sinks_) {
        events_.remove_ws_sink(sink_id);
    }
    native_event_sinks_.clear();

    return 0;
}

// ── Window IPC Commands ─────────────────────────────────────────────────────

void App::register_window_commands() {
    // window:create — create a new window from frontend
    commands_.add_async("window:create",
        [this](const json& args, CommandReply reply) {
            WindowCreateOptions opts;
            opts.label     = args.at("label").get<std::string>();
            opts.title     = args.value("title", "LibAnyar");
            opts.width     = args.value("width", 800);
            opts.height    = args.value("height", 600);
            opts.url       = args.value("url", "/");
            opts.parent    = args.value("parent", std::string());
            opts.modal     = args.value("modal", false);
            opts.resizable = args.value("resizable", true);
            opts.center    = args.value("center", true);
            opts.always_on_top = args.value("alwaysOnTop", false);
            opts.closable  = args.value("closable", true);
            opts.decorations = args.value("decorations", true);
            opts.debug     = config_.debug;

            // Window creation must happen on the main thread
            post_to_main_thread([this, opts, reply]() {
                try {
                    std::string label = window_mgr_.create(opts, port_);
                    // Emit window:created event
                    events_.emit("window:created", {
                        {"label", label},
                        {"title", opts.title}
                    });
                    reply(json{{"label", label}}, "");
                } catch (const std::exception& e) {
                    reply(json::object(), e.what());
                }
            });
        });

    // window:close — close a specific window
    commands_.add_async("window:close",
        [this](const json& args, CommandReply reply) {
            std::string label = args.value("label", std::string());
            if (label.empty()) {
                reply(json::object(), "Missing window label");
                return;
            }
            post_to_main_thread([this, label, reply]() {
                window_mgr_.close(label);
                reply(json{{"ok", true}}, "");
            });
        });

    // window:close-all — close all windows (triggers app shutdown)
    commands_.add("window:close-all", [this](const json&) -> json {
        post_to_main_thread([this]() {
            Window* main_win = window_mgr_.main_window();
            if (main_win) {
                main_win->terminate();
            }
        });
        return {{"ok", true}};
    });

    // window:list — list open windows
    commands_.add("window:list", [this](const json&) -> json {
        auto lbls = window_mgr_.labels();
        json result = json::array();
        for (auto& lbl : lbls) {
            result.push_back({{"label", lbl}});
        }
        return result;
    });

    // window:set-title — change a window's title
    commands_.add_async("window:set-title",
        [this](const json& args, CommandReply reply) {
            std::string label = args.at("label").get<std::string>();
            std::string title = args.at("title").get<std::string>();
            post_to_main_thread([this, label, title, reply]() {
                Window* win = window_mgr_.get(label);
                if (win) {
                    win->set_title(title);
                    reply(json{{"ok", true}}, "");
                } else {
                    reply(json::object(), "Window not found: " + label);
                }
            });
        });

    // window:set-size — resize a window
    commands_.add_async("window:set-size",
        [this](const json& args, CommandReply reply) {
            std::string label = args.at("label").get<std::string>();
            int w = args.at("width").get<int>();
            int h = args.at("height").get<int>();
            post_to_main_thread([this, label, w, h, reply]() {
                Window* win = window_mgr_.get(label);
                if (win) {
                    win->set_size(w, h);
                    reply(json{{"ok", true}}, "");
                } else {
                    reply(json::object(), "Window not found: " + label);
                }
            });
        });

    // window:focus — bring a window to front
    commands_.add_async("window:focus",
        [this](const json& args, CommandReply reply) {
            std::string label = args.at("label").get<std::string>();
            post_to_main_thread([this, label, reply]() {
                Window* win = window_mgr_.get(label);
                if (win) {
                    win->focus();
                    reply(json{{"ok", true}}, "");
                } else {
                    reply(json::object(), "Window not found: " + label);
                }
            });
        });

    // window:set-enabled — enable/disable input on a window
    commands_.add_async("window:set-enabled",
        [this](const json& args, CommandReply reply) {
            std::string label = args.at("label").get<std::string>();
            bool enabled = args.at("enabled").get<bool>();
            post_to_main_thread([this, label, enabled, reply]() {
                Window* win = window_mgr_.get(label);
                if (win) {
                    win->set_enabled(enabled);
                    reply(json{{"ok", true}}, "");
                } else {
                    reply(json::object(), "Window not found: " + label);
                }
            });
        });

    // window:set-always-on-top — toggle always-on-top
    commands_.add_async("window:set-always-on-top",
        [this](const json& args, CommandReply reply) {
            std::string label = args.at("label").get<std::string>();
            bool on_top = args.at("alwaysOnTop").get<bool>();
            post_to_main_thread([this, label, on_top, reply]() {
                Window* win = window_mgr_.get(label);
                if (win) {
                    win->set_always_on_top(on_top);
                    reply(json{{"ok", true}}, "");
                } else {
                    reply(json::object(), "Window not found: " + label);
                }
            });
        });

    // window:get-label — returns the calling window's own label
    // (resolved from the calling context — each window has its label injected)
    commands_.add("window:get-label", [](const json& args) -> json {
        // The label is injected client-side as window.__LIBANYAR_WINDOW_LABEL__
        // This command is a fallback / validation.
        return {{"label", args.value("_caller_label", "main")}};
    });
}

// ── Native IPC Setup ────────────────────────────────────────────────────────
//
// Per-window: binds `window.__anyar_ipc__(json)` → CommandRegistry::dispatch.
// Also registers a native event push sink per window.

void App::setup_native_ipc(Window* window) {
    if (!window) return;
    std::string label = window->label();

    // ── Bind __anyar_ipc__ for direct command invocation ─────────────────
    window->bind("__anyar_ipc__",
        [this, label](const std::string& seq, const std::string& req) {
            // This callback runs on the main thread.  Dispatch the
            // real work to a libasyik fiber so that command handlers can
            // use run_on_main_thread() without deadlocking.
            std::string seq_copy = seq;
            std::string req_copy = req;

            service_->execute([this, label, seq_copy, req_copy]() {
                std::string result;
                try {
                    auto args_arr = json::parse(req_copy);
                    std::string payload_str = args_arr[0].get<std::string>();
                    auto body = json::parse(payload_str);

                    IpcRequest ipc_req;
                    ipc_req.id  = body.value("id", "");
                    ipc_req.cmd = body.at("cmd").get<std::string>();
                    ipc_req.args = body.value("args", json::object());
                    // Inject calling window's label
                    ipc_req.args["_caller_label"] = label;

                    auto resp = commands_.dispatch(ipc_req);
                    result = resp.to_json().dump();
                } catch (const std::exception& e) {
                    IpcResponse err_resp;
                    err_resp.error = std::string("Native IPC error: ") + e.what();
                    result = err_resp.to_json().dump();
                }

                // Resolve the JS promise — dispatch to main thread
                Window* win = window_mgr_.get(label);
                if (win && !win->is_destroyed()) {
                    win->dispatch([this, label, seq_copy, result]() {
                        Window* w = window_mgr_.get(label);
                        if (w && !w->is_destroyed()) {
                            w->return_result(seq_copy, 0, result);
                        }
                    });
                }
            });
        }
    );

    // ── Register a native event push sink for this window ────────────────
    uint64_t sink_id = events_.add_ws_sink(
        [this, label](const std::string& message) {
            Window* win = window_mgr_.get(label);
            if (win && !win->is_destroyed()) {
                std::string msg_copy = message;
                win->dispatch([this, label, msg_copy]() {
                    Window* w = window_mgr_.get(label);
                    if (w && !w->is_destroyed()) {
                        w->eval(
                            "window.__anyar_dispatch_event__&&"
                            "window.__anyar_dispatch_event__(" + msg_copy + ")");
                    }
                });
            }
        }
    );
    native_event_sinks_[label] = sink_id;

    // ── Inject the JS-side event dispatcher (runs before window.onload) ─
    window->init(R"JS(
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
    if (!main_window_opts_.debug && !config_.debug) {
        window->init(R"JS(
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
        std::cout << "[LibAnyar] Native IPC bound for window '"
                  << label << "'" << std::endl;
    }
}

} // namespace anyar
