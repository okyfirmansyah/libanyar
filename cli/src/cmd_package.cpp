// anyar CLI — Linux packaging (DEB + AppImage)
//
// Generates distributable .deb packages and AppImage bundles from a built
// LibAnyar application.  Called by `anyar build --package deb|appimage|all`.

#include "cli.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <sys/stat.h>

namespace anyar_cli {

// ── Helpers ─────────────────────────────────────────────────────────────────

/// Read ldd output and return a sorted list of Debian package dependencies
/// for common runtime libraries.
static std::vector<std::string> detect_deb_deps(const fs::path& binary) {
    // We define a known mapping of .so patterns → Debian package names.
    // dpkg-shlibdeps could automate this, but requires a full debian/ source
    // tree.  This curated list covers all libs that libanyar apps link to.
    struct SoMapping {
        const char* pattern; // substring to match in ldd output
        const char* pkg;     // Debian package name
    };

    static const SoMapping mappings[] = {
        {"libwebkit2gtk-4.0",     "libwebkit2gtk-4.0-37"},
        {"libjavascriptcoregtk-4.0", "libjavascriptcoregtk-4.0-18"},
        {"libgtk-3.so",          "libgtk-3-0"},
        {"libgdk-3.so",          "libgtk-3-0"},
        {"libglib-2.0.so",       "libglib2.0-0"},
        {"libgio-2.0.so",        "libglib2.0-0"},
        {"libgobject-2.0.so",    "libglib2.0-0"},
        {"libssl.so.3",          "libssl3"},
        {"libcrypto.so.3",       "libssl3"},
        {"libstdc++.so",         "libstdc++6"},
        {"libgcc_s.so",          "libgcc-s1"},
        {"libc.so.6",            "libc6"},
        {"libpthread.so",        "libc6"},
        {"libm.so",              "libc6"},
    };

    std::vector<std::string> deps;

    // Run ldd
    std::string cmd = "ldd " + binary.string() + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return deps;

    char buf[512];
    std::string ldd_output;
    while (fgets(buf, sizeof(buf), pipe)) {
        ldd_output += buf;
    }
    pclose(pipe);

    // Match against known patterns
    for (const auto& m : mappings) {
        if (ldd_output.find(m.pattern) != std::string::npos) {
            std::string pkg = m.pkg;
            // Avoid duplicates
            if (std::find(deps.begin(), deps.end(), pkg) == deps.end()) {
                deps.push_back(pkg);
            }
        }
    }

    std::sort(deps.begin(), deps.end());
    return deps;
}

/// Detect the CPU architecture string for DEB (`amd64`, `arm64`, etc.)
static std::string detect_deb_arch() {
    FILE* pipe = popen("dpkg --print-architecture 2>/dev/null", "r");
    if (!pipe) return "amd64";
    char buf[64];
    std::string arch;
    if (fgets(buf, sizeof(buf), pipe)) {
        arch = buf;
        // trim newline
        while (!arch.empty() && (arch.back() == '\n' || arch.back() == '\r'))
            arch.pop_back();
    }
    pclose(pipe);
    return arch.empty() ? "amd64" : arch;
}

/// Generate a .desktop file content
static std::string make_desktop_entry(const std::string& app_name,
                                       const std::string& display_name,
                                       const std::string& comment,
                                       const std::string& icon_name,
                                       const std::string& categories) {
    std::ostringstream ss;
    ss << "[Desktop Entry]\n"
       << "Type=Application\n"
       << "Name=" << display_name << "\n"
       << "Comment=" << comment << "\n"
       << "Exec=" << app_name << "\n"
       << "Icon=" << icon_name << "\n"
       << "Terminal=false\n"
       << "Categories=" << categories << ";\n"
       << "StartupWMClass=" << app_name << "\n";
    return ss.str();
}

/// Create a simple default SVG icon (placeholder) if no icon exists
static std::string make_default_icon_svg(const std::string& app_name) {
    // A simple colored rectangle with the first letter of the app name
    char letter = 'A';
    if (!app_name.empty()) letter = std::toupper(app_name[0]);
    std::ostringstream ss;
    ss << R"(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect width="256" height="256" rx="32" fill="#6366f1"/>
  <text x="128" y="178" font-family="sans-serif" font-size="160" font-weight="bold"
        fill="white" text-anchor="middle">)" << letter << R"(</text>
</svg>)";
    return ss.str();
}

/// Turn "hello_world" or "hello-world" into "Hello World"
static std::string humanize_name(const std::string& name) {
    std::string result;
    bool next_upper = true;
    for (char c : name) {
        if (c == '_' || c == '-') {
            result += ' ';
            next_upper = true;
        } else if (next_upper) {
            result += std::toupper(c);
            next_upper = false;
        } else {
            result += c;
        }
    }
    return result;
}

/// Sanitize a project name for use as a Debian package name.
/// Rules: lowercase, only [a-z0-9.+-], must start with alphanum.
static std::string sanitize_deb_name(const std::string& name) {
    std::string result;
    for (char c : name) {
        char lc = std::tolower(static_cast<unsigned char>(c));
        if (std::isalnum(static_cast<unsigned char>(lc)) || lc == '+' || lc == '.') {
            result += lc;
        } else {
            // Replace underscores, spaces, and other invalid chars with '-'
            if (!result.empty() && result.back() != '-') {
                result += '-';
            }
        }
    }
    // Strip trailing hyphens
    while (!result.empty() && result.back() == '-') result.pop_back();
    return result;
}

// ── DEB Packaging ───────────────────────────────────────────────────────────

int package_deb(const std::string& project_name,
                const fs::path& project_dir,
                const fs::path& build_dir,
                const std::string& version) {
    print_step("Packaging DEB...");

    fs::path binary = build_dir / project_name;
    if (!fs::exists(binary)) {
        print_error("Binary not found: " + binary.string());
        print_error("Run `anyar build` first.");
        return 1;
    }

    std::string arch = detect_deb_arch();
    std::string display_name = humanize_name(project_name);
    std::string pkg_name = sanitize_deb_name(project_name);  // DEB-safe name
    std::string deb_name = pkg_name + "_" + version + "_" + arch;
    fs::path pkg_dir = build_dir / "pkg-deb" / deb_name;

    // Clean previous packaging
    if (fs::exists(pkg_dir)) {
        fs::remove_all(pkg_dir);
    }

    // ── Create directory structure ──────────────────────────────────────
    fs::path bin_dir      = pkg_dir / "usr" / "bin";
    fs::path share_dir    = pkg_dir / "usr" / "share" / project_name;
    fs::path desktop_dir  = pkg_dir / "usr" / "share" / "applications";
    fs::path icon_dir     = pkg_dir / "usr" / "share" / "icons" / "hicolor" / "256x256" / "apps";
    fs::path debian_dir   = pkg_dir / "DEBIAN";

    fs::create_directories(bin_dir);
    fs::create_directories(share_dir);
    fs::create_directories(desktop_dir);
    fs::create_directories(icon_dir);
    fs::create_directories(debian_dir);

    // ── Copy binary ────────────────────────────────────────────────────
    fs::copy_file(binary, bin_dir / project_name, fs::copy_options::overwrite_existing);
    // Set executable permission
    chmod((bin_dir / project_name).c_str(), 0755);

    // ── Copy frontend dist ─────────────────────────────────────────────
    fs::path frontend_dist = build_dir / "dist";
    if (!fs::exists(frontend_dist)) {
        // Try project-level frontend/dist
        frontend_dist = project_dir / "frontend" / "dist";
    }
    if (fs::exists(frontend_dist)) {
        fs::copy(frontend_dist, share_dir / "dist",
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);

        // Create a wrapper script that sets the dist path
        std::string wrapper = "#!/bin/sh\nexec /usr/share/" + project_name +
                              "/run \"$@\"\n";
        // Move binary to share_dir as "run", replace bin entry with wrapper
        fs::rename(bin_dir / project_name, share_dir / "run");
        chmod((share_dir / "run").c_str(), 0755);

        {
            std::ofstream f(bin_dir / project_name);
            f << "#!/bin/sh\n"
              << "cd /usr/share/" << project_name << "\n"
              << "exec ./run \"$@\"\n";
        }
        chmod((bin_dir / project_name).c_str(), 0755);
    }

    // ── Desktop entry ──────────────────────────────────────────────────
    {
        std::ofstream f(desktop_dir / (project_name + ".desktop"));
        f << make_desktop_entry(project_name, display_name,
                                display_name + " — built with LibAnyar",
                                project_name, "Utility");
    }

    // ── Icon ───────────────────────────────────────────────────────────
    // Check for user-provided icon in project
    fs::path user_icon;
    for (const auto& candidate : {
        project_dir / "icon.svg",
        project_dir / "icon.png",
        project_dir / "assets" / "icon.svg",
        project_dir / "assets" / "icon.png",
        project_dir / "frontend" / "public" / "icon.svg",
        project_dir / "frontend" / "public" / "icon.png",
    }) {
        if (fs::exists(candidate)) { user_icon = candidate; break; }
    }

    if (!user_icon.empty()) {
        fs::copy_file(user_icon, icon_dir / (project_name + user_icon.extension().string()),
                      fs::copy_options::overwrite_existing);
    } else {
        // Generate a placeholder SVG
        std::ofstream f(icon_dir / (project_name + ".svg"));
        f << make_default_icon_svg(project_name);
    }

    // ── DEBIAN/control ─────────────────────────────────────────────────
    auto deps = detect_deb_deps(binary);
    std::string dep_str;
    for (size_t i = 0; i < deps.size(); i++) {
        if (i > 0) dep_str += ", ";
        dep_str += deps[i];
    }

    {
        std::ofstream f(debian_dir / "control");
        f << "Package: " << pkg_name << "\n"
          << "Version: " << version << "\n"
          << "Section: utils\n"
          << "Priority: optional\n"
          << "Architecture: " << arch << "\n"
          << "Depends: " << dep_str << "\n"
          << "Maintainer: developer <developer@example.com>\n"
          << "Description: " << display_name << "\n"
          << " A desktop application built with LibAnyar.\n";
    }

    // ── Build the .deb ─────────────────────────────────────────────────
    fs::path output_deb = build_dir / (deb_name + ".deb");
    std::string cmd = "dpkg-deb --build --root-owner-group " +
                      pkg_dir.string() + " " + output_deb.string();
    int rc = run(cmd);
    if (rc != 0) {
        print_error("dpkg-deb failed");
        return 1;
    }

    auto size = fs::file_size(output_deb);
    std::string size_str;
    if (size > 1024 * 1024) {
        size_str = std::to_string(size / (1024 * 1024)) + " MB";
    } else {
        size_str = std::to_string(size / 1024) + " KB";
    }

    print_success("DEB package: build/" + deb_name + ".deb (" + size_str + ")");
    print_info("Install with: sudo dpkg -i build/" + deb_name + ".deb");
    return 0;
}

// ── AppImage Packaging ──────────────────────────────────────────────────────

/// Download linuxdeploy if not already cached
static fs::path ensure_linuxdeploy(const fs::path& build_dir) {
    fs::path tools_dir = build_dir / ".appimage-tools";
    fs::create_directories(tools_dir);
    fs::path linuxdeploy = tools_dir / "linuxdeploy-x86_64.AppImage";

    if (fs::exists(linuxdeploy)) return linuxdeploy;

    print_step("Downloading linuxdeploy...");
    std::string url = "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage";
    std::string cmd = "wget -q -O " + linuxdeploy.string() + " \"" + url + "\"";

    // Try wget, fall back to curl
    if (!has_command("wget")) {
        cmd = "curl -fsSL -o " + linuxdeploy.string() + " \"" + url + "\"";
    }

    int rc = run(cmd);
    if (rc != 0) {
        print_error("Failed to download linuxdeploy");
        print_info("Download manually from: " + url);
        return "";
    }

    chmod(linuxdeploy.c_str(), 0755);
    return linuxdeploy;
}

int package_appimage(const std::string& project_name,
                     const fs::path& project_dir,
                     const fs::path& build_dir,
                     const std::string& version) {
    print_step("Packaging AppImage...");

    fs::path binary = build_dir / project_name;
    if (!fs::exists(binary)) {
        print_error("Binary not found: " + binary.string());
        print_error("Run `anyar build` first.");
        return 1;
    }

    std::string display_name = humanize_name(project_name);
    std::string arch = detect_deb_arch();
    // AppImage uses different arch names
    std::string appimage_arch = (arch == "amd64") ? "x86_64" : arch;

    fs::path appdir = build_dir / "AppDir";

    // Clean previous
    if (fs::exists(appdir)) {
        fs::remove_all(appdir);
    }

    // ── Create AppDir structure ─────────────────────────────────────────
    fs::path appdir_bin   = appdir / "usr" / "bin";
    fs::path appdir_share = appdir / "usr" / "share" / project_name;
    fs::path appdir_apps  = appdir / "usr" / "share" / "applications";
    fs::path appdir_icons = appdir / "usr" / "share" / "icons" / "hicolor" / "256x256" / "apps";

    fs::create_directories(appdir_bin);
    fs::create_directories(appdir_share);
    fs::create_directories(appdir_apps);
    fs::create_directories(appdir_icons);

    // ── Copy binary ────────────────────────────────────────────────────
    fs::copy_file(binary, appdir_bin / project_name, fs::copy_options::overwrite_existing);
    chmod((appdir_bin / project_name).c_str(), 0755);

    // ── Copy frontend dist ─────────────────────────────────────────────
    fs::path frontend_dist = build_dir / "dist";
    if (!fs::exists(frontend_dist)) {
        frontend_dist = project_dir / "frontend" / "dist";
    }
    if (fs::exists(frontend_dist)) {
        fs::copy(frontend_dist, appdir_share / "dist",
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }

    // ── AppRun script ──────────────────────────────────────────────────
    {
        std::ofstream f(appdir / "AppRun");
        f << "#!/bin/sh\n"
          << "SELF=$(readlink -f \"$0\")\n"
          << "HERE=$(dirname \"$SELF\")\n"
          << "export LD_LIBRARY_PATH=\"${HERE}/usr/lib:${LD_LIBRARY_PATH}\"\n"
          << "export PATH=\"${HERE}/usr/bin:${PATH}\"\n"
          << "cd \"${HERE}/usr/share/" << project_name << "\"\n"
          << "exec \"${HERE}/usr/bin/" << project_name << "\" \"$@\"\n";
    }
    chmod((appdir / "AppRun").c_str(), 0755);

    // ── Desktop entry ──────────────────────────────────────────────────
    std::string desktop_content = make_desktop_entry(
        project_name, display_name,
        display_name + " — built with LibAnyar",
        project_name, "Utility");

    // Desktop file at top level (required by AppImage) AND in share/applications
    {
        std::ofstream f(appdir / (project_name + ".desktop"));
        f << desktop_content;
    }
    {
        std::ofstream f(appdir_apps / (project_name + ".desktop"));
        f << desktop_content;
    }

    // ── Icon ───────────────────────────────────────────────────────────
    fs::path user_icon;
    for (const auto& candidate : {
        project_dir / "icon.svg",
        project_dir / "icon.png",
        project_dir / "assets" / "icon.svg",
        project_dir / "assets" / "icon.png",
        project_dir / "frontend" / "public" / "icon.svg",
        project_dir / "frontend" / "public" / "icon.png",
    }) {
        if (fs::exists(candidate)) { user_icon = candidate; break; }
    }

    std::string icon_ext;
    if (!user_icon.empty()) {
        icon_ext = user_icon.extension().string();
        fs::copy_file(user_icon, appdir_icons / (project_name + icon_ext),
                      fs::copy_options::overwrite_existing);
    } else {
        icon_ext = ".svg";
        std::ofstream f(appdir_icons / (project_name + ".svg"));
        f << make_default_icon_svg(project_name);
    }
    // Also copy to top-level AppDir (required by some AppImage tools)
    if (fs::exists(appdir_icons / (project_name + icon_ext))) {
        fs::copy_file(appdir_icons / (project_name + icon_ext),
                      appdir / (project_name + icon_ext),
                      fs::copy_options::overwrite_existing);
    }

    // ── Bundle shared libraries ─────────────────────────────────────────
    // Try using linuxdeploy for automatic library bundling
    fs::path linuxdeploy = ensure_linuxdeploy(build_dir);

    fs::path output_appimage = build_dir / (project_name + "-" + version + "-" +
                                            appimage_arch + ".AppImage");
    if (fs::exists(output_appimage)) {
        fs::remove(output_appimage);
    }

    if (!linuxdeploy.empty() && fs::exists(linuxdeploy)) {
        // Use linuxdeploy for proper library bundling + AppImage creation
        std::string icon_path = (appdir_icons / (project_name + icon_ext)).string();
        std::string desktop_path = (appdir / (project_name + ".desktop")).string();

        // linuxdeploy creates the AppImage directly
        std::string cmd = "ARCH=" + appimage_arch +
                          " OUTPUT=" + output_appimage.string() +
                          " " + linuxdeploy.string() +
                          " --appdir " + appdir.string() +
                          " --executable " + binary.string() +
                          " --desktop-file " + desktop_path +
                          " --icon-file " + icon_path +
                          " --output appimage" +
                          " 2>&1";
        int rc = run(cmd);
        if (rc != 0) {
            print_error("linuxdeploy failed — trying manual AppImage build");
            // Fall through to manual method
        } else if (fs::exists(output_appimage)) {
            auto size = fs::file_size(output_appimage);
            std::string size_str;
            if (size > 1024 * 1024) {
                size_str = std::to_string(size / (1024 * 1024)) + " MB";
            } else {
                size_str = std::to_string(size / 1024) + " KB";
            }
            print_success("AppImage: build/" + output_appimage.filename().string() +
                          " (" + size_str + ")");
            print_info("Run with: chmod +x " + output_appimage.filename().string() +
                       " && ./" + output_appimage.filename().string());
            return 0;
        }
    }

    // ── Manual fallback: bundle key libs + use appimagetool ────────────
    print_step("Manual AppImage bundling (no linuxdeploy)...");

    // Copy Boost shared libs (not system-installed on most distributions)
    fs::path lib_dir = appdir / "usr" / "lib";
    fs::create_directories(lib_dir);

    std::string ldd_cmd = "ldd " + binary.string() + " 2>/dev/null";
    FILE* ldd_pipe = popen(ldd_cmd.c_str(), "r");
    if (ldd_pipe) {
        char buf[512];
        // Bundle non-system libs (Boost, libasyik, etc. from /usr/local/lib)
        while (fgets(buf, sizeof(buf), ldd_pipe)) {
            std::string line = buf;
            // Look for libs in /usr/local/lib (non-standard location)
            auto arrow = line.find("=> ");
            if (arrow == std::string::npos) continue;
            auto path_start = arrow + 3;
            auto path_end = line.find(" (", path_start);
            if (path_end == std::string::npos) continue;
            std::string lib_path = line.substr(path_start, path_end - path_start);
            // Trim whitespace
            while (!lib_path.empty() && lib_path.front() == ' ') lib_path.erase(0, 1);
            while (!lib_path.empty() && lib_path.back() == ' ') lib_path.pop_back();

            // Bundle libs from /usr/local (Boost, libasyik)
            if (lib_path.find("/usr/local/") == 0 && fs::exists(lib_path)) {
                fs::path dest = lib_dir / fs::path(lib_path).filename();
                if (!fs::exists(dest)) {
                    fs::copy_file(lib_path, dest);
                }
            }
        }
        pclose(ldd_pipe);
    }

    // Try to create AppImage with appimagetool
    if (has_command("appimagetool")) {
        std::string cmd = "ARCH=" + appimage_arch +
                          " appimagetool " + appdir.string() +
                          " " + output_appimage.string();
        int rc = run(cmd);
        if (rc == 0 && fs::exists(output_appimage)) {
            auto size = fs::file_size(output_appimage);
            std::string size_str = std::to_string(size / (1024 * 1024)) + " MB";
            print_success("AppImage: " + output_appimage.filename().string() +
                          " (" + size_str + ")");
            return 0;
        }
    }

    // If no appimagetool, output AppDir as the result
    print_success("AppDir created: build/AppDir/");
    print_info("To create AppImage, install appimagetool and run:");
    print_info("  ARCH=" + appimage_arch + " appimagetool build/AppDir " +
               output_appimage.filename().string());
    return 0;
}

// ── Public Entry Point ──────────────────────────────────────────────────────

int package_linux(const std::string& format,
                  const std::string& project_name,
                  const fs::path& project_dir,
                  const fs::path& build_dir,
                  const std::string& version) {
    if (format == "deb") {
        return package_deb(project_name, project_dir, build_dir, version);
    } else if (format == "appimage") {
        return package_appimage(project_name, project_dir, build_dir, version);
    } else if (format == "all") {
        int rc = package_deb(project_name, project_dir, build_dir, version);
        if (rc != 0) return rc;
        std::cout << std::endl;
        return package_appimage(project_name, project_dir, build_dir, version);
    }
    print_error("Unknown package format: " + format);
    print_info("Supported formats: deb, appimage, all");
    return 1;
}

} // namespace anyar_cli
