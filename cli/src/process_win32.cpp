// anyar CLI — process helpers (Windows: CreateProcess, job objects)
//
// Commands run through `cmd.exe /d /s /c "<cmd>"` so `npm` (npm.cmd), `&&`
// and PATH lookup behave like in a terminal.  Background children are put in
// a job object (created suspended, assigned, resumed) so killing the job
// stops the whole tree — cmd → npm → node for `npm run dev`.

#include "cli.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iostream>
#include <vector>

namespace anyar_cli {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

/// Start `cmd` via cmd.exe.  With @p in_job, the process (and everything it
/// spawns) is confined to a kill-on-close job.
bool spawn(const std::string& cmd, const fs::path& cwd, bool in_job, ChildProcess& out) {
    std::wstring cmdline = L"cmd.exe /d /s /c \"" + widen(cmd) + L"\"";
    std::wstring wcwd = cwd.empty() ? std::wstring() : fs::absolute(cwd).wstring();

    HANDLE job = nullptr;
    if (in_job) {
        job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // No STARTF_USESTDHANDLES: the child shares our console (live output).
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (job ? CREATE_SUSPENDED : 0);
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, flags,
                        nullptr, wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        if (job) CloseHandle(job);
        return false;
    }
    if (job) {
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    out.pid = pi.dwProcessId;
    out.handle = pi.hProcess;
    out.job = job;
    return true;
}

void close_child(ChildProcess& child) {
    if (child.handle) CloseHandle(static_cast<HANDLE>(child.handle));
    if (child.job) CloseHandle(static_cast<HANDLE>(child.job));  // kills leftovers
    child = ChildProcess{};
}

void (*g_interrupt_fn)() = nullptr;

BOOL WINAPI ctrl_handler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (g_interrupt_fn) g_interrupt_fn();
        std::cout << std::endl;
        ExitProcess(0);
    default:
        return FALSE;
    }
}

} // namespace

std::string shell_quote(const std::string& s) {
    if (!s.empty() && s.find_first_of(" \t&()[]{}^=;!'+,`~\"") == std::string::npos) {
        return s;
    }
    return "\"" + s + "\"";  // cmd.exe has no escape for an embedded quote
}

int run(const std::string& cmd, const fs::path& cwd) {
    ChildProcess child;
    if (!spawn(cmd, cwd, /*in_job=*/false, child)) {
        print_error("Failed to start: " + cmd);
        return -1;
    }
    return wait_child(child);
}

ChildProcess run_bg(const std::string& cmd, const fs::path& cwd) {
    ChildProcess child;
    spawn(cmd, cwd, /*in_job=*/true, child);
    return child;
}

int wait_child(ChildProcess& child) {
    if (!child.valid()) return -1;
    WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code);
    close_child(child);
    return static_cast<int>(code);
}

void kill_child(ChildProcess& child) {
    if (!child.valid()) return;
    if (child.job) {
        TerminateJobObject(static_cast<HANDLE>(child.job), 1);
    } else {
        TerminateProcess(static_cast<HANDLE>(child.handle), 1);
    }
    WaitForSingleObject(static_cast<HANDLE>(child.handle), 5000);
    close_child(child);
}

void on_interrupt(void (*fn)()) {
    g_interrupt_fn = fn;
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

void init_console() {
    // UTF-8 output (✓ ✗ → glyphs) and ANSI colour sequences.
    SetConsoleOutputCP(CP_UTF8);
    for (DWORD id : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE h = GetStdHandle(id);
        DWORD mode = 0;
        if (h && GetConsoleMode(h, &mode)) {
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}

fs::path executable_path() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size()) return fs::path(std::wstring(buf.data(), n));
        buf.resize(buf.size() * 2);
    }
}

bool has_command(const std::string& cmd) {
    ChildProcess child;
    if (!spawn("where " + cmd + " >nul 2>&1", {}, false, child)) return false;
    return wait_child(child) == 0;
}

} // namespace anyar_cli
