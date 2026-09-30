// LibAnyar — process-level platform hooks (Windows)

#include "platform.h"
#include "win32_util.h"

#include <vector>

namespace anyar::platform {

// Nothing to sanitise; per-monitor DPI awareness and COM (STA) are set up by
// webview/webview when the first window is created.
void init_process() {}

std::filesystem::path executable_path() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) return std::filesystem::path(std::wstring(buf.data(), n));
        buf.resize(buf.size() * 2);  // truncated (long path) — retry
    }
}

// WebView2 custom schemes must be registered when the CoreWebView2
// environment is created, which webview/webview does internally; until that
// is wired up, SharedBuffers are served over HTTP.
bool has_shm_uri_scheme() { return false; }

} // namespace anyar::platform
