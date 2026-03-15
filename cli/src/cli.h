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

// ── Packaging (Linux) ───────────────────────────────────────────────────────

/// Package a built application into the given format.
/// @param format   "deb", "appimage", or "all"
/// @param project_name  CMake project name (binary name)
/// @param project_dir   Root of the application project
/// @param build_dir     Build directory containing the binary
/// @param version       Semantic version string (e.g. "0.1.0")
int package_linux(const std::string& format,
                  const std::string& project_name,
                  const fs::path& project_dir,
                  const fs::path& build_dir,
                  const std::string& version);

// ── Utility functions ───────────────────────────────────────────────────────

/// Run a shell command, return exit code.  Streams stdout/stderr to terminal.
int run(const std::string& cmd, const fs::path& cwd = "");

/// Run a shell command in the background, return PID.
pid_t run_bg(const std::string& cmd, const fs::path& cwd = "");

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

/// Check if a command is available on PATH
bool has_command(const std::string& cmd);

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
