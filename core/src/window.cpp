#include <anyar/window.h>
#include "webview/webview.h"

#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

// Platform-specific GTK includes (Linux only)
#ifdef __linux__
#include <gtk/gtk.h>
#endif

namespace anyar {

// ── Pimpl ───────────────────────────────────────────────────────────────────

struct Window::Impl {
    webview_t wv = nullptr;
    int server_port;
    std::string label;
    bool destroyed = false;
    bool closable = true;

    // Pointers to bound callback closures — must outlive the webview
    std::vector<std::unique_ptr<Window::BindCallback>> bind_cbs;

    // Close handlers
    Window::CloseHandler on_close;
    Window::CloseRequestedHandler on_close_requested;

#ifdef __linux__
    gulong delete_event_handler_id = 0;
    gulong destroy_handler_id = 0;
#endif

    Impl(const WindowCreateOptions& opts, int port)
        : server_port(port), label(opts.label), closable(opts.closable),
          stored_width(opts.width), stored_height(opts.height),
          stored_hint(opts.resizable ? WEBVIEW_HINT_NONE : WEBVIEW_HINT_FIXED),
          needs_show(true)
    {
        wv = webview_create(opts.debug ? 1 : 0, nullptr);
        if (!wv) {
            throw std::runtime_error("Failed to create webview instance");
        }
        webview_set_title(wv, opts.title.c_str());
        // NOTE: Do NOT call webview_set_size() or connect_close_signals() here.
        // For child windows, we defer showing until all setup (parent, modal,
        // IPC binding) is complete.  Call show_window() after setup.

        // Apply initial options
        if (opts.always_on_top) {
            set_always_on_top(true);
        }
    }

    // Legacy constructor (backward compat from WindowConfig)
    Impl(const WindowConfig& config, int port)
        : server_port(port), label("main"), closable(true)
    {
        wv = webview_create(config.debug ? 1 : 0, nullptr);
        if (!wv) {
            throw std::runtime_error("Failed to create webview instance");
        }
        webview_set_title(wv, config.title.c_str());
        webview_set_size(wv, config.width, config.height,
                         config.resizable ? WEBVIEW_HINT_NONE : WEBVIEW_HINT_FIXED);
        connect_close_signals();
    }

    // Show the window (set size + realize + connect close signals).
    // Must be called on the main thread after all setup is done.
    void show_window() {
        if (!wv || !needs_show) return;
        needs_show = false;
        webview_set_size(wv, stored_width, stored_height, stored_hint);
        connect_close_signals();
    }

    ~Impl() {
        if (wv && !destroyed) {
            // Normal path: Window is being deleted without the GTK
            // "destroy" signal having fired (e.g. shared_ptr dropped).
            disconnect_close_signals();
            webview_destroy(wv);
            wv = nullptr;
        }
        // If destroyed == true, the GTK "destroy" signal already fired;
        // the library handled cleanup and we set wv = nullptr there.
    }

    // Deferred-show state for WindowCreateOptions constructor
    int stored_width = 800;
    int stored_height = 600;
    webview_hint_t stored_hint = WEBVIEW_HINT_NONE;
    bool needs_show = false;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // ── Platform-specific helpers ───────────────────────────────────────

    void* native_handle() const {
        if (!wv || destroyed) return nullptr;
        return webview_get_window(wv);
    }

#ifdef __linux__
    GtkWindow* gtk_window() const {
        return GTK_WINDOW(native_handle());
    }

    void connect_close_signals() {
        GtkWidget* win = static_cast<GtkWidget*>(native_handle());
        if (!win) return;

        // delete-event: fires when user tries to close (X button, Alt+F4)
        delete_event_handler_id = g_signal_connect(
            G_OBJECT(win), "delete-event",
            G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer user_data) -> gboolean {
                auto* self = static_cast<Impl*>(user_data);
                // If not closable, always prevent
                if (!self->closable) return TRUE;
                // If close-requested handler set, ask it
                if (self->on_close_requested) {
                    bool allow = self->on_close_requested();
                    return allow ? FALSE : TRUE;
                }
                return FALSE; // allow close
            }),
            this);

        // destroy: fires after the window is destroyed
        destroy_handler_id = g_signal_connect(
            G_OBJECT(win), "destroy",
            G_CALLBACK(+[](GtkWidget*, gpointer user_data) {
                auto* self = static_cast<Impl*>(user_data);
                self->destroyed = true;
                self->wv = nullptr;  // library handles cleanup — prevent double-destroy in ~Impl
                self->delete_event_handler_id = 0;
                self->destroy_handler_id = 0;
                // IMPORTANT: Defer the on_close callback to avoid re-entrant destruction.
                // Calling on_close() directly would drop the last shared_ptr → ~Impl
                // → webview_destroy → deplete_run_loop_event_queue → re-entrant event
                // processing INSIDE the "destroy" signal handler.
                if (self->on_close) {
                    auto* cb = new Window::CloseHandler(std::move(self->on_close));
                    self->on_close = nullptr;
                    g_idle_add(+[](gpointer data) -> gboolean {
                        auto* fn = static_cast<Window::CloseHandler*>(data);
                        (*fn)();
                        delete fn;
                        return G_SOURCE_REMOVE;
                    }, cb);
                }
            }),
            this);
    }

    void disconnect_close_signals() {
        GtkWidget* win = static_cast<GtkWidget*>(webview_get_window(wv));
        if (!win || destroyed) return;
        if (delete_event_handler_id) {
            g_signal_handler_disconnect(G_OBJECT(win), delete_event_handler_id);
            delete_event_handler_id = 0;
        }
        if (destroy_handler_id) {
            g_signal_handler_disconnect(G_OBJECT(win), destroy_handler_id);
            destroy_handler_id = 0;
        }
    }

    void set_parent(Impl& parent_impl) {
        auto* child_win = gtk_window();
        auto* parent_win = parent_impl.gtk_window();
        if (child_win && parent_win) {
            gtk_window_set_transient_for(child_win, parent_win);
        }
    }

    void set_modal(bool modal) {
        auto* win = gtk_window();
        if (win) {
            gtk_window_set_modal(win, modal ? TRUE : FALSE);
        }
    }

    void set_enabled(bool enabled) {
        auto* win = static_cast<GtkWidget*>(native_handle());
        if (win) {
            gtk_widget_set_sensitive(win, enabled ? TRUE : FALSE);
        }
    }

    void set_always_on_top(bool on_top) {
        auto* win = gtk_window();
        if (win) {
            gtk_window_set_keep_above(win, on_top ? TRUE : FALSE);
        }
    }

    void set_position(int x, int y) {
        auto* win = gtk_window();
        if (win) {
            gtk_window_move(win, x, y);
        }
    }

    void center_on_parent() {
        auto* win = gtk_window();
        if (!win) return;

        GtkWindow* parent = gtk_window_get_transient_for(win);
        if (parent) {
            // Center on parent
            int px, py, pw, ph;
            gtk_window_get_position(parent, &px, &py);
            gtk_window_get_size(parent, &pw, &ph);
            int cw, ch;
            gtk_window_get_size(win, &cw, &ch);
            int x = px + (pw - cw) / 2;
            int y = py + (ph - ch) / 2;
            gtk_window_move(win, x, y);
        } else {
            // Center on screen
            gtk_window_set_position(win, GTK_WIN_POS_CENTER);
        }
    }

    void focus() {
        auto* win = gtk_window();
        if (win) {
            gtk_window_present(win);
        }
    }

    void set_size(int w, int h) {
        if (wv) {
            webview_set_size(wv, w, h, WEBVIEW_HINT_NONE);
        }
    }
#else
    // Stubs for non-Linux platforms (Phase 7 will fill these)
    void connect_close_signals() {}
    void disconnect_close_signals() {}
    void set_parent(Impl&) {}
    void set_modal(bool) {}
    void set_enabled(bool) {}
    void set_always_on_top(bool) {}
    void set_position(int, int) {}
    void center_on_parent() {}
    void focus() {}
    void set_size(int w, int h) {
        if (wv) webview_set_size(wv, w, h, WEBVIEW_HINT_NONE);
    }
#endif
};

// ── Constructor / Destructor ────────────────────────────────────────────────

Window::Window(const WindowConfig& config, int server_port)
    : impl_(std::make_unique<Impl>(config, server_port))
{
    // Navigate to the LibAsyik HTTP server root
    std::ostringstream url;
    url << "http://127.0.0.1:" << server_port << "/";
    webview_navigate(impl_->wv, url.str().c_str());

    // Inject the port number so the JS bridge can find the IPC endpoint
    std::ostringstream init_js;
    init_js << "window.__LIBANYAR_PORT__ = " << server_port << ";";
    webview_init(impl_->wv, init_js.str().c_str());
}

Window::Window(const WindowCreateOptions& opts, int server_port)
    : impl_(std::make_unique<Impl>(opts, server_port))
{
    // Build the URL
    std::ostringstream url;
    if (opts.url.find("://") != std::string::npos) {
        // Absolute URL
        url << opts.url;
    } else {
        // Relative path — resolve against the local server
        url << "http://127.0.0.1:" << server_port;
        if (!opts.url.empty() && opts.url[0] != '/') {
            url << "/";
        }
        url << opts.url;
    }
    webview_navigate(impl_->wv, url.str().c_str());

    // Inject port + window label for the JS bridge
    std::ostringstream init_js;
    init_js << "window.__LIBANYAR_PORT__ = " << server_port << ";"
            << "window.__LIBANYAR_WINDOW_LABEL__ = '"
            << opts.label << "';";
    webview_init(impl_->wv, init_js.str().c_str());

    // Center if requested
    if (opts.center) {
        impl_->center_on_parent();
    }
}

Window::~Window() = default;

// ── Identification ──────────────────────────────────────────────────────────

const std::string& Window::label() const {
    return impl_->label;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void Window::run() {
    webview_run(impl_->wv);
}

void Window::terminate() {
    webview_terminate(impl_->wv);
}

void Window::destroy() {
    if (impl_->wv && !impl_->destroyed) {
        impl_->disconnect_close_signals();
        impl_->destroyed = true;
        webview_destroy(impl_->wv);
        impl_->wv = nullptr;
    }
}

bool Window::is_destroyed() const {
    return impl_->destroyed;
}

void Window::show() {
    impl_->show_window();
}

// ── Webview Operations ──────────────────────────────────────────────────────

void Window::eval(const std::string& js) {
    if (impl_->wv && !impl_->destroyed) {
        webview_eval(impl_->wv, js.c_str());
    }
}

void Window::navigate(const std::string& url) {
    if (impl_->wv) {
        webview_navigate(impl_->wv, url.c_str());
    }
}

void Window::dispatch(std::function<void()> fn) {
    if (!impl_->wv) return;
    auto* f = new std::function<void()>(std::move(fn));
    webview_dispatch(impl_->wv,
        [](webview_t /*w*/, void* arg) {
            auto* func = static_cast<std::function<void()>*>(arg);
            (*func)();
            delete func;
        },
        f);
}

void Window::set_title(const std::string& title) {
    if (impl_->wv) {
        webview_set_title(impl_->wv, title.c_str());
    }
}

void Window::set_size(int width, int height) {
    impl_->set_size(width, height);
}

// ── Native Handle ───────────────────────────────────────────────────────────

void* Window::native_handle() const {
    return impl_->native_handle();
}

// ── Parent / Child / Modal ──────────────────────────────────────────────────

void Window::set_parent(Window& parent) {
    impl_->set_parent(*parent.impl_);
}

void Window::set_modal(bool modal) {
    impl_->set_modal(modal);
}

void Window::set_enabled(bool enabled) {
    impl_->set_enabled(enabled);
}

// ── Window Appearance & Behavior ────────────────────────────────────────────

void Window::set_always_on_top(bool on_top) {
    impl_->set_always_on_top(on_top);
}

void Window::set_closable(bool closable) {
    impl_->closable = closable;
}

void Window::set_position(int x, int y) {
    impl_->set_position(x, y);
}

void Window::center_on_parent() {
    impl_->center_on_parent();
}

void Window::focus() {
    impl_->focus();
}

// ── Close Event Handlers ────────────────────────────────────────────────────

void Window::set_on_close(CloseHandler handler) {
    impl_->on_close = std::move(handler);
}

void Window::set_on_close_requested(CloseRequestedHandler handler) {
    impl_->on_close_requested = std::move(handler);
}

// ── Native IPC ──────────────────────────────────────────────────────────────

void Window::bind(const std::string& name, BindCallback callback) {
    if (!impl_->wv) return;
    auto cb = std::make_unique<BindCallback>(std::move(callback));
    auto* raw_ptr = cb.get();
    impl_->bind_cbs.push_back(std::move(cb));

    webview_bind(impl_->wv, name.c_str(),
        [](const char* seq, const char* req, void* arg) {
            auto* fn = static_cast<BindCallback*>(arg);
            (*fn)(std::string(seq), std::string(req));
        },
        raw_ptr);
}

void Window::return_result(const std::string& seq, int status,
                           const std::string& result) {
    if (impl_->wv) {
        webview_return(impl_->wv, seq.c_str(), status, result.c_str());
    }
}

void Window::init(const std::string& js) {
    if (impl_->wv) {
        webview_init(impl_->wv, js.c_str());
    }
}

} // namespace anyar
