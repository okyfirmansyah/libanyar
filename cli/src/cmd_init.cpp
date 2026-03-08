// anyar CLI — `anyar init` command
// Scaffolds a new LibAnyar project with C++ backend + web frontend

#include "cli.h"
#include <iostream>
#include <fstream>
#include <algorithm>

namespace anyar_cli {

static void print_init_usage() {
    std::cout << R"(
  Usage: anyar init [project-name] [options]

  Options:
    --template, -t <name>   Template: svelte-ts, react-ts, vanilla (default: svelte-ts)
    --help, -h              Show this help

  Examples:
    anyar init myapp
    anyar init myapp --template react-ts
    anyar init                            (interactive)
)" << std::endl;
}

int cmd_init(int argc, char* argv[]) {
    std::string project_name;
    std::string template_name;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_init_usage();
            return 0;
        }
        if ((arg == "--template" || arg == "-t") && i + 1 < argc) {
            template_name = argv[++i];
            continue;
        }
        if (arg[0] != '-' && project_name.empty()) {
            project_name = arg;
            continue;
        }
    }

    print_header("Create a new LibAnyar project");

    // ── Find libanyar root ──────────────────────────────────────────────
    fs::path libanyar_root = find_libanyar_root();
    if (libanyar_root.empty()) {
        print_error("Could not find LibAnyar installation.");
        print_info("Run this command from within the libanyar directory tree,");
        print_info("or set LIBANYAR_DIR environment variable.");
        // Check env var
        const char* env = std::getenv("LIBANYAR_DIR");
        if (env && fs::exists(fs::path(env) / "core" / "CMakeLists.txt")) {
            libanyar_root = env;
        } else {
            return 1;
        }
    }
    print_info("LibAnyar root: " + libanyar_root.string());

    // ── Interactive prompts if needed ────────────────────────────────────
    if (project_name.empty()) {
        project_name = prompt("Project name:", "my-anyar-app");
    }

    // Sanitize: lowercase, replace spaces with hyphens
    std::replace(project_name.begin(), project_name.end(), ' ', '-');

    if (template_name.empty()) {
        auto& templates = available_templates();
        std::vector<std::string> names;
        for (auto& t : templates) names.push_back(t.display);
        int idx = pick("Frontend template:", names, 0);
        template_name = templates[idx].name;
    }

    // Validate template
    {
        auto& templates = available_templates();
        bool found = false;
        for (auto& t : templates) {
            if (t.name == template_name) { found = true; break; }
        }
        if (!found) {
            print_error("Unknown template: " + template_name);
            std::cout << "  Available templates:" << std::endl;
            for (auto& t : templates) {
                std::cout << "    - " << t.name << "  (" << t.display << ")" << std::endl;
            }
            return 1;
        }
    }

    // ── Create project directory ────────────────────────────────────────
    fs::path dest = fs::current_path() / project_name;
    if (fs::exists(dest)) {
        print_error("Directory already exists: " + dest.string());
        return 1;
    }

    print_step("Creating project: " + project_name);
    print_step("Template: " + template_name);
    std::cout << std::endl;

    fs::create_directories(dest);

    // ── Generate files ──────────────────────────────────────────────────
    generate_template(template_name, project_name, dest, libanyar_root);

    // ── Install frontend dependencies ───────────────────────────────────
    print_step("Installing frontend dependencies...");
    int rc = run("npm install", dest / "frontend");
    if (rc != 0) {
        print_error("npm install failed. You can run it manually later.");
    } else {
        print_success("Frontend dependencies installed");
    }

    // ── Done ────────────────────────────────────────────────────────────
    std::cout << std::endl;
    print_success("Project created: " + dest.string());
    std::cout << std::endl;
    std::cout << "  Next steps:" << std::endl;
    std::cout << "    cd " << project_name << std::endl;
    std::cout << "    anyar dev        # Start development" << std::endl;
    std::cout << "    anyar build      # Build for production" << std::endl;
    std::cout << std::endl;

    return 0;
}

} // namespace anyar_cli
