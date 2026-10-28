// LibAnyar — debug tooling, Linux (WebKitGTK): webview capture + synthetic input.

#include "platform.h"

#include <cairo.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <cctype>
#include <memory>
#include <string>
#include <utility>

namespace anyar::platform {

namespace {

cairo_status_t append_png(void* closure, const unsigned char* data, unsigned int length) {
    auto* out = static_cast<std::vector<uint8_t>*>(closure);
    out->insert(out->end(), data, data + length);
    return CAIRO_STATUS_SUCCESS;
}

void on_snapshot(GObject* source, GAsyncResult* result, gpointer user_data) {
    std::unique_ptr<SnapshotCallback> done(static_cast<SnapshotCallback*>(user_data));
    GError* err = nullptr;
    cairo_surface_t* surface = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &err);
    if (!surface) {
        std::string msg = err ? err->message : "snapshot failed";
        if (err) g_error_free(err);
        (*done)({}, msg);
        return;
    }
    WebviewSnapshot snap;
    snap.width = cairo_image_surface_get_width(surface);
    snap.height = cairo_image_surface_get_height(surface);
    cairo_status_t st = cairo_surface_write_to_png_stream(surface, append_png, &snap.png);
    cairo_surface_destroy(surface);
    if (st != CAIRO_STATUS_SUCCESS) {
        (*done)({}, std::string("PNG encoding failed: ") + cairo_status_to_string(st));
        return;
    }
    (*done)(std::move(snap), {});
}

GdkModifierType modifiers(const InputEvent& ev) {
    int m = 0;
    if (ev.shift) m |= GDK_SHIFT_MASK;
    if (ev.ctrl) m |= GDK_CONTROL_MASK;
    if (ev.alt) m |= GDK_MOD1_MASK;
    if (ev.meta) m |= GDK_META_MASK;
    return GdkModifierType(m);
}

/// DOM key name ("Enter", "ArrowDown", "a") → GDK keyval.
guint keyval_for(const std::string& key) {
    static const std::pair<const char*, guint> names[] = {
        {"Enter", GDK_KEY_Return}, {"Escape", GDK_KEY_Escape}, {"Tab", GDK_KEY_Tab},
        {"Backspace", GDK_KEY_BackSpace}, {"Delete", GDK_KEY_Delete}, {"Space", GDK_KEY_space},
        {" ", GDK_KEY_space}, {"ArrowUp", GDK_KEY_Up}, {"ArrowDown", GDK_KEY_Down},
        {"ArrowLeft", GDK_KEY_Left}, {"ArrowRight", GDK_KEY_Right}, {"Home", GDK_KEY_Home},
        {"End", GDK_KEY_End}, {"PageUp", GDK_KEY_Page_Up}, {"PageDown", GDK_KEY_Page_Down},
        {"Shift", GDK_KEY_Shift_L}, {"Control", GDK_KEY_Control_L}, {"Alt", GDK_KEY_Alt_L},
    };
    for (const auto& [name, val] : names)
        if (key == name) return val;
    if (key.size() > 1 && key[0] == 'F' && std::isdigit(static_cast<unsigned char>(key[1])))
        return GDK_KEY_F1 + guint(std::stoi(key.substr(1)) - 1);
    // Single UTF-8 character.
    gunichar ch = g_utf8_get_char_validated(key.c_str(), -1);
    if (ch != gunichar(-1) && ch != gunichar(-2) && g_utf8_strlen(key.c_str(), -1) == 1)
        return gdk_unicode_to_keyval(ch);
    return gdk_keyval_from_name(key.c_str());
}

struct Target {
    GtkWidget* widget = nullptr;
    GdkWindow* window = nullptr;
    double ox = 0, oy = 0;   // widget origin inside `window`
};

Target target_for(void* engine) {
    Target t;
    t.widget = GTK_WIDGET(engine);
    t.window = gtk_widget_get_window(t.widget);
    if (t.window && !gtk_widget_get_has_window(t.widget)) {
        GtkAllocation a;
        gtk_widget_get_allocation(t.widget, &a);
        t.ox = a.x;
        t.oy = a.y;
    }
    return t;
}

GdkDevice* pointer(GdkWindow* w) {
    return gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(w)));
}

void send_button(const Target& t, const InputEvent& ev, GdkEventType type) {
    GdkEvent* e = gdk_event_new(type);
    e->button.window = GDK_WINDOW(g_object_ref(t.window));
    e->button.send_event = TRUE;
    e->button.time = GDK_CURRENT_TIME;
    e->button.x = ev.x + t.ox;
    e->button.y = ev.y + t.oy;
    e->button.button = guint(ev.button);
    e->button.state = modifiers(ev);
    gdk_event_set_device(e, pointer(t.window));
    gtk_main_do_event(e);
    gdk_event_free(e);
}

void send_key(const Target& t, guint keyval, GdkModifierType state, GdkEventType type) {
    GdkEvent* e = gdk_event_new(type);
    e->key.window = GDK_WINDOW(g_object_ref(t.window));
    e->key.send_event = TRUE;
    e->key.time = GDK_CURRENT_TIME;
    e->key.keyval = keyval;
    e->key.state = state;
    GdkKeymapKey* keys = nullptr;
    gint n = 0;
    if (gdk_keymap_get_entries_for_keyval(gdk_keymap_get_for_display(gdk_window_get_display(t.window)),
                                          keyval, &keys, &n) && n > 0) {
        e->key.hardware_keycode = guint16(keys[0].keycode);
        e->key.group = guint8(keys[0].group);
    }
    g_free(keys);
    GdkSeat* seat = gdk_display_get_default_seat(gdk_window_get_display(t.window));
    gdk_event_set_device(e, gdk_seat_get_keyboard(seat));
    gtk_main_do_event(e);
    gdk_event_free(e);
}

} // namespace

bool can_capture_webview() { return true; }
bool can_inject_webview_input() { return true; }

void capture_webview(void* engine, SnapshotCallback done) {
    if (!engine || !WEBKIT_IS_WEB_VIEW(engine)) {
        done({}, "no webview");
        return;
    }
    auto box = std::make_unique<SnapshotCallback>(std::move(done));
    webkit_web_view_get_snapshot(WEBKIT_WEB_VIEW(engine), WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr, on_snapshot, box.release());
}

std::string inject_webview_input(void* engine, const InputEvent& ev) {
    if (!engine || !WEBKIT_IS_WEB_VIEW(engine)) return "no webview";
    Target t = target_for(engine);
    if (!t.window) return "webview is not realized yet";

    using T = InputEvent::Type;
    switch (ev.type) {
    case T::Move: {
        GdkEvent* e = gdk_event_new(GDK_MOTION_NOTIFY);
        e->motion.window = GDK_WINDOW(g_object_ref(t.window));
        e->motion.send_event = TRUE;
        e->motion.time = GDK_CURRENT_TIME;
        e->motion.x = ev.x + t.ox;
        e->motion.y = ev.y + t.oy;
        e->motion.state = modifiers(ev);
        gdk_event_set_device(e, pointer(t.window));
        gtk_main_do_event(e);
        gdk_event_free(e);
        return {};
    }
    case T::Down:
        send_button(t, ev, GDK_BUTTON_PRESS);
        return {};
    case T::Up:
        send_button(t, ev, GDK_BUTTON_RELEASE);
        return {};
    case T::Click:
        send_button(t, ev, GDK_BUTTON_PRESS);
        send_button(t, ev, GDK_BUTTON_RELEASE);
        return {};
    case T::Scroll: {
        GdkEvent* e = gdk_event_new(GDK_SCROLL);
        e->scroll.window = GDK_WINDOW(g_object_ref(t.window));
        e->scroll.send_event = TRUE;
        e->scroll.time = GDK_CURRENT_TIME;
        e->scroll.x = ev.x + t.ox;
        e->scroll.y = ev.y + t.oy;
        e->scroll.direction = GDK_SCROLL_SMOOTH;
        e->scroll.delta_x = ev.dx;
        e->scroll.delta_y = ev.dy;
        e->scroll.state = modifiers(ev);
        gdk_event_set_device(e, pointer(t.window));
        gtk_main_do_event(e);
        gdk_event_free(e);
        return {};
    }
    case T::Key: {
        guint kv = keyval_for(ev.key);
        if (!kv || kv == GDK_KEY_VoidSymbol) return "unknown key: " + ev.key;
        gtk_widget_grab_focus(t.widget);
        send_key(t, kv, modifiers(ev), GDK_KEY_PRESS);
        send_key(t, kv, modifiers(ev), GDK_KEY_RELEASE);
        return {};
    }
    case T::Text: {
        gtk_widget_grab_focus(t.widget);
        for (const char* p = ev.text.c_str(); *p; p = g_utf8_next_char(p)) {
            guint kv = gdk_unicode_to_keyval(g_utf8_get_char(p));
            send_key(t, kv, modifiers(ev), GDK_KEY_PRESS);
            send_key(t, kv, modifiers(ev), GDK_KEY_RELEASE);
        }
        return {};
    }
    }
    return "unsupported input type";
}

} // namespace anyar::platform
