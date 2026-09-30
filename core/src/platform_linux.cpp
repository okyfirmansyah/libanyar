// LibAnyar — process-level platform hooks (Linux)

#include "platform.h"

#include <cstdlib>
#include <string>

namespace anyar::platform {

// Snap Environment Sanitisation
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
void init_process() {
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

std::filesystem::path executable_path() {
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : exe;
}

bool has_shm_uri_scheme() { return true; }

} // namespace anyar::platform
