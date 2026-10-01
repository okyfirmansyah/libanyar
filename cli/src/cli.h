#pragma once
// anyar CLI — shared types and declarations

#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace anyar_cli {

// ── Command dispatch ────────────────────────────────────────────────────────

int cmd_init(int argc, char* argv[]);
int cmd_dev(int argc, char* argv[]);
int cmd_build(int argc, char* argv[]);

// ── Packaging ───────────────────────────────────────────────────────────────

/// Options for package_app() (from `anyar build --package …`).
struct PackageOptions {
    /// Linux: "deb", "appimage", "all".  Windows: "zip", "installer"
    /// (alias "nsis"), "all".
    std::string format;
    std::string version = "0.1.0";   ///< Semantic version, e.g. "1.2.3"
    std::string build_type = "Release";  ///< multi-config subdir (VS)
    std::string publisher;           ///< Installer publisher (default: app name)
    std::string install_scope = "user";  ///< Windows installer: "user" | "machine"
    std::string webview2 = "bootstrapper";  ///< Windows installer: "bootstrapper" | "skip"
};

/// Package a built application.
/// @param opts          Format + metadata (see PackageOptions)
/// @param project_name  CMake project name (binary name)
/// @param project_dir   Root of the application project
/// @param build_dir     Build directory containing the binary
int package_app(const PackageOptions& opts,
                const std::string& project_name,
                const fs::path& project_dir,
                const fs::path& build_dir);

// ── Processes (process_posix.cpp / process_win32.cpp) ───────────────────────

/// A background child started by run_bg().  On Windows the child and all its
/// descendants live in a job object, so kill_child() stops the whole tree
/// (cmd → npm → node).
struct ChildProcess {
    long long pid = 0;        ///< OS process id (0 = not running)
    void* handle = nullptr;   ///< Windows process HANDLE
    void* job = nullptr;      ///< Windows job object HANDLE
    bool valid() const { return pid != 0; }
};

/// Run a shell command (sh -c / cmd.exe /c), return its exit code.
/// Streams stdout/stderr to the terminal.
int run(const std::string& cmd, const fs::path& cwd = "");

/// Start a shell command in the background.  Returns an invalid handle on
/// failure.
ChildProcess run_bg(const std::string& cmd, const fs::path& cwd = "");

/// Block until @p child exits; returns its exit code (-1 if unknown).
int wait_child(ChildProcess& child);

/// Terminate @p child (Windows: its whole process tree) and reap it.
void kill_child(ChildProcess& child);

/// Call @p fn on Ctrl+C / SIGINT / SIGTERM (then the process exits).
void on_interrupt(void (*fn)());

/// Prepare the terminal (Windows: UTF-8 output + ANSI colour sequences).
void init_console();

/// Absolute path of the running `anyar` executable, or empty.
fs::path executable_path();

/// Quote @p s as one shell argument for run()/run_bg() (sh or cmd.exe).
std::string shell_quote(const std::string& s);

/// Check if a command is available on PATH
bool has_command(const std::string& cmd);

// ── Build helpers (util.cpp) ────────────────────────────────────────────────

/// Read `project(<name> ...)` from a CMakeLists.txt; "app" if not found.
std::string read_project_name(const fs::path& cmakelists);

/// Extra `cmake` configure arguments for this platform on a FRESH build dir
/// (Windows: vcpkg toolchain + LibAsyik prefix when found).  Leading space.
std::string platform_configure_args(const fs::path& build_dir);

/// Locate the built app binary (single- or multi-config layout).  Empty if
/// not found.
fs::path find_app_binary(const fs::path& build_dir, const std::string& name,
                         const std::string& build_type);

/// Prompt the user for text input (with a default value)
std::string prompt(const std::string& question, const std::string& default_val = "");

/// Prompt the user to pick from a list, returns 0-based index
int pick(const std::string& question, const std::vector<std::string>& choices, int default_idx = 0);

/// Print colored text
void print_header(const std::string& text);
void print_success(const std::string& text);
void print_error(const std::string& text);
void print_info(const std::string& text);
void print_step(const std::string& text);

/// Find libanyar root by searching upward for ARCHITECTURE.md
fs::path find_libanyar_root(const fs::path& start = fs::current_path());

// ── Template generation ─────────────────────────────────────────────────────

struct TemplateSpec {
    std::string name;        // e.g. "svelte-ts", "react-ts", "vanilla"
    std::string display;     // e.g. "Svelte 5 + TypeScript"
    std::string framework;   // e.g. "svelte", "react", "vanilla"
};

const std::vector<TemplateSpec>& available_templates();

/// Generate project files for the given template into dest_dir
void generate_template(const std::string& template_name,
                       const std::string& project_name,
                       const fs::path& dest_dir,
                       const fs::path& libanyar_root);

} // namespace anyar_cli
