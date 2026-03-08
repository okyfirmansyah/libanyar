// anyar CLI — `anyar build` command
// Builds frontend for production + compiles C++ backend in Release mode

#include "cli.h"
#include <iostream>
#include <fstream>
#include <thread>

namespace anyar_cli {

static void print_build_usage() {
    std::cout << R"(
  Usage: anyar build [options]

  Options:
    --release         Build in Release mode (default)
    --debug           Build in Debug mode
    --no-frontend     Skip frontend build
    --no-backend      Skip C++ backend build
    --clean           Clean build directory before building
    --help, -h        Show this help

  Must be run from a LibAnyar project directory.
)" << std::endl;
}

int cmd_build(int argc, char* argv[]) {
    bool build_frontend = true;
    bool build_backend = true;
    bool clean = false;
    std::string build_type = "Release";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_build_usage(); return 0; }
        if (arg == "--release") { build_type = "Release"; continue; }
        if (arg == "--debug") { build_type = "Debug"; continue; }
        if (arg == "--no-frontend") { build_frontend = false; continue; }
        if (arg == "--no-backend") { build_backend = false; continue; }
        if (arg == "--clean") { clean = true; continue; }
    }

    // Verify project directory
    fs::path project_dir = fs::current_path();
    if (!fs::exists(project_dir / "CMakeLists.txt")) {
        print_error("No CMakeLists.txt found. Run this from a LibAnyar project directory.");
        return 1;
    }

    // Detect project name
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

    print_header("Building " + project_name + " (" + build_type + ")");

    // ── 1) Build frontend ───────────────────────────────────────────────
    if (build_frontend && fs::exists(project_dir / "frontend" / "package.json")) {
        print_step("Building frontend...");

        // Install if node_modules doesn't exist
        if (!fs::exists(project_dir / "frontend" / "node_modules")) {
            print_step("Installing frontend dependencies...");
            int rc = run("npm install", project_dir / "frontend");
            if (rc != 0) {
                print_error("npm install failed");
                return 1;
            }
        }

        int rc = run("npm run build", project_dir / "frontend");
        if (rc != 0) {
            print_error("Frontend build failed");
            return 1;
        }
        print_success("Frontend built → frontend/dist/");
    }

    // ── 2) Build C++ backend ────────────────────────────────────────────
    if (build_backend) {
        fs::path build_dir = project_dir / "build";

        if (clean && fs::exists(build_dir)) {
            print_step("Cleaning build directory...");
            fs::remove_all(build_dir);
        }

        fs::create_directories(build_dir);

        print_step("Configuring CMake (" + build_type + ")...");
        int rc = run("cmake .. -DCMAKE_BUILD_TYPE=" + build_type, build_dir);
        if (rc != 0) {
            print_error("CMake configuration failed");
            return 1;
        }

        unsigned int cores = std::thread::hardware_concurrency();
        if (cores == 0) cores = 4;

        print_step("Compiling C++ backend...");
        rc = run("make -j" + std::to_string(cores), build_dir);
        if (rc != 0) {
            print_error("C++ build failed");
            return 1;
        }

        // Check binary exists
        fs::path binary = build_dir / project_name;
        if (fs::exists(binary)) {
            auto size = fs::file_size(binary);
            std::string size_str;
            if (size > 1024 * 1024) {
                size_str = std::to_string(size / (1024 * 1024)) + " MB";
            } else {
                size_str = std::to_string(size / 1024) + " KB";
            }
            print_success("Binary: build/" + project_name + " (" + size_str + ")");
        }
    }

    // ── Done ────────────────────────────────────────────────────────────
    std::cout << std::endl;
    print_success("Build complete!");

    if (build_backend) {
        std::cout << std::endl;
        std::cout << "  Run your app:" << std::endl;
        std::cout << "    cd build && ./" << project_name << std::endl;
        std::cout << std::endl;
    }

    return 0;
}

} // namespace anyar_cli
