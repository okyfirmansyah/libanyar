// anyar CLI — process helpers (POSIX: fork/exec, signals)

#include "cli.h"

#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

namespace anyar_cli {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

int run(const std::string& cmd, const fs::path& cwd) {
    std::string full_cmd = cmd;
    if (!cwd.empty()) {
        full_cmd = "cd " + shell_quote(cwd.string()) + " && " + cmd;
    }
    int status = std::system(full_cmd.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

ChildProcess run_bg(const std::string& cmd, const fs::path& cwd) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) {
                _exit(1);
            }
        }
        // Run via shell
        execlp("sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(1);  // exec failed
    }
    ChildProcess child;
    if (pid > 0) child.pid = pid;
    return child;
}

int wait_child(ChildProcess& child) {
    if (!child.valid()) return -1;
    int status = 0;
    waitpid(static_cast<pid_t>(child.pid), &status, 0);
    child.pid = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void kill_child(ChildProcess& child) {
    if (!child.valid()) return;
    kill(static_cast<pid_t>(child.pid), SIGTERM);
    int status = 0;
    waitpid(static_cast<pid_t>(child.pid), &status, 0);
    child.pid = 0;
}

static void (*g_interrupt_fn)() = nullptr;

void on_interrupt(void (*fn)()) {
    g_interrupt_fn = fn;
    auto handler = +[](int) {
        if (g_interrupt_fn) g_interrupt_fn();
        _exit(0);
    };
    signal(SIGINT, handler);
    signal(SIGTERM, handler);
}

void init_console() {}

fs::path executable_path() {
#ifdef __linux__
    std::error_code ec;
    auto exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return exe;
#endif
    return {};
}

bool has_command(const std::string& cmd) {
    std::string check = "command -v " + cmd + " >/dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}

} // namespace anyar_cli
