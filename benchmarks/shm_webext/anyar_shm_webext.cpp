// LibAnyar — PROTOTYPE WebKitGTK WebProcess extension for fast SharedBuffer reads.
//
// Why: `anyar-shm://` goes through WebKitURISchemeRequest, which streams the
// response to the WebProcess in fixed 8 KB chunks (one async read + one IPC
// message each) — ~370 MB/s, ~21 ms per 1080p RGBA frame, regardless of fetch
// vs XHR.  This extension runs *inside* the WebProcess, maps the POSIX shm
// segment directly, and copies it into a JS Uint8Array with a single memcpy.
//
// JS API (prototype):  window.__anyar_shm_read(name) → Uint8Array (snapshot)
//
// True zero-copy (jsc_value_new_array_buffer over the mmap) is NOT possible:
// the WebProcess aborts with "Disabling Primitive gigacage is forbidden" —
// JSC only allows ArrayBuffer storage inside its Gigacage.  So we do ONE
// memcpy from the (cached) mapping into a JSC-allocated Uint8Array, which
// still skips the ~1000 × 8 KB IPC round-trips of the URI-scheme path.
//
// The UI process pid is passed as initialization user data (int32) because
// SharedBuffer names shm segments "/anyar_<pid>_<name>".

#include <webkit2/webkit-web-extension.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

namespace {

int g_ui_pid = 0;

struct Mapping {
    void*  addr = nullptr;
    size_t size = 0;
    ino_t  ino  = 0;
};

// name → mapping.  Re-mapped when the segment is recreated (inode/size change).
std::map<std::string, Mapping> g_maps;

const Mapping* get_mapping(const std::string& name) {
    std::string path = "/anyar_" + std::to_string(g_ui_pid) + "_" + name;
    int fd = shm_open(path.c_str(), O_RDONLY, 0);
    if (fd < 0) return nullptr;
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return nullptr; }
    auto& m = g_maps[name];
    if (m.addr && (m.ino != st.st_ino || m.size != size_t(st.st_size))) {
        munmap(m.addr, m.size);
        m = {};
    }
    if (!m.addr) {
        void* addr = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        if (addr != MAP_FAILED) m = {addr, size_t(st.st_size), st.st_ino};
    }
    close(fd);
    return m.addr ? &m : nullptr;
}

JSCValue* shm_read(const char* name, gpointer) {
    JSCContext* ctx = jsc_context_get_current();
    const Mapping* m = get_mapping(name ? name : "");
    if (!m) {
        jsc_context_throw(ctx, ("__anyar_shm_read: no shared buffer '" +
                                std::string(name ? name : "") + "'").c_str());
        return jsc_value_new_undefined(ctx);
    }
    JSCValue* arr = jsc_value_new_typed_array(ctx, JSC_TYPED_ARRAY_UINT8, m->size);
    gsize len = 0;
    void* dst = jsc_value_typed_array_get_data(arr, &len);
    std::memcpy(dst, m->addr, std::min<size_t>(len, m->size));
    return arr;
}

void on_window_object_cleared(WebKitScriptWorld* world, WebKitWebPage*,
                              WebKitFrame* frame, gpointer) {
    JSCContext* ctx = webkit_frame_get_js_context_for_script_world(frame, world);
    JSCValue* fn = jsc_value_new_function(ctx, "__anyar_shm_read", G_CALLBACK(shm_read),
                                          nullptr, nullptr, JSC_TYPE_VALUE, 1, G_TYPE_STRING);
    JSCValue* global = jsc_context_get_global_object(ctx);
    jsc_value_object_set_property(global, "__anyar_shm_read", fn);
    g_object_unref(global);
    g_object_unref(fn);
    g_object_unref(ctx);
}

}  // namespace

extern "C" G_MODULE_EXPORT void
webkit_web_extension_initialize_with_user_data(WebKitWebExtension*, const GVariant* data) {
    g_ui_pid = g_variant_get_int32(const_cast<GVariant*>(data));
    g_signal_connect(webkit_script_world_get_default(), "window-object-cleared",
                     G_CALLBACK(on_window_object_cleared), nullptr);
}
