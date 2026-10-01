#include <anyar/plugins/clipboard_plugin.h>
#include <anyar/main_thread.h>
#include "../win32_util.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace anyar {

namespace {

/// RAII OpenClipboard/CloseClipboard.  Another process may hold the
/// clipboard briefly (clipboard managers, RDP), so retry for ~250 ms.
class ClipboardLock {
public:
    ClipboardLock() {
        for (int i = 0; i < 25; ++i) {
            if (OpenClipboard(nullptr)) {
                open_ = true;
                return;
            }
            Sleep(10);
        }
        throw std::runtime_error("Cannot open clipboard: " +
                                 win32::error_message(GetLastError()));
    }
    ~ClipboardLock() {
        if (open_) CloseClipboard();
    }
    ClipboardLock(const ClipboardLock&) = delete;
    ClipboardLock& operator=(const ClipboardLock&) = delete;

private:
    bool open_ = false;
};

} // namespace

void ClipboardPlugin::initialize(PluginContext& ctx) {
    auto& cmds = ctx.commands;

    // ── clipboard:read ──────────────────────────────────────────────────────
    cmds.add("clipboard:read", [](const json& /*args*/) -> json {
        return run_on_main_thread([]() -> json {
            ClipboardLock lock;
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            if (!data) {
                return "";
            }
            auto* text = static_cast<const wchar_t*>(GlobalLock(data));
            if (!text) {
                return "";
            }
            std::string result = win32::narrow(text, wcslen(text));
            GlobalUnlock(data);
            return result;
        });
    });

    // ── clipboard:write ─────────────────────────────────────────────────────
    cmds.add("clipboard:write", [](const json& args) -> json {
        std::string text = args.at("text").get<std::string>();

        return run_on_main_thread([&text]() -> json {
            std::wstring wide = win32::widen(text);
            const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);

            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (!mem) {
                throw std::runtime_error("clipboard:write: out of memory");
            }
            std::memcpy(GlobalLock(mem), wide.c_str(), bytes);
            GlobalUnlock(mem);

            ClipboardLock lock;
            EmptyClipboard();
            // On success the system owns `mem` (and it outlives the app).
            if (!SetClipboardData(CF_UNICODETEXT, mem)) {
                DWORD err = GetLastError();
                GlobalFree(mem);
                throw std::runtime_error("clipboard:write failed: " +
                                         win32::error_message(err));
            }
            return nullptr;
        });
    });
}

} // namespace anyar
