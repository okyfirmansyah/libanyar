// anyar CLI — main entry point
// Usage: anyar <command> [options]
//   init  — Create a new LibAnyar project
//   dev   — Start frontend dev server + C++ backend
//   build — Build frontend + C++ backend for release

#include "cli.h"
#include <iostream>
#include <cstring>

static void print_usage() {
    std::cout << R"(
  ╭──────────────────────────────────╮
  │         anyar CLI v0.1.0         │
  ╰──────────────────────────────────╯

  Usage: anyar <command> [options]

  Commands:
    init    Create a new LibAnyar project
    dev     Start dev server (frontend HMR + C++ backend)
    build   Build frontend + C++ backend for production

  Options:
    --help, -h     Show this help message
    --version, -v  Show version

  Examples:
    anyar init myapp
    anyar dev
    anyar build
    anyar build --release
)" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    if (cmd == "--version" || cmd == "-v") {
        std::cout << "anyar 0.1.0" << std::endl;
        return 0;
    }

    if (cmd == "init") {
        return anyar_cli::cmd_init(argc - 1, argv + 1);
    }

    if (cmd == "dev") {
        return anyar_cli::cmd_dev(argc - 1, argv + 1);
    }

    if (cmd == "build") {
        return anyar_cli::cmd_build(argc - 1, argv + 1);
    }

    anyar_cli::print_error("Unknown command: " + cmd);
    print_usage();
    return 1;
}
