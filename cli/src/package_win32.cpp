// anyar CLI — Windows packaging: portable zip
//
// build/pkg-zip/<name>-<version>-win64/
//   <name>.exe      the app
//   *.dll           runtime deps next to the exe (vcpkg copies them there)
//   dist/           frontend (absent with --embed)
//   README.txt      WebView2 runtime note
// → build/<name>-<version>-win64.zip  (bsdtar, bundled with Windows 10 1803+)
//
// MSI/NSIS installers are roadmap 7.4.

#include "cli.h"

#include <fstream>
#include <iostream>

namespace anyar_cli {

static std::string human_size(uintmax_t bytes) {
    if (bytes > 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / 1024) + " KB";
}

static int package_zip(const std::string& project_name, const fs::path& build_dir,
                       const std::string& version, const std::string& build_type) {
    print_header("Packaging " + project_name + " (zip)");

    fs::path binary = find_app_binary(build_dir, project_name, build_type);
    if (binary.empty()) {
        print_error("Binary not found for '" + project_name + "' in " + build_dir.string() +
                    " — build first (anyar build)");
        return 1;
    }
    const fs::path exe_dir = binary.parent_path();

    const std::string stem = project_name + "-" + version + "-win64";
    const fs::path stage_root = build_dir / "pkg-zip";
    const fs::path stage = stage_root / stem;
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::create_directories(stage);

    print_step("Staging files...");
    fs::copy_file(binary, stage / binary.filename(), fs::copy_options::overwrite_existing);
    int dlls = 0;
    for (const auto& entry : fs::directory_iterator(exe_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dll") {
            fs::copy_file(entry.path(), stage / entry.path().filename(),
                          fs::copy_options::overwrite_existing);
            ++dlls;
        }
    }
    print_success(binary.filename().string() + " + " + std::to_string(dlls) + " DLL(s)");

    if (fs::exists(exe_dir / "dist")) {
        fs::copy(exe_dir / "dist", stage / "dist", fs::copy_options::recursive);
        print_success("dist/ (frontend)");
    } else {
        print_info("No dist/ next to the binary — assuming an embedded frontend (--embed)");
    }

    {
        std::ofstream readme(stage / "README.txt");
        readme << project_name << " " << version << "\r\n\r\n"
               << "Run " << binary.filename().string() << ".\r\n\r\n"
               << "Requires the Microsoft Edge WebView2 Runtime (preinstalled on\r\n"
               << "Windows 11; for Windows 10 get it from\r\n"
               << "https://developer.microsoft.com/microsoft-edge/webview2/).\r\n";
    }

    const fs::path zip = build_dir / (stem + ".zip");
    fs::remove(zip, ec);
    print_step("Creating " + zip.filename().string() + "...");
    // tar -a picks the format from the extension (bsdtar in System32).
    int rc = run("tar -a -c -f " + shell_quote(zip.string()) + " -C " +
                 shell_quote(stage_root.string()) + " " + shell_quote(stem));
    if (rc != 0 || !fs::exists(zip)) {
        print_info("tar failed — trying PowerShell Compress-Archive");
        rc = run("powershell -NoProfile -Command \"Compress-Archive -Force -Path '" +
                 stage.string() + "' -DestinationPath '" + zip.string() + "'\"");
    }
    if (rc != 0 || !fs::exists(zip)) {
        print_error("Could not create " + zip.string());
        return 1;
    }

    print_success("Package: " + zip.string() + " (" + human_size(fs::file_size(zip)) + ")");
    return 0;
}

int package_app(const std::string& format,
                const std::string& project_name,
                const fs::path& /*project_dir*/,
                const fs::path& build_dir,
                const std::string& version,
                const std::string& build_type) {
    if (format == "zip" || format == "all") {
        return package_zip(project_name, build_dir, version, build_type);
    }
    print_error("Unsupported package format on Windows: " + format);
    print_info("Supported formats: zip, all  (deb / appimage are Linux-only)");
    return 1;
}

} // namespace anyar_cli
