#include <anyar/plugins/shell_plugin.h>
#include "../win32_util.h"

#include <shellapi.h>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace anyar {

// ── Helpers ─────────────────────────────────────────────────────────────────

/// Quote one argument so CommandLineToArgvW / the MSVC CRT parse it back
/// verbatim (backslashes are only special before a double quote).
static std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');  // escape before closing quote
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

/// RAII for a Win32 HANDLE.
struct Handle {
    HANDLE h = nullptr;
    ~Handle() { reset(); }
    void reset() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
};

struct ExecResult {
    int code;
    std::string out;  // stdout
    std::string err;  // stderr
};

static std::string read_all(HANDLE pipe) {
    std::string result;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
        result.append(buf, n);
    }
    return result;
}

/// Run @p program (resolved like CreateProcess does: app dir, cwd, system
/// dirs, PATH; ".exe" appended when no extension) and capture stdout,
/// stderr and the exit code.  A program that cannot be started yields
/// code 127, matching the POSIX implementation.
static ExecResult run_command(const std::string& program,
                              const std::vector<std::string>& args,
                              const std::string& cwd) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle out_r, out_w, err_r, err_w;
    if (!CreatePipe(&out_r.h, &out_w.h, &sa, 0) ||
        !CreatePipe(&err_r.h, &err_w.h, &sa, 0)) {
        throw std::runtime_error("Failed to create pipes");
    }
    // Only the write ends are inherited by the child.
    SetHandleInformation(out_r.h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r.h, HANDLE_FLAG_INHERIT, 0);

    Handle nul;
    nul.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &sa, OPEN_EXISTING, 0, nullptr);

    std::wstring cmdline = quote_arg(win32::widen(program));
    for (auto& a : args) {
        cmdline += L' ';
        cmdline += quote_arg(win32::widen(a));
    }
    std::wstring wcwd = win32::widen(cwd);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul.h;
    si.hStdOutput = out_w.h;
    si.hStdError = err_w.h;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             nullptr, wcwd.empty() ? nullptr : wcwd.c_str(),
                             &si, &pi);
    // Close our copies of the write ends so ReadFile sees EOF when the
    // child exits (or immediately, if it never started).
    out_w.reset();
    err_w.reset();
    nul.reset();

    if (!ok) {
        return {127, "", win32::error_message(GetLastError())};
    }
    Handle process, thread;
    process.h = pi.hProcess;
    thread.h = pi.hThread;

    // Drain both pipes concurrently: a child blocked on a full stderr pipe
    // would otherwise deadlock against us blocked reading stdout.
    std::string stderr_str;
    std::thread err_reader([&] { stderr_str = read_all(err_r.h); });
    std::string stdout_str = read_all(out_r.h);
    err_reader.join();

    WaitForSingleObject(process.h, INFINITE);
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.h, &exit_code)) {
        exit_code = static_cast<DWORD>(-1);
    }
    return {static_cast<int>(exit_code), std::move(stdout_str), std::move(stderr_str)};
}

/// Open a URL or path with its registered handler (browser, Explorer, ...).
static void shell_open(const std::string& target, const char* what) {
    std::wstring wtarget = win32::widen(target);
    auto rc = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wtarget.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (rc <= 32) {  // documented failure range
        throw std::runtime_error(std::string("Failed to open ") + what + ": " + target);
    }
}

// ── Plugin registration ─────────────────────────────────────────────────────

void ShellPlugin::initialize(PluginContext& ctx) {
    auto& cmds = ctx.commands;

    // ── shell:openUrl ───────────────────────────────────────────────────────
    cmds.add("shell:openUrl", [](const json& args) -> json {
        shell_open(args.at("url").get<std::string>(), "URL");
        return nullptr;
    });

    // ── shell:openPath ──────────────────────────────────────────────────────
    cmds.add("shell:openPath", [](const json& args) -> json {
        shell_open(args.at("path").get<std::string>(), "path");
        return nullptr;
    });

    // ── shell:execute ───────────────────────────────────────────────────────
    cmds.add("shell:execute", [](const json& args) -> json {
        std::string program = args.at("program").get<std::string>();

        std::vector<std::string> cmd_args;
        if (args.contains("args") && args["args"].is_array()) {
            for (auto& a : args["args"]) {
                cmd_args.push_back(a.get<std::string>());
            }
        }

        std::string cwd = args.value("cwd", "");

        auto result = run_command(program, cmd_args, cwd);

        return {
            {"code", result.code},
            {"stdout", result.out},
            {"stderr", result.err},
        };
    });
}

} // namespace anyar
