#include <anyar/app.h>
#include <anyar/main_thread.h>
#include <anyar/pinhole.h>
#include <anyar/shared_buffer.h>

#include <anyar/plugins/fs_plugin.h>
#include <anyar/plugins/dialog_plugin.h>
#include <anyar/plugins/shell_plugin.h>
#include <anyar/plugins/clipboard_plugin.h>
#include <anyar/plugins/db_plugin.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

#ifdef __linux__
#include <gtk/gtk.h>
#endif

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

    // Create the service (fiber engine) eagerly so app.service() is
    // available immediately after construction — before run().
    // The HTTP server and fibers will still start inside run().
    service_ = asyik::make_service();
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

void App::emit_to(const std::string& label, const std::string& event,
                  const json& payload) {
    events_.emit_to_window(label, event, payload);
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

// ── Embedded Frontend ───────────────────────────────────────────────────────

void App::set_frontend_resolver(FileResolver resolver) {
    frontend_resolver_ = std::move(resolver);
}

void App::set_on_server_ready(ServerReadyCallback cb) {
    on_server_ready_ = std::move(cb);
}

void App::on_ready(ReadyCallback cb) {
    on_ready_ = std::move(cb);
}

void App::on_window_ready(WindowReadyCallback cb) {
    on_window_ready_ = std::move(cb);
}

// ── Custom HTTP Routes ──────────────────────────────────────────────────────

void App::http_get(const std::string& path, RouteHandler handler) {
    if (server_) {
        // Server already running — register with insert_front for priority
        server_->on_http_request(path, "GET", std::move(handler), /*insert_front=*/true);
    } else {
        deferred_routes_.push_back({"GET", path, std::move(handler)});
    }
}

void App::http_post(const std::string& path, RouteHandler handler) {
    if (server_) {
        server_->on_http_request(path, "POST", std::move(handler), /*insert_front=*/true);
    } else {
        deferred_routes_.push_back({"POST", path, std::move(handler)});
    }
}

// ── Local File Access ───────────────────────────────────────────────────────

void App::allow_file_access(const std::string& directory) {
    namespace fs = std::filesystem;
    // Resolve to canonical path to prevent traversal via symlinks
    std::string canonical = fs::canonical(fs::path(directory)).string();
    allowed_file_roots_.push_back(std::move(canonical));
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
        events_.emit(event, payload);   // broadcast to C++ subs + all windows
        return nullptr;
    });

    // ── Targeted event: emit to a specific window ────────────────────────
    commands_.add("anyar:emit_to_window", [this](const json& args) -> json {
        std::string label = args.at("label").get<std::string>();
        std::string event = args.at("event").get<std::string>();
        json payload = args.value("payload", json::object());
        events_.emit_to_window(label, event, payload);
        return nullptr;
    });

    // ── Global listener toggle ───────────────────────────────────────────
    commands_.add("anyar:enable_global_listener", [this](const json& args) -> json {
        std::string caller = args.value("_caller_label", std::string("main"));
        bool enabled = args.value("enabled", true);
        auto it = native_event_sinks_.find(caller);
        if (it != native_event_sinks_.end()) {
            events_.set_global_listener(it->second, enabled);
        }
        return json{{"ok", true}};
    });

    // ── Register window management commands ─────────────────────────────
    register_window_commands();

    // ── Register shared buffer commands ──────────────────────────────────
    register_buffer_commands();

    // ── Register pinhole IPC commands ────────────────────────────────────
    register_pinhole_commands();

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

    // ── SharedBuffer HTTP fallback (for browser dev mode) ───────────────────
    // When running in a regular browser (`anyar dev`), the anyar-shm:// custom
    // URI scheme is unavailable. This HTTP GET endpoint serves raw buffer bytes
    // so the JS fetchBuffer() can fall back to HTTP.
    server_->on_http_request("/__anyar__/buffer/<string>", "GET",
        [](asyik::http_request_ptr req, asyik::http_route_args args) {
            std::string name = args[1];
            auto buf = SharedBufferRegistry::instance().get(name);
            if (!buf) {
                req->response.body = "Buffer not found: " + name;
                req->response.headers.set("Content-Type", "text/plain");
                req->response.result(404);
                return;
            }
            req->response.body.assign(
                reinterpret_cast<const char*>(buf->data()), buf->size());
            req->response.headers.set("Content-Type", "application/octet-stream");
            req->response.headers.set("Content-Length", std::to_string(buf->size()));
            req->response.headers.set("Cache-Control", "no-store");
            req->response.result(200);
        }
    );

    // ── User-registered HTTP routes (before serve_static) ─────────────────
    // Deferred routes registered via http_get/http_post before run()
    for (auto& route : deferred_routes_) {
        server_->on_http_request(route.path, route.method,
                                 std::move(route.handler));
    }
    deferred_routes_.clear();

    // Legacy callback
    if (on_server_ready_) {
        on_server_ready_(server_);
    }

    // ── Local file serving HTTP fallback (for browser dev mode) ─────────
    if (!allowed_file_roots_.empty()) {
        auto roots = allowed_file_roots_;  // capture a copy
        server_->on_http_request("/__anyar__/file/<path>", "GET",
            [roots](asyik::http_request_ptr req, asyik::http_route_args args) {
                std::string rel = args[1];
                // Reject obvious traversal attempts
                if (rel.find("..") != std::string::npos) {
                    req->response.body = "Forbidden";
                    req->response.result(403);
                    return;
                }
                namespace fs = std::filesystem;
                for (auto& root : roots) {
                    fs::path candidate = fs::path(root) / rel;
                    std::error_code ec;
                    fs::path canon = fs::canonical(candidate, ec);
                    if (ec || !fs::is_regular_file(canon, ec)) continue;
                    // Verify the canonical path is under the allowed root
                    if (canon.string().rfind(root, 0) != 0) continue;
                    // Read and serve the file
                    std::ifstream ifs(canon, std::ios::binary);
                    if (!ifs) continue;
                    std::string body((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                    // Determine Content-Type from extension
                    std::string ext = canon.extension().string();
                    std::string ct = "application/octet-stream";
                    if (ext == ".png") ct = "image/png";
                    else if (ext == ".jpg" || ext == ".jpeg") ct = "image/jpeg";
                    else if (ext == ".gif") ct = "image/gif";
                    else if (ext == ".webp") ct = "image/webp";
                    else if (ext == ".svg") ct = "image/svg+xml";
                    else if (ext == ".mp4") ct = "video/mp4";
                    else if (ext == ".webm") ct = "video/webm";
                    else if (ext == ".mp3") ct = "audio/mpeg";
                    else if (ext == ".wav") ct = "audio/wav";
                    else if (ext == ".ogg") ct = "audio/ogg";
                    else if (ext == ".pdf") ct = "application/pdf";
                    else if (ext == ".json") ct = "application/json";
                    else if (ext == ".txt") ct = "text/plain";
                    else if (ext == ".html") ct = "text/html";
                    else if (ext == ".css") ct = "text/css";
                    else if (ext == ".js") ct = "text/javascript";
                    req->response.body = std::move(body);
                    req->response.headers.set("Content-Type", ct);
                    req->response.headers.set("Cache-Control", "no-store");
                    req->response.result(200);
                    return;
                }
                req->response.body = "File not found or access denied";
                req->response.headers.set("Content-Type", "text/plain");
                req->response.result(404);
            }
        );
    }

    // ── on_ready callback (service + server + plugins all initialized) ───
    if (on_ready_) {
        on_ready_();
    }

    // ── Frontend serving ────────────────────────────────────────────────────
    if (frontend_resolver_) {
        // Embedded mode: serve frontend from compiled-in resources
        auto resolver = frontend_resolver_;
        server_->on_http_request("/<path>", "GET",
            [resolver](asyik::http_request_ptr req, asyik::http_route_args args) {
                std::string path = "/" + args[0];
                std::string body, content_type;
                if (resolver(path, body, content_type)) {
                    req->response.body = std::move(body);
                    req->response.headers.set("Content-Type", content_type);
                    req->response.headers.set("Cache-Control", "no-cache");
                    req->response.result(200);
                } else {
                    req->response.body = "404 Not Found";
                    req->response.headers.set("Content-Type", "text/plain");
                    req->response.result(404);
                }
            }
        );
        // Also handle bare "/" (root without path)
        server_->on_http_request("/", "GET",
            [resolver](asyik::http_request_ptr req, asyik::http_route_args args) {
                std::string body, content_type;
                if (resolver("/", body, content_type)) {
                    req->response.body = std::move(body);
                    req->response.headers.set("Content-Type", content_type);
                    req->response.headers.set("Cache-Control", "no-cache");
                    req->response.result(200);
                } else {
                    req->response.body = "404 Not Found";
                    req->response.headers.set("Content-Type", "text/plain");
                    req->response.result(404);
                }
            }
        );
    } else {
        // Filesystem mode: serve frontend from dist directory
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
}

// ── Run ─────────────────────────────────────────────────────────────────────

int App::run() {
    // Register the anyar-shm:// URI scheme BEFORE any webview is created.
    // This must happen on the main thread, before start_server().
    register_shm_uri_scheme();

    // Register anyar-file:// if any directories were allowed
    register_file_uri_scheme(allowed_file_roots_);

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

                // Emit window:focused when the window gains focus
                std::string lbl = opts.label;
                window.set_on_focus([this, lbl]() {
                    events_.emit("window:focused", {{"label", lbl}});
                });
            });

        // ── Window closed callback — cleanup sinks, stop app on last ──
        window_mgr_.set_on_window_closed(
            [this](const std::string& label) {
                // Remove the event sink for this window
                auto it = native_event_sinks_.find(label);
                if (it != native_event_sinks_.end()) {
                    events_.remove_window_sink(label);
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

        // User hook: window exists, GTK loop not yet running.
        // Correct place for create_pinhole() and similar setup.
        if (on_window_ready_) {
            on_window_ready_(*main_win);
        }

        // Block on the main window's event loop.
        // This also processes events for all child windows.
        main_win->run();

        // ── Orderly shutdown ────────────────────────────────────────
#ifdef __linux__
        // 0) Drain a limited number of pending GTK idle callbacks
        //    BEFORE stopping the service.  This fulfils promises from
        //    run_on_main_thread() so blocked fibers can resume.
        //    Cap iterations to avoid hanging under xvfb where
        //    WebKitGTK may continuously generate events.
        for (int i = 0; i < 200 && g_main_context_pending(nullptr); ++i) {
            g_main_context_iteration(nullptr, FALSE);
        }
#endif

        // 1) Close the HTTP server acceptor so the accept-loop fiber
        //    can exit, then stop the service.
        if (server_) {
            server_->close();
        }
        if (service_) {
            service_->stop();
        }
        if (service_thread_.joinable()) {
            service_thread_.join();
        }

        // 2) Clean up event sinks BEFORE window destruction so that
        //    no event dispatch tries to eval JS on a dying webview.
        for (auto& [lbl, sink_id] : native_event_sinks_) {
            events_.remove_window_sink(lbl);
        }
        native_event_sinks_.clear();

        // 3) Explicitly destroy all windows.  Each Window::destroy()
        //    sets destroyed=true, so any stale g_idle_add callbacks
        //    processed during webview’s deplete_run_loop_event_queue()
        //    will bail out via the is_destroyed() guard.
        window_mgr_.close_all();

        // NOTE: We intentionally do NOT drain GTK events after
        // close_all().  Under xvfb, WebKitGTK may continuously
        // generate events during web process teardown, causing an
        // infinite drain loop.  The destroyed flag guards all
        // callbacks, and deferred on_close handlers are not needed
        // since we are shutting down the entire application.

    } else {
        // No window — run headless (useful for testing)
        service_thread_.join();
        return 0;
    }

    // Shutdown plugins
    for (auto& plugin : plugins_) {
        plugin->shutdown();
    }

    return 0;
}

// ── Window IPC Commands ─────────────────────────────────────────────────────

void App::register_window_commands() {
    // window:create — create a new window from frontend
    // Uses run_on_main_thread() (fiber-blocking) so that the calling fiber
    // waits until the GTK main thread finishes window creation.
    commands_.add("window:create", [this](const json& args) -> json {
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
            opts.min_width = args.value("minWidth", 0);
            opts.min_height = args.value("minHeight", 0);

        // Window creation must happen on the main thread.
        // run_on_main_thread blocks the current fiber until complete.
        std::string label = run_on_main_thread([this, opts]() -> std::string {
            return window_mgr_.create(opts, port_);
        });

        // Emit window:created event (safe from fiber context)
        events_.emit("window:created", {
            {"label", label},
            {"title", opts.title}
        });
        return json{{"label", label}};
    });

    // window:close — close a specific window
    commands_.add("window:close", [this](const json& args) -> json {
        std::string label = args.value("label", std::string());
        if (label.empty()) {
            throw std::runtime_error("Missing window label");
        }
        run_on_main_thread([this, label]() {
            window_mgr_.close(label);
        });
        return json{{"ok", true}};
    });

    // window:close-all — close all windows (triggers app shutdown)
    // NOTE: terminate() is thread-safe (internally uses g_idle_add),
    // so we call it directly instead of blocking the fiber with
    // post_to_main_thread — that would race with the shutdown
    // sequence after the main loop exits.
    commands_.add("window:close-all", [this](const json&) -> json {
        Window* main_win = window_mgr_.main_window();
        if (main_win) {
            main_win->terminate();
        }
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
    commands_.add("window:set-title", [this](const json& args) -> json {
        std::string label = args.at("label").get<std::string>();
        std::string title = args.at("title").get<std::string>();
        return run_on_main_thread([this, label, title]() -> json {
            Window* win = window_mgr_.get(label);
            if (win) {
                win->set_title(title);
                return json{{"ok", true}};
            }
            throw std::runtime_error("Window not found: " + label);
        });
    });

    // window:set-size — resize a window
    commands_.add("window:set-size", [this](const json& args) -> json {
        std::string label = args.at("label").get<std::string>();
        int w = args.at("width").get<int>();
        int h = args.at("height").get<int>();
        return run_on_main_thread([this, label, w, h]() -> json {
            Window* win = window_mgr_.get(label);
            if (win) {
                win->set_size(w, h);
                return json{{"ok", true}};
            }
            throw std::runtime_error("Window not found: " + label);
        });
    });

    // window:focus — bring a window to front
    commands_.add("window:focus", [this](const json& args) -> json {
        std::string label = args.at("label").get<std::string>();
        return run_on_main_thread([this, label]() -> json {
            Window* win = window_mgr_.get(label);
            if (win) {
                win->focus();
                return json{{"ok", true}};
            }
            throw std::runtime_error("Window not found: " + label);
        });
    });

    // window:set-enabled — enable/disable input on a window
    commands_.add("window:set-enabled", [this](const json& args) -> json {
        std::string label = args.at("label").get<std::string>();
        bool enabled = args.at("enabled").get<bool>();
        return run_on_main_thread([this, label, enabled]() -> json {
            Window* win = window_mgr_.get(label);
            if (win) {
                win->set_enabled(enabled);
                return json{{"ok", true}};
            }
            throw std::runtime_error("Window not found: " + label);
        });
    });

    // window:set-always-on-top — toggle always-on-top
    commands_.add("window:set-always-on-top", [this](const json& args) -> json {
        std::string label = args.at("label").get<std::string>();
        bool on_top = args.at("alwaysOnTop").get<bool>();
        return run_on_main_thread([this, label, on_top]() -> json {
            Window* win = window_mgr_.get(label);
            if (win) {
                win->set_always_on_top(on_top);
                return json{{"ok", true}};
            }
            throw std::runtime_error("Window not found: " + label);
        });
    });

    // window:get-label — returns the calling window's own label
    // (resolved from the calling context — each window has its label injected)
    commands_.add("window:get-label", [](const json& args) -> json {
        // The label is injected client-side as window.__LIBANYAR_WINDOW_LABEL__
        // This command is a fallback / validation.
        return {{"label", args.value("_caller_label", "main")}};
    });

    // window:set-close-confirmation — show a native confirm dialog before close
    //
    // When enabled, the GTK delete-event handler will show an Ok/Cancel dialog.
    // If the user clicks Cancel, the close is prevented.
    //
    // Args:
    //   label    (string)  — target window label
    //   enabled  (bool)    — true to enable, false to disable
    //   message  (string?) — confirmation message (default: "You have unsaved changes.\nClose anyway?")
    //   title    (string?) — dialog title (default: "Confirm Close")
    commands_.add("window:set-close-confirmation", [this](const json& args) -> json {
        std::string label   = args.at("label").get<std::string>();
        bool enabled        = args.at("enabled").get<bool>();
        std::string message = args.value("message",
            std::string("You have unsaved changes.\nClose anyway?"));
        std::string title   = args.value("title", std::string("Confirm Close"));

        run_on_main_thread([this, label, enabled, message, title]() {
            Window* win = window_mgr_.get(label);
            if (!win) return;

            if (!enabled) {
                win->set_close_confirmation("", "");
            } else {
                win->set_close_confirmation(message, title);
            }
        });

        return json{{"ok", true}};
    });
}

// ── Shared Buffer Commands ──────────────────────────────────────────────────

void App::register_buffer_commands() {
    // buffer:create — Create a named shared buffer
    //   name   (string)  — unique buffer name
    //   size   (number)  — byte size
    // Returns: { name, size, url }
    commands_.add("buffer:create", [this](const json& args) -> json {
        std::string name = args.at("name").get<std::string>();
        size_t size = args.at("size").get<size_t>();

        auto buf = SharedBuffer::create(name, size);
        if (!buf) {
            throw std::runtime_error("Failed to create shared buffer: " + name);
        }

        return json{
            {"name", buf->name()},
            {"size", buf->size()},
            {"url", "anyar-shm://" + buf->name()}
        };
    });

    // buffer:write — Write base64-encoded data into a shared buffer
    //   name   (string)  — buffer name
    //   data   (string)  — base64-encoded payload
    //   offset (number?) — byte offset (default 0)
    commands_.add("buffer:write", [this](const json& args) -> json {
        std::string name = args.at("name").get<std::string>();
        std::string data_b64 = args.at("data").get<std::string>();
        size_t offset = args.value("offset", 0);

        auto buf = SharedBufferRegistry::instance().get(name);
        if (!buf) {
            throw std::runtime_error("Buffer not found: " + name);
        }

        // Decode base64
        // Use a simple base64 decode (Boost.Beast has one)
        auto decoded = json::from_cbor(
            json::to_cbor(json::binary_t(
                std::vector<uint8_t>(data_b64.begin(), data_b64.end()))));
        // Actually, we'll accept raw binary from the CBOR path.
        // For now, use a simple base64 decoder:
        static const std::string b64chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<uint8_t> out;
        out.reserve(data_b64.size() * 3 / 4);
        int val = 0, valb = -8;
        for (unsigned char c : data_b64) {
            if (c == '=') break;
            auto pos = b64chars.find(c);
            if (pos == std::string::npos) continue;
            val = (val << 6) + static_cast<int>(pos);
            valb += 6;
            if (valb >= 0) {
                out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }

        if (offset + out.size() > buf->size()) {
            throw std::runtime_error("Write exceeds buffer size");
        }
        std::memcpy(buf->data() + offset, out.data(), out.size());

        return json{{"ok", true}, {"bytes_written", out.size()}};
    });

    // buffer:destroy — Destroy a shared buffer
    //   name (string) — buffer name
    commands_.add("buffer:destroy", [this](const json& args) -> json {
        std::string name = args.at("name").get<std::string>();
        SharedBufferRegistry::instance().remove(name);
        return json{{"ok", true}};
    });

    // buffer:list — List all active shared buffers
    commands_.add("buffer:list", [this](const json& args) -> json {
        auto names = SharedBufferRegistry::instance().names();
        json list = json::array();
        for (auto& n : names) {
            auto buf = SharedBufferRegistry::instance().get(n);
            if (buf) {
                list.push_back(json{
                    {"name", buf->name()},
                    {"size", buf->size()},
                    {"url", "anyar-shm://" + buf->name()}
                });
            }
        }
        return json{{"buffers", list}};
    });

    // buffer:notify — Emit a buffer-ready event to the frontend
    //   name     (string)  — buffer name
    //   metadata (object?) — arbitrary metadata to send with notification
    commands_.add("buffer:notify", [this](const json& args) -> json {
        std::string name = args.at("name").get<std::string>();
        json metadata = args.value("metadata", json::object());

        auto buf = SharedBufferRegistry::instance().get(name);
        if (!buf) {
            throw std::runtime_error("Buffer not found: " + name);
        }

        json payload = {
            {"name", name},
            {"url", "anyar-shm://" + name},
            {"size", buf->size()},
            {"metadata", metadata}
        };
        events_.emit("buffer:ready", payload);

        return json{{"ok", true}};
    });

    // ── Pool commands ──────────────────────────────────────────────────────

    // buffer:pool-create — Create a shared buffer pool
    //   name        (string) — base name for the pool
    //   bufferSize  (number) — size of each buffer in bytes
    //   count       (number?) — number of buffers (default 3)
    commands_.add("buffer:pool-create", [this](const json& args) -> json {
        std::string name = args.at("name").get<std::string>();
        size_t buffer_size = args.at("bufferSize").get<size_t>();
        size_t count = args.value("count", 3);

        if (buffer_pools_.count(name)) {
            throw std::runtime_error("Pool already exists: " + name);
        }

        auto pool = std::make_unique<SharedBufferPool>(name, buffer_size, count);
        json buffers = json::array();
        for (size_t i = 0; i < count; ++i) {
            std::string buf_name = name + "_" + std::to_string(i);
            auto buf = SharedBufferRegistry::instance().get(buf_name);
            if (buf) {
                buffers.push_back(json{
                    {"name", buf->name()},
                    {"size", buf->size()},
                    {"url", "anyar-shm://" + buf->name()}
                });
            }
        }

        buffer_pools_[name] = std::move(pool);

        return json{
            {"name", name},
            {"bufferSize", buffer_size},
            {"count", count},
            {"buffers", buffers}
        };
    });

    // buffer:pool-destroy — Destroy a shared buffer pool
    //   name (string) — pool base name
    commands_.add("buffer:pool-destroy", [this](const json& args) -> json {
        std::string name = args.at("name").get<std::string>();
        auto it = buffer_pools_.find(name);
        if (it == buffer_pools_.end()) {
            throw std::runtime_error("Pool not found: " + name);
        }
        buffer_pools_.erase(it);
        return json{{"ok", true}};
    });

    // buffer:pool-acquire — Acquire a writable buffer from the pool (C++ side)
    //   name (string) — pool base name
    // Returns: { name, url, size } of the acquired buffer
    commands_.add("buffer:pool-acquire", [this](const json& args) -> json {
        std::string name = args.at("name").get<std::string>();
        auto it = buffer_pools_.find(name);
        if (it == buffer_pools_.end()) {
            throw std::runtime_error("Pool not found: " + name);
        }

        SharedBuffer& buf = it->second->acquire_write();
        return json{
            {"name", buf.name()},
            {"size", buf.size()},
            {"url", "anyar-shm://" + buf.name()}
        };
    });

    // buffer:pool-release-write — Release a written buffer and notify frontend
    //   pool     (string) — pool base name
    //   name     (string) — buffer name
    //   metadata (object?) — metadata to include in notification
    commands_.add("buffer:pool-release-write", [this](const json& args) -> json {
        std::string pool_name = args.at("pool").get<std::string>();
        std::string buf_name = args.at("name").get<std::string>();
        json metadata = args.value("metadata", json::object());

        auto it = buffer_pools_.find(pool_name);
        if (it == buffer_pools_.end()) {
            throw std::runtime_error("Pool not found: " + pool_name);
        }

        auto buf = SharedBufferRegistry::instance().get(buf_name);
        if (!buf) {
            throw std::runtime_error("Buffer not found: " + buf_name);
        }

        it->second->release_write(*buf, metadata.dump());

        // Emit buffer:ready event
        json payload = {
            {"name", buf_name},
            {"pool", pool_name},
            {"url", "anyar-shm://" + buf_name},
            {"size", buf->size()},
            {"metadata", metadata}
        };
        events_.emit("buffer:ready", payload);

        return json{{"ok", true}};
    });

    // buffer:pool-release-read — Consumer releases a buffer back to the pool
    //   pool (string) — pool base name
    //   name (string) — buffer name to release
    commands_.add("buffer:pool-release-read", [this](const json& args) -> json {
        std::string pool_name = args.at("pool").get<std::string>();
        std::string buf_name = args.at("name").get<std::string>();

        auto it = buffer_pools_.find(pool_name);
        if (it == buffer_pools_.end()) {
            throw std::runtime_error("Pool not found: " + pool_name);
        }

        it->second->release_read(buf_name);
        return json{{"ok", true}};
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
    uint64_t sink_id = events_.add_window_sink(label,
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

// ── Pinhole IPC Commands ─────────────────────────────────────────────────────

void App::register_pinhole_commands() {
    // pinhole:update_rect — JS tracking protocol sends updated CSS-pixel rect
    // Args: { id, window_label, x, y, width, height, dpr }
    commands_.add("pinhole:update_rect", [this](const json& args) -> json {
        std::string win_label = args.value("window_label", std::string("main"));
        std::string id  = args.at("id").get<std::string>();
        int x  = static_cast<int>(args.value("x",      0.0));
        int y  = static_cast<int>(args.value("y",      0.0));
        int w  = static_cast<int>(args.value("width",  0.0));
        int h  = static_cast<int>(args.value("height", 0.0));

        Window* win = window_mgr_.get(win_label);
        if (!win) return {{"ok", false}, {"error", "window not found"}};
        auto pin = win->find_pinhole(id);
        if (!pin) return {{"ok", false}, {"error", "pinhole not found"}};
        // set_rect dispatches gtk_widget_queue_resize to main thread internally
        pin->set_rect(x, y, w, h);
        return {{"ok", true}};
    });

    // pinhole:set_visible — JS scroll-hide protocol
    // Args: { id, window_label, visible }
    commands_.add("pinhole:set_visible", [this](const json& args) -> json {
        std::string win_label = args.value("window_label", std::string("main"));
        std::string id  = args.at("id").get<std::string>();
        bool visible    = args.value("visible", true);

        Window* win = window_mgr_.get(win_label);
        if (!win) return {{"ok", false}, {"error", "window not found"}};
        auto pin = win->find_pinhole(id);
        if (!pin) return {{"ok", false}, {"error", "pinhole not found"}};
        pin->set_visible(visible);
        return {{"ok", true}};
    });

    // pinhole:get_metrics — query current pinhole state from JS
    // Args: { id, window_label }
    commands_.add("pinhole:get_metrics", [this](const json& args) -> json {
        std::string win_label = args.value("window_label", std::string("main"));
        std::string id  = args.at("id").get<std::string>();

        Window* win = window_mgr_.get(win_label);
        if (!win) return {{"ok", false}, {"error", "window not found"}};
        auto pin = win->find_pinhole(id);
        if (!pin) return {{"ok", false}, {"error", "pinhole not found"}};
        return {
            {"ok",        true},
            {"id",        pin->id()},
            {"is_native", pin->is_native()},
        };
    });

    // pinhole:dom_detached — JS tracking reports placeholder element removed from DOM.
    // Args: { id, window_label }
    commands_.add("pinhole:dom_detached", [this](const json& args) -> json {
        std::string win_label = args.value("window_label", std::string("main"));
        std::string id  = args.at("id").get<std::string>();

        Window* win = window_mgr_.get(win_label);
        if (!win) return {{"ok", false}, {"error", "window not found"}};
        auto pin = win->find_pinhole(id);
        if (!pin) return {{"ok", false}, {"error", "pinhole not found"}};
        pin->notify_dom_detached();
        return {{"ok", true}};
    });
}

} // namespace anyar
