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
    --embed           Embed frontend into binary (single-file deployment)
    --package FORMAT  Package after build (Linux: deb, appimage, all;
                      Windows: zip, installer, all)
    --version VER     Application version for packaging (default: 0.1.0)
    --publisher NAME  Installer publisher (Windows; default: app name)
    --install-scope S Windows installer: user (default, no admin prompt) or
                      machine (Program Files, needs admin)
    --webview2 MODE   Windows installer: bootstrapper (default: installs the
                      WebView2 runtime if missing) or skip

  Code signing (Windows, Authenticode via signtool):
    --sign                 Sign the app (and installer + uninstaller) using
                           the ANYAR_SIGN_* environment variables
    --sign-cert FILE.pfx   PFX certificate; password in ANYAR_SIGN_PASSWORD
    --sign-thumbprint SHA1 Certificate from the Windows certificate store
    --sign-command "CMD"   Custom signer, {file} = path (Azure Trusted
                           Signing, jsign, HSM wrappers, ...)
    --sign-timestamp URL   RFC 3161 timestamp server (default DigiCert;
                           "none" to skip)

  App icon (Windows): icon.ico or icon.png in the project root, assets/ or
  frontend/public/ is embedded into the exe and used by the installer.

    --help, -h        Show this help

  Must be run from a LibAnyar project directory.
)" << std::endl;
}

/// Windows: icon.ico/png → build/app-icon.ico (embedded as resource 32512 by
/// anyar_app_icon() in the project's CMakeLists.txt; reused by the
/// installer).  Elsewhere a no-op.
static void prepare_icon(const fs::path& project_dir, PackageOptions& pkg) {
#ifdef _WIN32
    fs::path icon = find_app_icon(project_dir);
    if (icon.empty()) return;
    fs::create_directories(project_dir / "build");
    fs::path ico = project_dir / "build" / "app-icon.ico";
    if (make_ico(icon, ico)) {
        print_success("App icon: " + fs::relative(icon, project_dir).generic_string());
        pkg.icon = ico;
    } else {
        print_error("Could not convert " + icon.string() + " to .ico — no app icon");
    }
#else
    (void)project_dir;
    (void)pkg;
#endif
}

int cmd_build(int argc, char* argv[]) {
    bool build_frontend = true;
    bool build_backend = true;
    bool clean = false;
    bool embed = false;
    std::string build_type = "Release";
    PackageOptions pkg;
    std::string& package_format = pkg.format;
    std::string& app_version = pkg.version;
    std::string sign_cert, sign_thumbprint, sign_timestamp, sign_command;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_build_usage(); return 0; }
        if (arg == "--release") { build_type = "Release"; continue; }
        if (arg == "--debug") { build_type = "Debug"; continue; }
        if (arg == "--no-frontend") { build_frontend = false; continue; }
        if (arg == "--no-backend") { build_backend = false; continue; }
        if (arg == "--clean") { clean = true; continue; }
        if (arg == "--embed") { embed = true; continue; }
        if (arg == "--package" && i + 1 < argc) { package_format = argv[++i]; continue; }
        if (arg == "--version" && i + 1 < argc) { app_version = argv[++i]; continue; }
        if (arg == "--publisher" && i + 1 < argc) { pkg.publisher = argv[++i]; continue; }
        if (arg == "--install-scope" && i + 1 < argc) { pkg.install_scope = argv[++i]; continue; }
        if (arg == "--webview2" && i + 1 < argc) { pkg.webview2 = argv[++i]; continue; }
        if (arg == "--sign") { pkg.sign = true; continue; }
        if (arg == "--sign-cert" && i + 1 < argc) { sign_cert = argv[++i]; pkg.sign = true; continue; }
        if (arg == "--sign-thumbprint" && i + 1 < argc) { sign_thumbprint = argv[++i]; pkg.sign = true; continue; }
        if (arg == "--sign-timestamp" && i + 1 < argc) { sign_timestamp = argv[++i]; continue; }
        if (arg == "--sign-command" && i + 1 < argc) { sign_command = argv[++i]; pkg.sign = true; continue; }
    }
    if (pkg.install_scope != "user" && pkg.install_scope != "machine") {
        print_error("--install-scope must be 'user' or 'machine'");
        return 1;
    }
    if (pkg.webview2 != "bootstrapper" && pkg.webview2 != "skip") {
        print_error("--webview2 must be 'bootstrapper' or 'skip'");
        return 1;
    }

    // Verify project directory
    fs::path project_dir = fs::current_path();
    if (!fs::exists(project_dir / "CMakeLists.txt")) {
        print_error("No CMakeLists.txt found. Run this from a LibAnyar project directory.");
        return 1;
    }

    const std::string project_name = read_project_name(project_dir / "CMakeLists.txt");

    print_header("Building " + project_name + " (" + build_type + ")");

#ifdef _WIN32
    // Signing secrets travel via the environment (inherited by makensis →
    // `anyar sign-file`), never via generated files.
    auto set_env = [](const char* k, const std::string& v) { _putenv_s(k, v.c_str()); };
    if (!sign_cert.empty()) set_env("ANYAR_SIGN_CERT", fs::absolute(sign_cert).string());
    if (!sign_thumbprint.empty()) set_env("ANYAR_SIGN_THUMBPRINT", sign_thumbprint);
    if (!sign_timestamp.empty()) set_env("ANYAR_SIGN_TIMESTAMP", sign_timestamp);
    if (!sign_command.empty()) set_env("ANYAR_SIGN_COMMAND", sign_command);
    if (pkg.sign && !signing_configured()) {
        print_error("--sign needs --sign-cert <pfx> (+ ANYAR_SIGN_PASSWORD), --sign-thumbprint "
                    "<sha1> or --sign-command \"<cmd {file}>\" (or the ANYAR_SIGN_* variables)");
        return 1;
    }
#else
    if (pkg.sign) {
        print_error("--sign is only implemented for Windows builds");
        return 1;
    }
#endif

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
        std::string cmake_cmd = "cmake .. -DCMAKE_BUILD_TYPE=" + build_type +
                                platform_configure_args(build_dir);
        cmake_cmd += embed ? " -DANYAR_EMBED_FRONTEND=ON" : " -DANYAR_EMBED_FRONTEND=OFF";
        prepare_icon(project_dir, pkg);  // after --clean, which wipes build/
        if (!pkg.icon.empty()) {
            cmake_cmd += " -DANYAR_APP_ICON=" + shell_quote(pkg.icon.generic_string());
        }
        int rc = run(cmake_cmd, build_dir);
        if (rc != 0) {
            print_error("CMake configuration failed");
            return 1;
        }

        unsigned int cores = std::thread::hardware_concurrency();
        if (cores == 0) cores = 4;

        print_step("Compiling C++ backend...");
        rc = run("cmake --build . --config " + build_type + " --parallel " +
                 std::to_string(cores), build_dir);
        if (rc != 0) {
            print_error("C++ build failed");
            return 1;
        }

        // Check binary exists
        fs::path binary = find_app_binary(build_dir, project_name, build_type);
        if (!binary.empty()) {
            auto size = fs::file_size(binary);
            std::string size_str;
            if (size > 1024 * 1024) {
                size_str = std::to_string(size / (1024 * 1024)) + " MB";
            } else {
                size_str = std::to_string(size / 1024) + " KB";
            }
            print_success("Binary: " + fs::relative(binary, project_dir).generic_string() +
                          " (" + size_str + ")");
        }
    }

    // ── Done ────────────────────────────────────────────────────────────
    std::cout << std::endl;
    print_success("Build complete!");

    // ── 3) Package if requested ─────────────────────────────────────────
    if (!package_format.empty()) {
        std::cout << std::endl;
        fs::path build_dir = project_dir / "build";
        pkg.build_type = build_type;
        if (pkg.icon.empty()) prepare_icon(project_dir, pkg);  // --no-backend
        int rc = package_app(pkg, project_name, project_dir, build_dir);
        if (rc != 0) return rc;
    }

#ifdef _WIN32
    // --sign without --package: sign the built binary in place.
    if (pkg.sign && package_format.empty() && build_backend) {
        fs::path binary = find_app_binary(project_dir / "build", project_name, build_type);
        if (binary.empty() || !sign_file(binary, project_name)) return 1;
    }
#endif

    if (build_backend && package_format.empty()) {
        fs::path binary = find_app_binary(project_dir / "build", project_name, build_type);
        if (!binary.empty()) {
            std::cout << std::endl;
            std::cout << "  Run your app:" << std::endl;
            std::cout << "    " << fs::relative(binary, project_dir).make_preferred().string()
                      << std::endl;
            std::cout << std::endl;
        }
    }

    return 0;
}

} // namespace anyar_cli
