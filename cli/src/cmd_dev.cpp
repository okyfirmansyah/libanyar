// anyar CLI — `anyar dev` command
// Starts frontend dev server (Vite HMR) + builds and runs C++ backend

#include "cli.h"
#include <iostream>
#include <thread>

namespace anyar_cli {

static ChildProcess vite_proc;
static ChildProcess app_proc;

/// Ctrl+C / SIGTERM: stop both children (Windows: their whole trees).
static void stop_children() {
    kill_child(vite_proc);
    kill_child(app_proc);
}

static void print_dev_usage() {
    std::cout << R"(
  Usage: anyar dev [options]

  Options:
    --no-frontend   Skip starting the Vite dev server
    --no-backend    Skip building/running the C++ backend
    --help, -h      Show this help

  Must be run from a LibAnyar project directory (with CMakeLists.txt + frontend/).
)" << std::endl;
}

int cmd_dev(int argc, char* argv[]) {
    bool run_frontend = true;
    bool run_backend = true;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_dev_usage(); return 0; }
        if (arg == "--no-frontend") { run_frontend = false; continue; }
        if (arg == "--no-backend") { run_backend = false; continue; }
    }

    // Verify we're in a project directory
    fs::path project_dir = fs::current_path();
    if (!fs::exists(project_dir / "CMakeLists.txt")) {
        print_error("No CMakeLists.txt found. Run this from a LibAnyar project directory.");
        return 1;
    }
    if (!fs::exists(project_dir / "frontend" / "package.json")) {
        print_error("No frontend/package.json found. Is this a LibAnyar project?");
        return 1;
    }

    const std::string project_name = read_project_name(project_dir / "CMakeLists.txt");
    const std::string build_type = "Debug";

    print_header("LibAnyar Development Server");

    // Clean shutdown on Ctrl+C
    on_interrupt(stop_children);

    // ── 1) Start Vite dev server ────────────────────────────────────────
    if (run_frontend) {
        print_step("Starting Vite dev server...");
        vite_proc = run_bg("npm run dev", project_dir / "frontend");
        if (vite_proc.valid()) {
            print_success("Vite dev server started (PID " + std::to_string(vite_proc.pid) + ")");
        } else {
            print_error("Failed to start Vite dev server");
        }
    }

    // ── 2) Build C++ backend ────────────────────────────────────────────
    if (run_backend) {
        fs::path build_dir = project_dir / "build";
        fs::create_directories(build_dir);

        print_step("Configuring CMake...");
        int rc = run("cmake .. -DCMAKE_BUILD_TYPE=" + build_type +
                     platform_configure_args(build_dir), build_dir);
        if (rc != 0) {
            print_error("CMake configuration failed");
            stop_children();
            return 1;
        }

        // Get number of CPU cores for parallel build
        unsigned int cores = std::thread::hardware_concurrency();
        if (cores == 0) cores = 4;

        print_step("Building C++ backend...");
        rc = run("cmake --build . --config " + build_type + " --parallel " +
                 std::to_string(cores), build_dir);
        if (rc != 0) {
            print_error("C++ build failed");
            stop_children();
            return 1;
        }
        print_success("C++ backend built");

        // ── 3) Run the app ──────────────────────────────────────────────
        fs::path binary = find_app_binary(build_dir, project_name, build_type);
        if (binary.empty()) {
            print_error("Binary not found for project '" + project_name + "' in " +
                        build_dir.string());
            stop_children();
            return 1;
        }

        print_step("Starting " + project_name + "...");
        std::cout << std::endl;

        std::string run_cmd = shell_quote(binary.string());
#ifdef __linux__
        // Use run.sh if available (handles snap GTK env issues)
        fs::path run_script = find_libanyar_root(project_dir) / "run.sh";
        if (fs::exists(run_script)) {
            run_cmd = "bash " + shell_quote(run_script.string()) + " " + run_cmd;
        }
#endif

        app_proc = run_bg(run_cmd, binary.parent_path());
        if (app_proc.valid()) {
            print_success(project_name + " started (PID " + std::to_string(app_proc.pid) + ")");
        }

        // Wait for the app to exit
        wait_child(app_proc);
        print_info(project_name + " exited");
    }

    if (!run_backend && vite_proc.valid()) {
        // Frontend-only: keep serving until Ctrl+C (or Vite exits).
        wait_child(vite_proc);
        return 0;
    }

    // Clean up frontend server
    kill_child(vite_proc);

    return 0;
}

} // namespace anyar_cli
