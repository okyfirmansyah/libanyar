// anyar CLI — `anyar dev` command
// Starts frontend dev server (Vite HMR) + builds and runs C++ backend

#include "cli.h"
#include <iostream>
#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#include <thread>
#include <chrono>

namespace anyar_cli {

static pid_t vite_pid = 0;
static pid_t app_pid = 0;

static void cleanup(int /*sig*/) {
    if (vite_pid > 0) kill(vite_pid, SIGTERM);
    if (app_pid > 0) kill(app_pid, SIGTERM);
    // Wait for children
    int status;
    if (vite_pid > 0) waitpid(vite_pid, &status, WNOHANG);
    if (app_pid > 0) waitpid(app_pid, &status, WNOHANG);
    std::cout << std::endl;
    _exit(0);
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

    // Detect project name from CMakeLists.txt
    std::string project_name;
    {
        std::ifstream f(project_dir / "CMakeLists.txt");
        std::string line;
        while (std::getline(f, line)) {
            auto pos = line.find("project(");
            if (pos != std::string::npos) {
                auto start = pos + 8;
                auto end = line.find_first_of(" )", start);
                if (end != std::string::npos) {
                    project_name = line.substr(start, end - start);
                }
                break;
            }
        }
    }
    if (project_name.empty()) project_name = "app";

    print_header("LibAnyar Development Server");

    // Install signal handler for clean shutdown
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    // ── 1) Start Vite dev server ────────────────────────────────────────
    if (run_frontend) {
        print_step("Starting Vite dev server...");
        vite_pid = run_bg("npm run dev", project_dir / "frontend");
        if (vite_pid > 0) {
            print_success("Vite dev server started (PID " + std::to_string(vite_pid) + ")");
        } else {
            print_error("Failed to start Vite dev server");
        }
    }

    // ── 2) Build C++ backend ────────────────────────────────────────────
    if (run_backend) {
        fs::path build_dir = project_dir / "build";
        fs::create_directories(build_dir);

        print_step("Configuring CMake...");
        int rc = run("cmake .. -DCMAKE_BUILD_TYPE=Debug", build_dir);
        if (rc != 0) {
            print_error("CMake configuration failed");
            cleanup(0);
            return 1;
        }

        // Get number of CPU cores for parallel build
        unsigned int cores = std::thread::hardware_concurrency();
        if (cores == 0) cores = 4;

        print_step("Building C++ backend...");
        rc = run("make -j" + std::to_string(cores), build_dir);
        if (rc != 0) {
            print_error("C++ build failed");
            cleanup(0);
            return 1;
        }
        print_success("C++ backend built");

        // ── 3) Run the app ──────────────────────────────────────────────
        // Check for run.sh in project or libanyar root
        fs::path binary = build_dir / project_name;
        if (!fs::exists(binary)) {
            print_error("Binary not found: " + binary.string());
            cleanup(0);
            return 1;
        }

        print_step("Starting " + project_name + "...");
        std::cout << std::endl;

        // Use run.sh if available (handles snap GTK env issues)
        fs::path run_script = find_libanyar_root(project_dir) / "run.sh";
        std::string run_cmd;
        if (fs::exists(run_script)) {
            run_cmd = "bash " + run_script.string() + " ./" + project_name;
        } else {
            run_cmd = "./" + project_name;
        }

        app_pid = run_bg(run_cmd, build_dir);
        if (app_pid > 0) {
            print_success(project_name + " started (PID " + std::to_string(app_pid) + ")");
        }

        // Wait for the app to exit
        int status;
        waitpid(app_pid, &status, 0);
        app_pid = 0;

        print_info(project_name + " exited");
    }

    // Clean up frontend server
    if (vite_pid > 0) {
        kill(vite_pid, SIGTERM);
        int status;
        waitpid(vite_pid, &status, 0);
        vite_pid = 0;
    }

    return 0;
}

} // namespace anyar_cli
