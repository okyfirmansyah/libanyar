// LibAnyar — Native dialogs (Windows): IFileOpenDialog / IFileSaveDialog for
// file pickers, TaskDialogIndirect (MessageBoxW fallback) for message boxes.

// LibAsyik defines NOGDI for every target that links it; <commctrl.h> needs
// GDI types, and this file never uses LOG(ERROR).
#ifdef NOGDI
#undef NOGDI
#endif

#include <anyar/plugins/dialog_plugin.h>
#include <anyar/main_thread.h>
#include "../win32_util.h"

#include <commctrl.h>
#include <shobjidl.h>

#include <string>
#include <vector>

// TaskDialog lives in Common Controls v6, which is only loaded when the
// executable's manifest asks for it.  This object is always linked into the
// app (DialogPlugin is a built-in), so the dependency reaches the exe.
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace anyar {

// ── Helpers ─────────────────────────────────────────────────────────────────

namespace {

/// Balanced CoInitializeEx for the calling (UI) thread.  webview/webview
/// already initialised an STA there, so this is normally a cheap S_FALSE.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    HRESULT hr_;
};

/// Minimal COM smart pointer (avoids pulling in ATL/WRL).
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }

private:
    T* p_ = nullptr;
};

HWND dialog_owner() {
    // The active LibAnyar window (UI thread), so dialogs are modal to it.
    return GetActiveWindow();
}

std::string item_path(IShellItem* item) {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        out = win32::narrow(path, wcslen(path));
        CoTaskMemFree(path);
    }
    return out;
}

void set_folder(IFileDialog* dlg, const std::string& folder) {
    if (folder.empty()) return;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(win32::widen(folder).c_str(), nullptr,
                                              IID_PPV_ARGS(item.put())))) {
        dlg->SetFolder(item.get());
    }
}

/// Apply `[{name, extensions:[...]}]` as file-type filters.
void set_filters(IFileDialog* dlg, const json& filters) {
    if (!filters.is_array() || filters.empty()) return;

    // COMDLG_FILTERSPEC stores raw pointers — keep the strings alive.
    std::vector<std::wstring> names, specs;
    for (auto& f : filters) {
        std::wstring spec;
        if (f.contains("extensions") && f["extensions"].is_array()) {
            for (auto& ext : f["extensions"]) {
                if (!spec.empty()) spec += L';';
                spec += L"*." + win32::widen(ext.get<std::string>());
            }
        }
        if (spec.empty()) spec = L"*.*";
        names.push_back(win32::widen(f.value("name", "")));
        specs.push_back(std::move(spec));
    }
    std::vector<COMDLG_FILTERSPEC> spec_list;
    for (size_t i = 0; i < names.size(); ++i) {
        spec_list.push_back({names[i].c_str(), specs[i].c_str()});
    }
    dlg->SetFileTypes(static_cast<UINT>(spec_list.size()), spec_list.data());
}

// ── Message boxes ───────────────────────────────────────────────────────────

enum ButtonPreset { BP_OK, BP_OK_CANCEL, BP_YES_NO, BP_YES_NO_CANCEL };

// Custom TaskDialog button ids (outside the IDOK..IDCONTINUE range).
constexpr int kBtnOk = 100, kBtnCancel = 101, kBtnYes = 102, kBtnNo = 103;

struct MessageSpec {
    std::string title, message, kind;
    ButtonPreset preset = BP_OK;
    std::string lbl_ok, lbl_cancel, lbl_yes, lbl_no;  // empty → default
};

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

TaskDialogIndirectFn task_dialog_fn() {
    // Only exported by comctl32 v6 (needs the manifest dependency above).
    static TaskDialogIndirectFn fn = [] {
        HMODULE lib = LoadLibraryW(L"comctl32.dll");
        return lib ? reinterpret_cast<TaskDialogIndirectFn>(
                         GetProcAddress(lib, "TaskDialogIndirect"))
                   : nullptr;
    }();
    return fn;
}

/// Returns "Ok", "Cancel", "Yes" or "No" (closing the dialog → "Cancel",
/// or "No" for a plain Yes/No question, like the GTK implementation).
std::string show_message(const MessageSpec& spec) {
    auto label = [](const std::string& custom, const wchar_t* def) {
        return custom.empty() ? std::wstring(def) : win32::widen(custom);
    };
    std::wstring ok = label(spec.lbl_ok, L"OK"), cancel = label(spec.lbl_cancel, L"Cancel"),
                 yes = label(spec.lbl_yes, L"Yes"), no = label(spec.lbl_no, L"No");
    std::wstring title = win32::widen(spec.title), message = win32::widen(spec.message);

    std::vector<TASKDIALOG_BUTTON> buttons;
    switch (spec.preset) {
    case BP_OK:            buttons = {{kBtnOk, ok.c_str()}}; break;
    case BP_OK_CANCEL:     buttons = {{kBtnOk, ok.c_str()}, {kBtnCancel, cancel.c_str()}}; break;
    case BP_YES_NO:        buttons = {{kBtnYes, yes.c_str()}, {kBtnNo, no.c_str()}}; break;
    case BP_YES_NO_CANCEL: buttons = {{kBtnYes, yes.c_str()}, {kBtnNo, no.c_str()},
                                      {kBtnCancel, cancel.c_str()}}; break;
    }

    if (auto task_dialog = task_dialog_fn()) {
        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = dialog_owner();
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.pszWindowTitle = title.c_str();
        cfg.pszContent = message.c_str();
        cfg.pButtons = buttons.data();
        cfg.cButtons = static_cast<UINT>(buttons.size());
        if (spec.kind == "warning")    cfg.pszMainIcon = TD_WARNING_ICON;
        else if (spec.kind == "error") cfg.pszMainIcon = TD_ERROR_ICON;
        else if (spec.preset == BP_OK || spec.preset == BP_OK_CANCEL)
            cfg.pszMainIcon = TD_INFORMATION_ICON;

        int clicked = 0;
        if (SUCCEEDED(task_dialog(&cfg, &clicked, nullptr, nullptr))) {
            switch (clicked) {
            case kBtnOk:  return "Ok";
            case kBtnYes: return "Yes";
            case kBtnNo:  return "No";
            default:      return spec.preset == BP_YES_NO ? "No" : "Cancel";
            }
        }
    }

    // Fallback: standard MessageBox (custom labels are not supported).
    UINT type = MB_OK;
    switch (spec.preset) {
    case BP_OK:            type = MB_OK; break;
    case BP_OK_CANCEL:     type = MB_OKCANCEL; break;
    case BP_YES_NO:        type = MB_YESNO; break;
    case BP_YES_NO_CANCEL: type = MB_YESNOCANCEL; break;
    }
    if (spec.kind == "warning")    type |= MB_ICONWARNING;
    else if (spec.kind == "error") type |= MB_ICONERROR;
    else if (spec.preset == BP_YES_NO || spec.preset == BP_YES_NO_CANCEL)
        type |= MB_ICONQUESTION;
    else
        type |= MB_ICONINFORMATION;

    switch (MessageBoxW(dialog_owner(), message.c_str(), title.c_str(), type)) {
    case IDOK:  return "Ok";
    case IDYES: return "Yes";
    case IDNO:  return "No";
    default:    return "Cancel";
    }
}

} // namespace

// ── Plugin registration ─────────────────────────────────────────────────────

void DialogPlugin::initialize(PluginContext& ctx) {
    auto& cmds = ctx.commands;

    // ── dialog:open ─────────────────────────────────────────────────────────
    cmds.add("dialog:open", [](const json& args) -> json {
        std::string title   = args.value("title", "Open File");
        bool multiple       = args.value("multiple", false);
        bool directory      = args.value("directory", false);
        std::string defPath = args.value("defaultPath", "");
        json filters        = args.value("filters", json::array());

        return run_on_main_thread([&]() -> json {
            ComScope com;
            ComPtr<IFileOpenDialog> dlg;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(dlg.put())))) {
                throw std::runtime_error("dialog:open: cannot create IFileOpenDialog");
            }

            FILEOPENDIALOGOPTIONS opts = 0;
            dlg->GetOptions(&opts);
            opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
            if (directory) opts |= FOS_PICKFOLDERS;
            else           opts |= FOS_FILEMUSTEXIST;
            if (multiple)  opts |= FOS_ALLOWMULTISELECT;
            dlg->SetOptions(opts);

            dlg->SetTitle(win32::widen(title).c_str());
            set_folder(dlg.get(), defPath);
            if (!directory) set_filters(dlg.get(), filters);

            if (FAILED(dlg->Show(dialog_owner()))) {
                return nullptr;  // cancelled (HRESULT_FROM_WIN32(ERROR_CANCELLED))
            }

            json paths = json::array();
            ComPtr<IShellItemArray> items;
            if (SUCCEEDED(dlg->GetResults(items.put()))) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    ComPtr<IShellItem> item;
                    if (SUCCEEDED(items->GetItemAt(i, item.put()))) {
                        std::string p = item_path(item.get());
                        if (!p.empty()) paths.push_back(p);
                    }
                }
            }
            return paths;
        });
    });

    // ── dialog:save ─────────────────────────────────────────────────────────
    cmds.add("dialog:save", [](const json& args) -> json {
        std::string title   = args.value("title", "Save File");
        std::string defPath = args.value("defaultPath", "");
        json filters        = args.value("filters", json::array());

        return run_on_main_thread([&]() -> json {
            ComScope com;
            ComPtr<IFileSaveDialog> dlg;
            if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(dlg.put())))) {
                throw std::runtime_error("dialog:save: cannot create IFileSaveDialog");
            }

            FILEOPENDIALOGOPTIONS opts = 0;
            dlg->GetOptions(&opts);
            dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
            dlg->SetTitle(win32::widen(title).c_str());

            if (!defPath.empty()) {
                auto pos = defPath.find_last_of("/\\");
                if (pos != std::string::npos) {
                    set_folder(dlg.get(), defPath.substr(0, pos));
                    if (pos + 1 < defPath.size()) {
                        dlg->SetFileName(win32::widen(defPath.substr(pos + 1)).c_str());
                    }
                } else {
                    dlg->SetFileName(win32::widen(defPath).c_str());
                }
            }
            set_filters(dlg.get(), filters);

            if (FAILED(dlg->Show(dialog_owner()))) {
                return nullptr;
            }
            ComPtr<IShellItem> item;
            if (FAILED(dlg->GetResult(item.put()))) {
                return nullptr;
            }
            return json(item_path(item.get()));
        });
    });

    // ── dialog:message ──────────────────────────────────────────────────────
    // Same argument contract as the GTK implementation (see dialog_plugin.h).
    cmds.add("dialog:message", [](const json& args) -> json {
        MessageSpec spec;
        spec.title   = args.value("title", "Message");
        spec.message = args.at("message").get<std::string>();
        spec.kind    = args.value("kind", "info");
        json buttons_arg = args.value("buttons", json("Ok"));

        if (buttons_arg.is_string()) {
            std::string s = buttons_arg.get<std::string>();
            if (s == "OkCancel")         spec.preset = BP_OK_CANCEL;
            else if (s == "YesNo")       spec.preset = BP_YES_NO;
            else if (s == "YesNoCancel") spec.preset = BP_YES_NO_CANCEL;
        } else if (buttons_arg.is_object()) {
            if (buttons_arg.contains("yes")) {
                spec.lbl_yes = buttons_arg["yes"].get<std::string>();
                spec.lbl_no  = buttons_arg.value("no", std::string("No"));
                if (buttons_arg.contains("cancel")) {
                    spec.lbl_cancel = buttons_arg["cancel"].get<std::string>();
                    spec.preset = BP_YES_NO_CANCEL;
                } else {
                    spec.preset = BP_YES_NO;
                }
            } else {
                spec.lbl_ok = buttons_arg.value("ok", std::string("OK"));
                if (buttons_arg.contains("cancel")) {
                    spec.lbl_cancel = buttons_arg["cancel"].get<std::string>();
                    spec.preset = BP_OK_CANCEL;
                }
            }
        }

        return run_on_main_thread([&]() -> json { return show_message(spec); });
    });

    // ── dialog:ask ──────────────────────────────────────────────────────────
    cmds.add("dialog:ask", [](const json& args) -> json {
        MessageSpec spec;
        spec.title   = args.value("title", "Question");
        spec.message = args.at("message").get<std::string>();
        spec.kind    = args.value("kind", "info");
        spec.preset  = BP_YES_NO;

        return run_on_main_thread([&]() -> json { return show_message(spec) == "Yes"; });
    });

    // ── dialog:confirm ──────────────────────────────────────────────────────
    cmds.add("dialog:confirm", [](const json& args) -> json {
        MessageSpec spec;
        spec.title      = args.value("title", "Confirm");
        spec.message    = args.at("message").get<std::string>();
        spec.kind       = args.value("kind", "info");
        spec.preset     = BP_OK_CANCEL;
        spec.lbl_ok     = args.value("okLabel", std::string("OK"));
        spec.lbl_cancel = args.value("cancelLabel", std::string("Cancel"));

        return run_on_main_thread([&]() -> json { return show_message(spec) == "Ok"; });
    });
}

} // namespace anyar
