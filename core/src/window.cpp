#include <anyar/window.h>
#include <anyar/pinhole.h>
#include "webview/webview.h"

#include <atomic>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <map>
#include <vector>

// Platform-specific includes
#ifdef __linux__
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#elif defined(_WIN32)
#include <anyar/main_thread.h>
#include "platform.h"
#include "win32_util.h"
#include "pinhole_win32.h"
#include <iostream>
#endif

namespace anyar {

// ── Pimpl ───────────────────────────────────────────────────────────────────

struct Window::Impl {
    webview_t wv = nullptr;
    int server_port;
    std::string label;
    bool destroyed = false;
    bool closable = true;
    bool owns_run_loop = false;

    // Pointers to bound callback closures — must outlive the webview
    std::vector<std::unique_ptr<Window::BindCallback>> bind_cbs;

    // Focus handler
    Window::FocusHandler on_focus;

    // Close handlers
    Window::CloseHandler on_close;
    Window::CloseRequestedHandler on_close_requested;

    // Live pinhole objects owned by this window (keyed by id for O(1) lookup).
    std::map<std::string, std::shared_ptr<Pinhole>> pinholes;

#ifdef __linux__
    gulong focus_in_handler_id = 0;
    gulong delete_event_handler_id = 0;
    gulong destroy_handler_id = 0;
    gulong window_state_handler_id = 0;

    // Inner GtkOverlay where pinhole GtkGLAreas are attached. Pinholes are
    // composited UNDER a transparent WebKitWebView so HTML can render on top
    // of native GL surfaces. Non-owning: GTK widget tree owns the lifetime.
    GtkOverlay* overlay = nullptr;

    // Tracks whether the pinhole JS tracking snippet has been injected
    // via webview_init() on this window (injected once per window).
    bool pinhole_js_injected = false;
#elif defined(_WIN32)
    // The engine's top-level HWND is subclassed to observe activation,
    // close requests, minimize and destruction.  Impl* is stored as a window
    // property because webview/webview already owns GWLP_USERDATA.
    HWND hooked_hwnd = nullptr;
    WNDPROC orig_wndproc = nullptr;

    HWND parent_window = nullptr;   // owner window (non-owning)
    bool is_modal = false;
    bool minimized = false;
    bool center_pending = false;    // center once the size is known (show)

    // Pinholes: DComp host below the webview (created on first pinhole) and
    // a liveness token for work queued by the fallback canvas.
    std::shared_ptr<PinholeHost> pinhole_host;
    bool pinhole_js_injected = false;
    std::shared_ptr<std::atomic<bool>> alive_token =
        std::make_shared<std::atomic<bool>>(true);

    // Window teardown (UI thread): pinholes and their composition host.
    void pinholes_window_destroyed() {
        alive_token->store(false);
        for (auto& [pid, pin] : pinholes) pin->notify_window_destroyed();
        if (pinhole_host) pinhole_host_window_destroyed(*pinhole_host);
    }
#endif

    Impl(const WindowCreateOptions& opts, int port)
        : server_port(port), label(opts.label), closable(opts.closable),
          stored_width(opts.width), stored_height(opts.height),
          stored_hint(opts.resizable ? WEBVIEW_HINT_NONE : WEBVIEW_HINT_FIXED),
          stored_min_width(opts.min_width), stored_min_height(opts.min_height),
          needs_show(true)
    {
        wv = webview_create(opts.debug ? 1 : 0, nullptr);
        if (!wv) {
            throw std::runtime_error("Failed to create webview instance");
        }
        webview_set_title(wv, opts.title.c_str());
        note_controller();
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
        note_controller();
        webview_set_size(wv, config.width, config.height,
                         config.resizable ? WEBVIEW_HINT_NONE : WEBVIEW_HINT_FIXED);
        connect_close_signals();
    }

    // Show the window (set size + realize + connect close signals).
    // Must be called on the main thread after all setup is done.
    void show_window() {
        if (!wv || !needs_show) return;
        needs_show = false;
        // Apply minimum size constraint before showing
        if (stored_min_width > 0 || stored_min_height > 0) {
            webview_set_size(wv, stored_min_width, stored_min_height,
                             WEBVIEW_HINT_MIN);
        }
#ifdef _WIN32
        // webview_set_size() shows the window; size + place it first so it
        // does not appear at CW_USEDEFAULT and then jump.
        if (center_pending) {
            center_pending = false;
            place_centered(frame_size_for_client(stored_width, stored_height));
        }
#endif
        webview_set_size(wv, stored_width, stored_height, stored_hint);

#ifdef __linux__
        // Reparent the WebKitWebView so pinholes can be composited UNDER it.
        // webview/webview places the WebKitWebView as the direct child of
        // GtkWindow (gtk_container_add in GTK3). We rebuild that subtree as:
        //
        //   GtkWindow → GtkOverlay (outer)
        //                 ├── GtkOverlay (inner)        ← `overlay`
        //                 │     ├── GtkEventBox (main, transparent)
        //                 │     └── GtkGLArea (overlay)  ← pinholes
        //                 └── WebKitWebView (transparent, on top)
        //
        // The webview gets a transparent background so HTML can blend with
        // whatever the pinhole(s) underneath are rendering. HTML controls
        // (timelines, panels, buttons) thus composite freely on top of
        // native GL surfaces.
        //
        // Trade-offs (versus a fully opaque webview):
        //  - Disables WebKit's opaque-blit fast path (slightly higher
        //    CPU/GPU cost).
        //  - Native widget input is masked by the webview; pinholes are
        //    visual-only unless the page explicitly forwards events.
        //  - Subpixel text antialiasing degrades to grayscale on a
        //    transparent webview.
        {
            GtkWidget* gtk_win = static_cast<GtkWidget*>(webview_get_window(wv));
            GtkWidget* child = gtk_bin_get_child(GTK_BIN(gtk_win));
            if (child && !GTK_IS_OVERLAY(child)) {
                g_object_ref(child);  // keep alive while detached
                gtk_container_remove(GTK_CONTAINER(gtk_win), child);

                GtkOverlay* outer = GTK_OVERLAY(gtk_overlay_new());

                // Inner overlay: holds pinhole GtkGLAreas. Its main child is
                // a transparent event box that fills the overlay; pinholes
                // are added later as overlay children of `inner`.
                GtkOverlay* inner = GTK_OVERLAY(gtk_overlay_new());
                GtkWidget*  bg    = gtk_event_box_new();
                gtk_event_box_set_visible_window(GTK_EVENT_BOX(bg), FALSE);
                gtk_widget_set_halign(GTK_WIDGET(inner), GTK_ALIGN_FILL);
                gtk_widget_set_valign(GTK_WIDGET(inner), GTK_ALIGN_FILL);
                gtk_widget_set_hexpand(GTK_WIDGET(inner), TRUE);
                gtk_widget_set_vexpand(GTK_WIDGET(inner), TRUE);
                gtk_widget_set_halign(bg, GTK_ALIGN_FILL);
                gtk_widget_set_valign(bg, GTK_ALIGN_FILL);
                gtk_widget_set_hexpand(bg, TRUE);
                gtk_widget_set_vexpand(bg, TRUE);
                gtk_container_add(GTK_CONTAINER(inner), bg);
                overlay = inner;

                // Outer overlay: inner is the main child (drawn first, bottom
                // of the stack); WebKitWebView is the overlay child (drawn
                // last, top of the stack).
                gtk_container_add(GTK_CONTAINER(outer), GTK_WIDGET(inner));
                gtk_overlay_add_overlay(outer, child);
                gtk_widget_set_halign(child, GTK_ALIGN_FILL);
                gtk_widget_set_valign(child, GTK_ALIGN_FILL);
                gtk_widget_set_hexpand(child, TRUE);
                gtk_widget_set_vexpand(child, TRUE);

                // Make WebKit render with a transparent backdrop so the
                // pinhole layer underneath is visible through any HTML
                // element that sets `background: transparent`.
                if (WEBKIT_IS_WEB_VIEW(child)) {
                    GdkRGBA transparent = {0.0, 0.0, 0.0, 0.0};
                    webkit_web_view_set_background_color(
                        WEBKIT_WEB_VIEW(child), &transparent);
                }

                g_object_unref(child);
                gtk_container_add(GTK_CONTAINER(gtk_win), GTK_WIDGET(outer));
                gtk_widget_show_all(gtk_win);
            }
        }
#endif

        connect_close_signals();

#ifdef _WIN32
        // Setup (bind + init scripts) is complete.  If the UI loop is
        // already running (child window), load now; otherwise run() will.
        if (g_run_loop_active.load()) {
            flush_navigation();
        }
#endif
    }

    // ── Initial navigation ──────────────────────────────────────────────
    //
    // WebView2 applies AddScriptToExecuteOnDocumentCreated (webview_bind /
    // webview_init) only to navigations issued AFTER the script was added,
    // and webview/webview pumps the message loop while adding each script —
    // so a navigation issued in the constructor commits before the IPC
    // binding exists (window.__anyar_ipc__ undefined).  On Win32 the first
    // URL is therefore held until setup is done.  WebKitGTK does not load
    // until the GTK loop runs, so Linux navigates immediately.
#ifdef _WIN32
    static inline std::atomic<bool> g_run_loop_active{false};
    std::string pending_url;
#endif

    // webview/webview pumps nested message loops inside webview_create()
    // and every webview_bind()/webview_init() (waiting for WebView2
    // callbacks); those loops consume WM_QUIT.  A window:close-all arriving
    // meanwhile (e.g. over HTTP while the first window is still being set
    // up) would be lost — restore it.  No-op off Windows.
    void* browser_controller() const {
        if (!wv || destroyed) return nullptr;
        return webview_get_native_handle(wv, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER);
    }

    // Win32: let SharedBuffers allocate WebView2 shared memory from this
    // window's environment (first window wins).  No-op elsewhere.
    void note_controller() {
#ifdef _WIN32
        platform::note_webview_controller(browser_controller());
#endif
    }

    void after_nested_pump() {
#ifdef _WIN32
        platform::repost_quit_if_requested();
#endif
    }

    void start_navigation(const std::string& url) {
#ifdef _WIN32
        pending_url = url;
#else
        webview_navigate(wv, url.c_str());
#endif
    }

#ifdef _WIN32
    void flush_navigation() {
        if (wv && !pending_url.empty()) {
            std::string url = std::move(pending_url);
            pending_url.clear();
            webview_navigate(wv, url.c_str());
        }
    }
#endif

    ~Impl() {
#ifdef _WIN32
        pinholes_window_destroyed();
#else
        for (auto& [id, pin] : pinholes) {
            pin->notify_window_destroyed();
        }
#endif
        pinholes.clear();

        // Re-enable parent if this was a modal child
        restore_parent();
#ifdef _WIN32
        // Unlike GTK, a natively destroyed HWND leaves the engine (and its
        // WebView2 COM objects) alive with `destroyed == true` — always free
        // it here.  webview_destroy() pumps a nested loop that swallows
        // WM_QUIT, hence the re-post.
        if (wv) {
            disconnect_close_signals();
            destroyed = true;
            webview_destroy(wv);
            wv = nullptr;
            platform::repost_quit_if_requested();
        }
#else
        if (wv && !destroyed) {
            // Normal path: Window is being deleted without the GTK
            // "destroy" signal having fired (e.g. shared_ptr dropped).
            // Mark destroyed FIRST so that any stale g_idle_add callbacks
            // processed during deplete_run_loop_event_queue() inside
            // webview_destroy() will see is_destroyed()==true and bail out.
            disconnect_close_signals();
            destroyed = true;
#ifdef __linux__
            // Drain a bounded number of stale g_idle_add callbacks
            // BEFORE webview_destroy().  This prevents webview’s
            // internal deplete_run_loop_event_queue() from processing
            // stale callbacks that reference the destroyed widget.
            // Cap iterations to avoid hanging under xvfb.
            for (int i = 0; i < 200 && g_main_context_pending(nullptr); ++i) {
                g_main_context_iteration(nullptr, FALSE);
            }
#endif
            webview_destroy(wv);
            wv = nullptr;
        }
        // If destroyed == true, the GTK "destroy" signal already fired;
        // the library handled cleanup and we set wv = nullptr there.
#endif
    }

    // Deferred-show state for WindowCreateOptions constructor
    int stored_width = 800;
    int stored_height = 600;
    webview_hint_t stored_hint = WEBVIEW_HINT_NONE;
    int stored_min_width = 0;
    int stored_min_height = 0;
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

        // focus-in-event: fires when the window gains keyboard focus
        focus_in_handler_id = g_signal_connect(
            G_OBJECT(win), "focus-in-event",
            G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer user_data) -> gboolean {
                auto* self = static_cast<Impl*>(user_data);
                if (self->on_focus) {
                    self->on_focus();
                }
                return FALSE; // propagate the event
            }),
            this);

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
                for (auto& [id, pin] : self->pinholes) {
                    pin->notify_window_destroyed();
                }
                if (self->owns_run_loop && self->wv) {
                    webview_terminate(self->wv);
                }
                self->destroyed = true;
                self->wv = nullptr;  // library handles cleanup — prevent double-destroy in ~Impl
                self->delete_event_handler_id = 0;
                self->destroy_handler_id = 0;
                // Re-enable parent if this was a modal child
                if (self->is_modal && self->parent_window) {
                    gtk_widget_set_sensitive(
                        GTK_WIDGET(self->parent_window), TRUE);
                    self->parent_window = nullptr;
                }
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

        // window-state-event: pause/resume pinholes on OS minimize/restore.
        window_state_handler_id = g_signal_connect(
            G_OBJECT(win), "window-state-event",
            G_CALLBACK(+[](GtkWidget*, GdkEventWindowState* ev,
                           gpointer user_data) -> gboolean {
                auto* self = static_cast<Impl*>(user_data);
                if (ev->changed_mask & GDK_WINDOW_STATE_ICONIFIED) {
                    bool minimized = static_cast<bool>(
                        ev->new_window_state & GDK_WINDOW_STATE_ICONIFIED);
                    for (auto& [wid, pin] : self->pinholes) {
                        pin->set_window_active(!minimized);
                    }
                }
                return FALSE;
            }), this);
    }

    void disconnect_close_signals() {
        GtkWidget* win = static_cast<GtkWidget*>(webview_get_window(wv));
        if (!win || destroyed) return;
        if (focus_in_handler_id) {
            g_signal_handler_disconnect(G_OBJECT(win), focus_in_handler_id);
            focus_in_handler_id = 0;
        }
        if (delete_event_handler_id) {
            g_signal_handler_disconnect(G_OBJECT(win), delete_event_handler_id);
            delete_event_handler_id = 0;
        }
        if (destroy_handler_id) {
            g_signal_handler_disconnect(G_OBJECT(win), destroy_handler_id);
            destroy_handler_id = 0;
        }
        if (window_state_handler_id) {
            g_signal_handler_disconnect(G_OBJECT(win), window_state_handler_id);
            window_state_handler_id = 0;
        }
    }

    void set_parent(Impl& parent_impl) {
        auto* child_win = gtk_window();
        auto* parent_win = parent_impl.gtk_window();
        if (child_win && parent_win) {
            // Use DIALOG hint + keep-above instead of transient_for.
            // transient_for causes the WM to group-move parent+child and
            // prevents moving the child when the parent is fullscreen.
            gtk_window_set_type_hint(
                child_win, GDK_WINDOW_TYPE_HINT_DIALOG);
            gtk_window_set_keep_above(child_win, TRUE);
            parent_window = parent_win;
        }
    }

    void set_modal(bool modal) {
        auto* win = gtk_window();
        if (!win) return;
        is_modal = modal;
        if (modal && parent_window) {
            // Disable parent input instead of gtk_window_set_modal
            // (which only works with transient_for and causes group-move).
            gtk_widget_set_sensitive(
                GTK_WIDGET(parent_window), FALSE);
        }
    }

    // Pointer to the parent GtkWindow (non-owning) for manual modal management
    GtkWindow* parent_window = nullptr;
    bool is_modal = false;

    void restore_parent() {
        if (is_modal && parent_window) {
            gtk_widget_set_sensitive(
                GTK_WIDGET(parent_window), TRUE);
            parent_window = nullptr;
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

        if (parent_window) {
            // Center on parent
            int px, py, pw, ph;
            gtk_window_get_position(parent_window, &px, &py);
            gtk_window_get_size(parent_window, &pw, &ph);
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

    void reorder_pinholes_by_z() {
        // Sort pinholes by z_index (ascending).  GtkOverlay draws last-added
        // on top, so the highest-z pinhole must be re-inserted last.
        std::vector<std::shared_ptr<Pinhole>> sorted;
        sorted.reserve(pinholes.size());
        for (auto& [id, p] : pinholes) sorted.push_back(p);
        std::sort(sorted.begin(), sorted.end(),
            [](const std::shared_ptr<Pinhole>& a,
               const std::shared_ptr<Pinhole>& b) {
                return a->z_index() < b->z_index();
            });
        // Dispatch re-insertion to the GTK main thread.
        auto* payload = new std::vector<std::shared_ptr<Pinhole>>(std::move(sorted));
        g_idle_add(+[](gpointer data) -> gboolean {
            auto* pins = static_cast<std::vector<std::shared_ptr<Pinhole>>*>(data);
            for (auto& pin : *pins) pin->reorder_in_overlay();
            delete pins;
            return G_SOURCE_REMOVE;
        }, payload);
    }

    void set_size(int w, int h) {
        if (wv) {
            webview_set_size(wv, w, h, WEBVIEW_HINT_NONE);
        }
    }
#elif defined(_WIN32)
    static constexpr const wchar_t* kImplProp = L"anyar.window.impl";

    HWND hwnd() const {
        return static_cast<HWND>(native_handle());
    }

    // Run a user callback from inside the window procedure: exceptions must
    // not unwind through Win32 frames.
    template <typename F>
    static auto guarded(const char* what, F&& fn, decltype(fn()) fallback) {
        try {
            return fn();
        } catch (const std::exception& e) {
            std::cerr << "[LibAnyar] " << what << " threw: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[LibAnyar] " << what << " threw" << std::endl;
        }
        return fallback;
    }

    static LRESULT CALLBACK subclass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = static_cast<Impl*>(GetPropW(h, kImplProp));
        if (!self || !self->orig_wndproc) {
            return DefWindowProcW(h, msg, wp, lp);
        }
        WNDPROC orig = self->orig_wndproc;

        switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && self->on_focus) {
                guarded("focus handler", [&] { self->on_focus(); return 0; }, 0);
            }
            break;

        case WM_CLOSE:  // title-bar X, Alt+F4
            if (!self->closable) return 0;
            if (self->on_close_requested &&
                !guarded("close-requested handler",
                         [&] { return self->on_close_requested(); }, true)) {
                return 0;
            }
            // Re-enable the owner BEFORE this window goes away so Windows
            // hands activation back to it rather than to another app.
            self->restore_parent();
            break;

        case WM_SIZE: {  // pause/resume pinholes on minimize/restore
            bool now_minimized = (wp == SIZE_MINIMIZED);
            if (now_minimized != self->minimized) {
                self->minimized = now_minimized;
                for (auto& [pid, pin] : self->pinholes) {
                    pin->set_window_active(!now_minimized);
                }
            }
            break;
        }

        case WM_DESTROY:
            self->on_native_destroy();  // unhooks; engine still sees WM_DESTROY
            break;
        }
        return CallWindowProcW(orig, h, msg, wp, lp);
    }

    // Mirrors the GTK "destroy" handler.
    void on_native_destroy() {
        pinholes_window_destroyed();
        if (owns_run_loop) {
            platform::request_quit();
        }
        disconnect_close_signals();
        destroyed = true;
        restore_parent();
        // Defer on_close to avoid re-entrant destruction.  NOT for the window
        // that owns the run loop: posted messages are delivered before
        // WM_QUIT, so on_close would delete this Window while run() is still
        // on its stack.  App::run() tears the main window down after the
        // loop returns instead.
        if (on_close && !owns_run_loop) {
            post_to_main_thread(std::move(on_close));
        }
        on_close = nullptr;
    }

    void connect_close_signals() {
        HWND h = hwnd();
        if (!h || hooked_hwnd) return;
        SetPropW(h, kImplProp, this);
        orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&subclass_proc)));
        hooked_hwnd = h;
    }

    void disconnect_close_signals() {
        if (!hooked_hwnd) return;
        if (IsWindow(hooked_hwnd)) {
            // Only restore if nobody subclassed on top of us meanwhile.
            if (GetWindowLongPtrW(hooked_hwnd, GWLP_WNDPROC) ==
                reinterpret_cast<LONG_PTR>(&subclass_proc)) {
                SetWindowLongPtrW(hooked_hwnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(orig_wndproc));
            }
            RemovePropW(hooked_hwnd, kImplProp);
        }
        hooked_hwnd = nullptr;
        orig_wndproc = nullptr;
    }

    void set_parent(Impl& parent_impl) {
        HWND child = hwnd();
        HWND parent = parent_impl.hwnd();
        if (child && parent) {
            // Owned window: always above its owner, minimizes with it.
            SetWindowLongPtrW(child, GWLP_HWNDPARENT,
                              reinterpret_cast<LONG_PTR>(parent));
            parent_window = parent;
        }
    }

    void set_modal(bool modal) {
        if (!hwnd()) return;
        is_modal = modal;
        if (modal && parent_window) {
            EnableWindow(parent_window, FALSE);
        }
    }

    void restore_parent() {
        if (is_modal && parent_window) {
            if (IsWindow(parent_window)) EnableWindow(parent_window, TRUE);
            parent_window = nullptr;
        }
    }

    void set_enabled(bool enabled) {
        if (HWND h = hwnd()) EnableWindow(h, enabled ? TRUE : FALSE);
    }

    void set_always_on_top(bool on_top) {
        if (HWND h = hwnd()) {
            SetWindowPos(h, on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    void set_position(int x, int y) {
        if (HWND h = hwnd()) {
            SetWindowPos(h, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // Outer frame size for a client area given in CSS/DIP pixels (matches
    // how webview_set_size() scales by the window DPI).
    SIZE frame_size_for_client(int width, int height) const {
        HWND h = hwnd();
        UINT dpi = h ? GetDpiForWindow(h) : USER_DEFAULT_SCREEN_DPI;
        RECT r{0, 0, MulDiv(width, dpi, USER_DEFAULT_SCREEN_DPI),
               MulDiv(height, dpi, USER_DEFAULT_SCREEN_DPI)};
        if (h) {
            AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE)),
                                     FALSE, static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE)),
                                     dpi);
        }
        return SIZE{r.right - r.left, r.bottom - r.top};
    }

    // Size the window to @p frame and center it on the owner, or on the
    // work area of the monitor it is on.
    void place_centered(SIZE frame) {
        HWND h = hwnd();
        if (!h) return;
        RECT area{};
        if (parent_window && IsWindow(parent_window)) {
            GetWindowRect(parent_window, &area);
        } else {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
            area = mi.rcWork;
        }
        int x = area.left + ((area.right - area.left) - frame.cx) / 2;
        int y = area.top + ((area.bottom - area.top) - frame.cy) / 2;
        SetWindowPos(h, nullptr, x, y, frame.cx, frame.cy,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void center_on_parent() {
        HWND h = hwnd();
        if (!h) return;
        if (needs_show) {
            // Size not applied yet (the window is 0×0 until show_window()).
            center_pending = true;
            return;
        }
        RECT r{};
        GetWindowRect(h, &r);
        place_centered(SIZE{r.right - r.left, r.bottom - r.top});
    }

    void focus() {
        if (HWND h = hwnd()) {
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
        }
    }

    void set_size(int w, int h) {
        if (wv) webview_set_size(wv, w, h, WEBVIEW_HINT_NONE);
    }
#else
    // Stubs for other platforms (macOS: Phase 7)
    void connect_close_signals() {}
    void disconnect_close_signals() {}
    void restore_parent() {}
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
    impl_->start_navigation(url.str());

    // Inject the port number so the JS bridge can find the IPC endpoint
    std::ostringstream init_js;
    init_js << "window.__LIBANYAR_PORT__ = " << server_port << ";";
    webview_init(impl_->wv, init_js.str().c_str());
    impl_->after_nested_pump();
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
    impl_->start_navigation(url.str());  // deferred on Win32, see Impl

    // Inject port + window label for the JS bridge
    std::ostringstream init_js;
    init_js << "window.__LIBANYAR_PORT__ = " << server_port << ";"
            << "window.__LIBANYAR_WINDOW_LABEL__ = '"
            << opts.label << "';";
    webview_init(impl_->wv, init_js.str().c_str());
    impl_->after_nested_pump();

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
    impl_->owns_run_loop = true;
#ifdef _WIN32
    // All setup, including App::on_window_ready scripts, is done: load now.
    Impl::g_run_loop_active.store(true);
    impl_->flush_navigation();
    // A quit requested during setup (swallowed by a nested pump) must end
    // the loop right away.
    platform::repost_quit_if_requested();
#endif
    webview_run(impl_->wv);
    impl_->owns_run_loop = false;
#ifdef _WIN32
    Impl::g_run_loop_active.store(false);
    // The loop has exited; a stale quit request must not kill later loops
    // (webview_destroy() depletion during shutdown, another App instance).
    platform::clear_quit_request();
#endif
}

void Window::terminate() {
#ifdef _WIN32
    // webview_terminate() is PostQuitMessage() on the CALLING thread — not
    // thread-safe on Win32, unlike GTK.  Route it to the UI thread.
    platform::request_quit();
#else
    webview_terminate(impl_->wv);
#endif
}

void Window::destroy() {
    if (impl_->wv && !impl_->destroyed) {
#ifdef _WIN32
        impl_->pinholes_window_destroyed();
#else
        for (auto& [id, pin] : impl_->pinholes) {
            pin->notify_window_destroyed();
        }
#endif
        if (impl_->owns_run_loop) {
            terminate();
        }
        impl_->disconnect_close_signals();
        impl_->destroyed = true;

        // The native destroy handler (now disconnected above) normally
        // re-enables the parent and defers the on_close callback.  Since we
        // disconnected it, we must do both manually before webview_destroy()
        // so that the parent window remains responsive.
        impl_->restore_parent();

#ifdef __linux__
        // Drain a bounded number of stale g_idle_add callbacks
        // BEFORE webview_destroy() so that deplete_run_loop_event_queue()
        // inside the webview destructor won't process stale callbacks.
        // Cap iterations to avoid hanging under xvfb.
        for (int i = 0; i < 200 && g_main_context_pending(nullptr); ++i) {
            g_main_context_iteration(nullptr, FALSE);
        }
#endif

        webview_destroy(impl_->wv);
        impl_->wv = nullptr;

#ifdef _WIN32
        // webview_destroy() depletes the queue in a nested loop that
        // swallows WM_QUIT.
        platform::repost_quit_if_requested();

        // Defer on_close (see Impl::on_native_destroy for why the run-loop
        // owner is excluded).
        if (impl_->on_close && !impl_->owns_run_loop) {
            post_to_main_thread(std::move(impl_->on_close));
        }
        impl_->on_close = nullptr;
#elif defined(__linux__)
        // Defer the on_close callback to avoid re-entrant destruction:
        // on_close → windows_.erase() could drop the last shared_ptr →
        // ~Impl while we are still inside Window::destroy().
        if (impl_->on_close) {
            auto* cb = new CloseHandler(std::move(impl_->on_close));
            impl_->on_close = nullptr;
            g_idle_add(+[](gpointer data) -> gboolean {
                auto* fn = static_cast<CloseHandler*>(data);
                (*fn)();
                delete fn;
                return G_SOURCE_REMOVE;
            }, cb);
        }
#endif
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
#ifdef _WIN32
    if (!impl_->pending_url.empty()) {
        impl_->pending_url = url;  // initial load not issued yet — replace it
        return;
    }
#endif
    if (impl_->wv) {
        webview_navigate(impl_->wv, url.c_str());
    }
}

void Window::dispatch(std::function<void()> fn) {
    if (!impl_->wv || impl_->destroyed) return;
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

void* Window::browser_controller() const {
    return impl_->browser_controller();
}

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

// ── Lifecycle Event Handlers ────────────────────────────────────────────────

void Window::set_on_focus(FocusHandler handler) {
    impl_->on_focus = std::move(handler);
}

void Window::set_on_close(CloseHandler handler) {
    impl_->on_close = std::move(handler);
}

void Window::set_on_close_requested(CloseRequestedHandler handler) {
    impl_->on_close_requested = std::move(handler);
}

void Window::set_close_confirmation(const std::string& message,
                                    const std::string& title) {
#ifdef __linux__
    if (message.empty()) {
        // Disable: remove the close-requested handler
        impl_->on_close_requested = nullptr;
        return;
    }

    // Capture copies for the lambda
    std::string msg = message;
    std::string ttl = title;

    impl_->on_close_requested = [msg, ttl]() -> bool {
        GtkWidget* dlg = gtk_message_dialog_new(
            nullptr,
            static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_NONE,
            "%s", msg.c_str());
        gtk_window_set_title(GTK_WINDOW(dlg), ttl.c_str());
        gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
        gtk_dialog_add_button(GTK_DIALOG(dlg), "Close", GTK_RESPONSE_OK);

        gint result = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        while (gtk_events_pending()) gtk_main_iteration();

        return result == GTK_RESPONSE_OK;
    };
#elif defined(_WIN32)
    if (message.empty()) {
        impl_->on_close_requested = nullptr;
        return;
    }

    std::wstring msg = win32::widen(message);
    std::wstring ttl = win32::widen(title);
    Impl* impl = impl_.get();  // the handler is owned by *impl

    impl_->on_close_requested = [msg, ttl, impl]() -> bool {
        // Runs inside WM_CLOSE on the UI thread; MessageBoxW pumps a modal
        // loop, so posted main-thread work keeps flowing meanwhile.
        return MessageBoxW(impl->hwnd(), msg.c_str(), ttl.c_str(),
                           MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
    };
#endif
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
    impl_->after_nested_pump();
}

void Window::return_result(const std::string& seq, int status,
                           const std::string& result) {
    if (impl_->wv && !impl_->destroyed) {
        webview_return(impl_->wv, seq.c_str(), status, result.c_str());
    }
}

void Window::init(const std::string& js) {
    if (impl_->wv) {
        webview_init(impl_->wv, js.c_str());
        impl_->after_nested_pump();
    }
}

// ── Pinhole Native Overlay ───────────────────────────────────────────────────

std::shared_ptr<Pinhole> Window::create_pinhole(const std::string& id,
                                                 const PinholeOptions& opts)
{
    auto pin = std::shared_ptr<Pinhole>(new Pinhole());
#ifdef __linux__
    // Build the JS eval function up front so the canvas fallback path (4g.5)
    // is wired immediately during platform_init() rather than via a separate
    // post-construction call.
    webview_t wv = impl_->wv;
    auto eval_fn = [wv](const std::string& js) {
        if (wv) webview_eval(wv, js.c_str());
    };
    pin->platform_init(id, opts, impl_->overlay, std::move(eval_fn));
    impl_->pinholes.emplace(id, pin);
    // Wire set_z_index() → Window reorder so z-order changes propagate.
    {
        Impl* impl_ptr = impl_.get();
        pin->set_reorder_callback([impl_ptr]() { impl_ptr->reorder_pinholes_by_z(); });
    }
    // Inject the JS tracking bootstrap once per window
    if (!impl_->pinhole_js_injected) {
        impl_->pinhole_js_injected = true;
        const std::string& js = Pinhole::tracking_js();
        if (!js.empty() && impl_->wv) {
            webview_init(impl_->wv, js.c_str());
        }
    }
#elif defined(_WIN32)
    // Per-window DirectComposition host below the (now transparent) webview,
    // created on first use; nullptr if D3D11/DComp are unavailable → the
    // pinhole uses the canvas fallback.
    if (!impl_->pinhole_host && impl_->wv && !impl_->destroyed) {
        impl_->pinhole_host = create_pinhole_host(
            webview_get_native_handle(impl_->wv, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET),
            impl_->browser_controller());
    }
    // Fallback canvas JS may be produced off the UI thread (set_rect from an
    // IPC fiber); webview_eval must run on it.  The token stops evals queued
    // after the window died.
    Impl* impl_ptr = impl_.get();
    std::shared_ptr<std::atomic<bool>> alive = impl_->alive_token;
    auto eval_fn = [impl_ptr, alive](const std::string& js) {
        auto run = [impl_ptr, alive, js] {
            if (alive->load() && impl_ptr->wv && !impl_ptr->destroyed) {
                webview_eval(impl_ptr->wv, js.c_str());
            }
        };
        if (platform::is_main_thread()) run();
        else post_to_main_thread(run);
    };
    pin->platform_init(id, opts, &impl_->pinhole_host, std::move(eval_fn));
    impl_->pinholes.emplace(id, pin);
    Pinhole* raw = pin.get();  // callback is owned by the pinhole itself
    pin->set_reorder_callback([raw]() { raw->reorder_in_overlay(); });

    // Inject the JS tracking bootstrap once per window
    if (!impl_->pinhole_js_injected) {
        impl_->pinhole_js_injected = true;
        if (impl_->wv) {
            webview_init(impl_->wv, Pinhole::tracking_js().c_str());
            impl_->after_nested_pump();
        }
    }
#else
    // Stub pinhole (is_native() == false); tracked so IPC lookups resolve.
    pin->platform_init(id, opts, nullptr, {});
    impl_->pinholes.emplace(id, pin);
#endif
    return pin;
}

std::shared_ptr<Pinhole> Window::find_pinhole(const std::string& id) const {
    auto it = impl_->pinholes.find(id);
    if (it != impl_->pinholes.end()) return it->second;
    return nullptr;
}

} // namespace anyar
