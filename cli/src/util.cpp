// anyar CLI — utility functions

#include "cli.h"
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>

namespace anyar_cli {

// ── ANSI colors ─────────────────────────────────────────────────────────────

static const char* RESET   = "\033[0m";
static const char* BOLD    = "\033[1m";
static const char* DIM     = "\033[2m";
static const char* GREEN   = "\033[32m";
static const char* YELLOW  = "\033[33m";
static const char* RED     = "\033[31m";
static const char* CYAN    = "\033[36m";
static const char* MAGENTA = "\033[35m";

void print_header(const std::string& text) {
    std::cout << "\n" << BOLD << MAGENTA << "  " << text << RESET << "\n" << std::endl;
}

void print_success(const std::string& text) {
    std::cout << GREEN << "  ✓ " << RESET << text << std::endl;
}

void print_error(const std::string& text) {
    std::cerr << RED << "  ✗ " << RESET << text << std::endl;
}

void print_info(const std::string& text) {
    std::cout << CYAN << "  ℹ " << RESET << text << std::endl;
}

void print_step(const std::string& text) {
    std::cout << YELLOW << "  → " << RESET << text << std::endl;
}

// ── Shell execution ─────────────────────────────────────────────────────────

int run(const std::string& cmd, const fs::path& cwd) {
    std::string full_cmd = cmd;
    if (!cwd.empty()) {
        full_cmd = "cd " + cwd.string() + " && " + cmd;
    }
    return WEXITSTATUS(std::system(full_cmd.c_str()));
}

pid_t run_bg(const std::string& cmd, const fs::path& cwd) {
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
    return pid;
}

// ── User prompts ────────────────────────────────────────────────────────────

std::string prompt(const std::string& question, const std::string& default_val) {
    if (!default_val.empty()) {
        std::cout << CYAN << "  ? " << RESET << question
                  << DIM << " (" << default_val << ")" << RESET << " ";
    } else {
        std::cout << CYAN << "  ? " << RESET << question << " ";
    }

    std::string line;
    std::getline(std::cin, line);

    if (line.empty()) return default_val;
    return line;
}

int pick(const std::string& question, const std::vector<std::string>& choices, int default_idx) {
    std::cout << CYAN << "  ? " << RESET << question << std::endl;
    for (size_t i = 0; i < choices.size(); i++) {
        std::cout << "    " << (i == (size_t)default_idx ? BOLD : DIM)
                  << "  " << (i + 1) << ") " << choices[i]
                  << (i == (size_t)default_idx ? " (default)" : "")
                  << RESET << std::endl;
    }
    std::cout << "    " << DIM << "Enter choice [1-" << choices.size() << "]: " << RESET;

    std::string line;
    std::getline(std::cin, line);

    if (line.empty()) return default_idx;

    try {
        int val = std::stoi(line);
        if (val >= 1 && val <= (int)choices.size()) return val - 1;
    } catch (...) {}

    return default_idx;
}

// ── Path utilities ──────────────────────────────────────────────────────────

bool has_command(const std::string& cmd) {
    std::string check = "command -v " + cmd + " >/dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}

fs::path find_libanyar_root(const fs::path& start) {
    // 1) Walk up from start (usually cwd)
    fs::path dir = fs::absolute(start);
    while (!dir.empty() && dir != dir.root_path()) {
        if (fs::exists(dir / "ARCHITECTURE.md") && fs::exists(dir / "core" / "CMakeLists.txt")) {
            return dir;
        }
        dir = dir.parent_path();
    }

    // 2) Walk up from the binary's own location (e.g. build/cli/anyar → ../../)
    fs::path exe;
    #ifdef __linux__
    {
        char buf[4096];
        ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) { buf[len] = '\0'; exe = fs::path(buf).parent_path(); }
    }
    #endif
    if (!exe.empty()) {
        dir = exe;
        while (!dir.empty() && dir != dir.root_path()) {
            if (fs::exists(dir / "ARCHITECTURE.md") && fs::exists(dir / "core" / "CMakeLists.txt")) {
                return dir;
            }
            dir = dir.parent_path();
        }
    }

    return {};  // not found
}

} // namespace anyar_cli
