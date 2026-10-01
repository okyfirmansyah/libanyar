// anyar CLI — utility functions (platform-neutral).
// Process spawning lives in process_posix.cpp / process_win32.cpp.

#include "cli.h"
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace anyar_cli {

// ── ANSI colors ─────────────────────────────────────────────────────────────
// (Windows consoles understand these once init_console() enabled VT mode.)

static const char* RESET   = "\033[0m";
static const char* BOLD    = "\033[1m";
static const char* DIM     = "\033[2m";
static const char* GREEN   = "\033[32m";
static const char* YELLOW  = "\033[33m";
static const char* RED     = "\033[31m";
static const char* CYAN    = "\033[36m";
static const char* MAGENTA = "\033[35m";

// Plain mode: ASCII markers, no colours — for output relayed by another tool
// (e.g. `anyar sign-file` under makensis, which re-encodes child output).
static bool g_plain = false;

void set_plain_output(bool plain) { g_plain = plain; }

static void emit(std::ostream& os, const char* colour, const char* glyph, const char* ascii,
                 const std::string& text) {
    if (g_plain) os << "  " << ascii << " " << text << std::endl;
    else os << colour << "  " << glyph << " " << RESET << text << std::endl;
}

void print_header(const std::string& text) {
    if (g_plain) { std::cout << "\n  " << text << "\n" << std::endl; return; }
    std::cout << "\n" << BOLD << MAGENTA << "  " << text << RESET << "\n" << std::endl;
}

void print_success(const std::string& text) { emit(std::cout, GREEN, "✓", "[ok]", text); }
void print_error(const std::string& text)   { emit(std::cerr, RED, "✗", "[error]", text); }
void print_info(const std::string& text)    { emit(std::cout, CYAN, "ℹ", "[info]", text); }
void print_step(const std::string& text)    { emit(std::cout, YELLOW, "→", "->", text); }

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

static bool is_libanyar_root(const fs::path& dir) {
    return fs::exists(dir / "ARCHITECTURE.md") && fs::exists(dir / "core" / "CMakeLists.txt");
}

fs::path find_libanyar_root(const fs::path& start) {
    // 1) Walk up from start (usually cwd)
    fs::path dir = fs::absolute(start);
    while (!dir.empty() && dir != dir.root_path()) {
        if (is_libanyar_root(dir)) return dir;
        dir = dir.parent_path();
    }

    // 2) Walk up from the binary's own location (e.g. build/cli/anyar → ../../)
    fs::path exe = executable_path();
    if (!exe.empty()) {
        dir = exe.parent_path();
        while (!dir.empty() && dir != dir.root_path()) {
            if (is_libanyar_root(dir)) return dir;
            dir = dir.parent_path();
        }
    }

    // 3) LIBANYAR_DIR environment variable
    if (const char* env = std::getenv("LIBANYAR_DIR")) {
        if (is_libanyar_root(env)) return fs::path(env);
    }

    return {};  // not found
}

// ── Build helpers ───────────────────────────────────────────────────────────

std::string read_project_name(const fs::path& cmakelists) {
    std::ifstream f(cmakelists);
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("project(");
        if (pos != std::string::npos) {
            auto start = pos + 8;
            auto end = line.find_first_of(" )", start);
            if (end != std::string::npos) {
                return line.substr(start, end - start);
            }
            break;
        }
    }
    return "app";
}

std::string platform_configure_args(const fs::path& build_dir) {
#ifdef _WIN32
    // Only on a fresh build dir: a cached toolchain must not change.
    if (fs::exists(build_dir / "CMakeCache.txt")) return {};
    std::string args;

    // vcpkg toolchain: VCPKG_ROOT, else the conventional C:\vcpkg
    fs::path vcpkg;
    if (const char* env = std::getenv("VCPKG_ROOT")) vcpkg = env;
    else if (fs::exists("C:/vcpkg/vcpkg.exe")) vcpkg = "C:/vcpkg";
    fs::path toolchain = vcpkg / "scripts" / "buildsystems" / "vcpkg.cmake";
    if (!vcpkg.empty() && fs::exists(toolchain)) {
        args += " -DCMAKE_TOOLCHAIN_FILE=" + shell_quote(toolchain.generic_string());
    }

    // LibAsyik install: ANYAR_LIBASYIK_PREFIX, else what setup-windows.ps1
    // produces inside the libanyar tree (build-deps/libasyik).
    fs::path prefix;
    if (const char* env = std::getenv("ANYAR_LIBASYIK_PREFIX")) {
        prefix = env;
    } else {
        fs::path root = find_libanyar_root();
        if (!root.empty() && fs::exists(root / "build-deps" / "libasyik")) {
            prefix = root / "build-deps" / "libasyik";
        }
    }
    if (!prefix.empty()) {
        args += " -DCMAKE_PREFIX_PATH=" + shell_quote(prefix.generic_string());
    }
    return args;
#else
    (void)build_dir;
    return {};
#endif
}

fs::path find_app_icon(const fs::path& project_dir) {
    for (const char* dir : {"", "assets", "frontend/public"}) {
        for (const char* file : {"icon.ico", "icon.png"}) {
            fs::path p = project_dir / dir / file;
            if (fs::exists(p)) return p;
        }
    }
    return {};
}

fs::path find_app_binary(const fs::path& build_dir, const std::string& name,
                         const std::string& build_type) {
#ifdef _WIN32
    const std::string file = name + ".exe";
#else
    const std::string& file = name;
#endif
    for (const fs::path& p : {build_dir / build_type / file,   // multi-config (VS)
                              build_dir / file}) {             // single-config
        if (fs::exists(p)) return p;
    }
    return {};
}

} // namespace anyar_cli
