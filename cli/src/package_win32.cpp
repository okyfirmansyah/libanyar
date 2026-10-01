// anyar CLI — Windows packaging
//
// Both formats ship the same staged tree:
//   build/pkg-win/<name>-<version>-win64/
//     <name>.exe      the app
//     *.dll           runtime deps next to the exe (vcpkg copies them there)
//     dist/           frontend (absent with --embed)
//     README.txt      WebView2 runtime note
//
// zip:       build/<name>-<version>-win64.zip        (bsdtar, Windows 10 1803+)
// installer: build/<name>-<version>-setup.exe        (NSIS, generated script)
//   - per-user (default; no admin prompt, %LOCALAPPDATA%\Programs\<name>) or
//     per-machine (Program Files, admin) — --install-scope
//   - bundles the Evergreen WebView2 bootstrapper (signature-checked) and runs
//     it only when no WebView2 runtime is registered — --webview2
//   - Start Menu + desktop shortcuts, Add/Remove Programs entry, uninstaller
//     that deletes exactly the files it installed

#include "cli.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>


namespace anyar_cli {

namespace {

// Evergreen WebView2 Runtime bootstrapper (~2 MB; downloads the runtime).
constexpr const char* kWebView2BootstrapperUrl =
    "https://go.microsoft.com/fwlink/p/?LinkId=2124703";
constexpr const char* kWebView2ClientKey =
    "Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

std::string human_size(uintmax_t bytes) {
    if (bytes > 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / 1024) + " KB";
}

struct Staged {
    std::string stem;      // <name>-<version>-win64
    fs::path stage_root;   // build/pkg-win
    fs::path stage;        // build/pkg-win/<stem>
    fs::path exe_name;     // <name>.exe
};

/// Copy the app, its DLLs and dist/ into a clean staging directory.
bool stage_app(const std::string& name, const fs::path& build_dir, const PackageOptions& opts,
               Staged& out) {
    fs::path binary = find_app_binary(build_dir, name, opts.build_type);
    if (binary.empty()) {
        print_error("Binary not found for '" + name + "' in " + build_dir.string() +
                    " — build first (anyar build)");
        return false;
    }
    const fs::path exe_dir = binary.parent_path();

    out.stem = name + "-" + opts.version + "-win64";
    out.stage_root = build_dir / "pkg-win";
    out.stage = out.stage_root / out.stem;
    out.exe_name = binary.filename();
    std::error_code ec;
    fs::remove_all(out.stage, ec);
    fs::create_directories(out.stage);

    print_step("Staging files...");
    fs::copy_file(binary, out.stage / binary.filename(), fs::copy_options::overwrite_existing);
    int dlls = 0;
    for (const auto& entry : fs::directory_iterator(exe_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dll") {
            fs::copy_file(entry.path(), out.stage / entry.path().filename(),
                          fs::copy_options::overwrite_existing);
            ++dlls;
        }
    }
    print_success(binary.filename().string() + " + " + std::to_string(dlls) + " DLL(s)");

    // Sign the staged copy (the build output stays as the compiler left it).
    if (opts.sign && !sign_file(out.stage / binary.filename(), name)) return false;

    if (fs::exists(exe_dir / "dist")) {
        fs::copy(exe_dir / "dist", out.stage / "dist", fs::copy_options::recursive);
        print_success("dist/ (frontend)");
    } else {
        print_info("No dist/ next to the binary — assuming an embedded frontend (--embed)");
    }

    std::ofstream readme(out.stage / "README.txt");
    readme << name << " " << opts.version << "\r\n\r\n"
           << "Run " << binary.filename().string() << ".\r\n\r\n"
           << "Requires the Microsoft Edge WebView2 Runtime (preinstalled on\r\n"
           << "Windows 11; for Windows 10 get it from\r\n"
           << "https://developer.microsoft.com/microsoft-edge/webview2/).\r\n";
    return true;
}

int package_zip(const std::string& name, const fs::path& build_dir, const PackageOptions& opts) {
    print_header("Packaging " + name + " (zip)");
    Staged st;
    if (!stage_app(name, build_dir, opts, st)) return 1;

    const fs::path zip = build_dir / (st.stem + ".zip");
    std::error_code ec;
    fs::remove(zip, ec);
    print_step("Creating " + zip.filename().string() + "...");
    // tar -a picks the format from the extension (bsdtar in System32).
    int rc = run("tar -a -c -f " + shell_quote(zip.string()) + " -C " +
                 shell_quote(st.stage_root.string()) + " " + shell_quote(st.stem));
    if (rc != 0 || !fs::exists(zip)) {
        print_info("tar failed — trying PowerShell Compress-Archive");
        rc = run("powershell -NoProfile -Command \"Compress-Archive -Force -Path '" +
                 st.stage.string() + "' -DestinationPath '" + zip.string() + "'\"");
    }
    if (rc != 0 || !fs::exists(zip)) {
        print_error("Could not create " + zip.string());
        return 1;
    }
    print_success("Package: " + zip.string() + " (" + human_size(fs::file_size(zip)) + ")");
    return 0;
}

// ── WebView2 bootstrapper ────────────────────────────────────────────────────

/// True if @p file carries a valid (trusted) Authenticode signature by Microsoft.
bool signed_by_microsoft(const fs::path& file) {
    const SignatureInfo s = verify_signature(file);
    return s.trusted && s.signer.rfind("Microsoft", 0) == 0;
}

/// Cached, signature-checked bootstrapper; downloaded on first use.
/// Override with ANYAR_WEBVIEW2_BOOTSTRAPPER=<path> (e.g. offline builds).
fs::path webview2_bootstrapper() {
    fs::path file;
    if (const char* env = std::getenv("ANYAR_WEBVIEW2_BOOTSTRAPPER")) {
        file = env;
    } else {
        const char* local = std::getenv("LOCALAPPDATA");
        fs::path cache = fs::path(local ? local : ".") / "anyar" / "cache";
        fs::create_directories(cache);
        file = cache / "MicrosoftEdgeWebview2Setup.exe";
        if (!fs::exists(file)) {
            print_step("Downloading the WebView2 bootstrapper...");
            fs::path tmp = file;
            tmp += ".part";
            int rc = run("curl -fsSL -o " + shell_quote(tmp.string()) + " " +
                         shell_quote(kWebView2BootstrapperUrl));
            if (rc != 0) {
                rc = run("powershell -NoProfile -Command \"Invoke-WebRequest -UseBasicParsing -Uri '" +
                         std::string(kWebView2BootstrapperUrl) + "' -OutFile '" + tmp.string() + "'\"");
            }
            std::error_code ec;
            if (rc != 0 || !fs::exists(tmp)) {
                fs::remove(tmp, ec);
                print_error("Could not download " + std::string(kWebView2BootstrapperUrl));
                return {};
            }
            fs::rename(tmp, file, ec);
        }
    }
    if (!fs::exists(file)) {
        print_error("WebView2 bootstrapper not found: " + file.string());
        return {};
    }
    if (!signed_by_microsoft(file)) {
        print_error("WebView2 bootstrapper is not validly signed by Microsoft: " + file.string());
        if (!std::getenv("ANYAR_WEBVIEW2_BOOTSTRAPPER")) {
            std::error_code ec;
            fs::remove(file, ec);  // re-download next time
        }
        return {};
    }
    print_success("WebView2 bootstrapper: " + file.string() + " (Microsoft-signed)");
    return file;
}

// ── NSIS installer ──────────────────────────────────────────────────────────

/// makensis on PATH, under $NSIS_HOME, or in the default install dirs.
std::string find_makensis() {
    if (const char* home = std::getenv("NSIS_HOME")) {
        fs::path p = fs::path(home) / "makensis.exe";
        if (fs::exists(p)) return p.string();
    }
    if (has_command("makensis")) return "makensis";
    for (const char* var : {"ProgramFiles(x86)", "ProgramFiles"}) {
        if (const char* pf = std::getenv(var)) {
            fs::path p = fs::path(pf) / "NSIS" / "makensis.exe";
            if (fs::exists(p)) return p.string();
        }
    }
    return {};
}

/// NSIS string literal contents: escape $ and ".
std::string nsis_str(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '$') out += "$$";
        else if (c == '"') out += "$\\\"";
        else out += c;
    }
    return out;
}

/// "1.2.3" / "1.2.3-beta" → "1.2.3.0" (VIProductVersion needs 4 numbers).
std::string four_part_version(const std::string& v) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur += c;
        } else if (c == '.') {
            parts.push_back(cur.empty() ? "0" : cur);
            cur.clear();
        } else {
            break;  // pre-release / build suffix
        }
    }
    parts.push_back(cur.empty() ? "0" : cur);
    while (parts.size() < 4) parts.push_back("0");
    parts.resize(4);
    return parts[0] + "." + parts[1] + "." + parts[2] + "." + parts[3];
}

std::string generate_nsi(const std::string& name, const PackageOptions& opts, const Staged& st,
                         const fs::path& bootstrapper, const fs::path& out_exe) {
    const bool machine = opts.install_scope == "machine";
    const std::string publisher = opts.publisher.empty() ? name : opts.publisher;
    const std::string root = machine ? "HKLM" : "HKCU";
    const std::string uninst_key =
        "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + name;
    const std::string exe = st.exe_name.string();

    // Files grouped by directory, plus every directory (for uninstall).
    std::map<std::string, std::vector<fs::path>> files_by_dir;
    std::set<std::string> dirs;
    uintmax_t total = 0;
    for (const auto& e : fs::recursive_directory_iterator(st.stage)) {
        std::string rel = fs::relative(e.path(), st.stage).string();
        if (e.is_directory()) {
            dirs.insert(rel);
        } else if (e.is_regular_file()) {
            files_by_dir[fs::path(rel).parent_path().string()].push_back(e.path());
            total += e.file_size();
        }
    }
    auto inst = [](const std::string& rel) {
        return rel.empty() ? std::string("$INSTDIR") : "$INSTDIR\\" + nsis_str(rel);
    };

    std::string s;
    s += "; Generated by `anyar build --package installer` — do not edit.\n";
    s += "Unicode true\n!include \"MUI2.nsh\"\n!include \"LogicLib.nsh\"\n\n";
    s += "Name \"" + nsis_str(name) + "\"\n";
    s += "OutFile \"" + nsis_str(out_exe.string()) + "\"\n";
    s += "RequestExecutionLevel " + std::string(machine ? "admin" : "user") + "\n";
    s += "InstallDir \"" + std::string(machine ? "$PROGRAMFILES64" : "$LOCALAPPDATA\\Programs") +
         "\\" + nsis_str(name) + "\"\n";
    s += "InstallDirRegKey " + root + " \"" + nsis_str(uninst_key) + "\" \"InstallLocation\"\n";
    s += "SetCompressor /SOLID lzma\nShowInstDetails show\nShowUninstDetails show\n\n";

    const std::string v4 = four_part_version(opts.version);
    s += "VIProductVersion \"" + v4 + "\"\n";
    s += "VIAddVersionKey \"ProductName\" \"" + nsis_str(name) + "\"\n";
    s += "VIAddVersionKey \"ProductVersion\" \"" + nsis_str(opts.version) + "\"\n";
    s += "VIAddVersionKey \"FileVersion\" \"" + v4 + "\"\n";
    s += "VIAddVersionKey \"CompanyName\" \"" + nsis_str(publisher) + "\"\n";
    s += "VIAddVersionKey \"LegalCopyright\" \"(c) " + nsis_str(publisher) + "\"\n";
    s += "VIAddVersionKey \"FileDescription\" \"" + nsis_str(name) + " installer\"\n\n";

    if (!opts.icon.empty()) {  // installer + uninstaller icon (the app exe embeds its own)
        s += "!define MUI_ICON \"" + nsis_str(fs::absolute(opts.icon).string()) + "\"\n";
        s += "!define MUI_UNICON \"" + nsis_str(fs::absolute(opts.icon).string()) + "\"\n";
    }
    if (opts.sign) {
        // makensis signs both outputs itself (NSIS 3.08+): the uninstaller as
        // it is generated, the installer once written.  The hooks call back
        // into this CLI, which takes the secrets from ANYAR_SIGN_* — nothing
        // sensitive is written into this script.
        const std::string self = nsis_str(executable_path().string());
        s += "!uninstfinalize '\"" + self + "\" sign-file \"%1\" \"" + nsis_str(name) +
             " Uninstaller\"' = 0\n";
        s += "!finalize '\"" + self + "\" sign-file \"%1\" \"" + nsis_str(name) +
             " Setup\"' = 0\n";
    }
    s += "!define MUI_FINISHPAGE_RUN \"$INSTDIR\\" + nsis_str(exe) + "\"\n";
    s += "!insertmacro MUI_PAGE_DIRECTORY\n!insertmacro MUI_PAGE_INSTFILES\n"
         "!insertmacro MUI_PAGE_FINISH\n";
    s += "!insertmacro MUI_UNPAGE_CONFIRM\n!insertmacro MUI_UNPAGE_INSTFILES\n";
    s += "!insertmacro MUI_LANGUAGE \"English\"\n\n";

    s += "Function .onInit\n  InitPluginsDir\n";
    s += machine ? "  SetShellVarContext all\n  SetRegView 64\n" : "  SetShellVarContext current\n";
    s += "FunctionEnd\n\n";
    s += "Function un.onInit\n";
    s += machine ? "  SetShellVarContext all\n  SetRegView 64\n" : "  SetShellVarContext current\n";
    s += "FunctionEnd\n\n";

    if (!bootstrapper.empty()) {
        // Evergreen runtime registers its version ("pv") per machine (32-bit
        // registry view → WOW6432Node) or per user.
        s += "Section \"-WebView2\"\n";
        s += "  SetRegView 32\n";
        s += "  ReadRegStr $0 HKLM \"SOFTWARE\\" + std::string(kWebView2ClientKey) + "\" \"pv\"\n";
        s += "  ${If} $0 == \"\"\n  ${OrIf} $0 == \"0.0.0.0\"\n";
        s += "    ReadRegStr $0 HKCU \"Software\\" + std::string(kWebView2ClientKey) + "\" \"pv\"\n";
        s += "  ${EndIf}\n";
        if (machine) s += "  SetRegView 64\n";
        s += "  ${If} $0 == \"\"\n  ${OrIf} $0 == \"0.0.0.0\"\n";
        s += "    DetailPrint \"Installing the Microsoft Edge WebView2 Runtime...\"\n";
        s += "    SetOutPath \"$PLUGINSDIR\"\n";
        s += "    File \"" + nsis_str(bootstrapper.string()) + "\"\n";
        s += "    ExecWait '\"$PLUGINSDIR\\" + nsis_str(bootstrapper.filename().string()) +
             "\" /silent /install' $1\n";
        s += "    ${If} $1 <> 0\n";
        s += "      MessageBox MB_ICONEXCLAMATION|MB_OK \"The WebView2 Runtime could not be "
             "installed (code $1). " + nsis_str(name) + " needs it: "
             "https://developer.microsoft.com/microsoft-edge/webview2/\" /SD IDOK\n";
        s += "    ${EndIf}\n";
        s += "  ${Else}\n    DetailPrint \"WebView2 Runtime $0 already installed\"\n  ${EndIf}\n";
        s += "SectionEnd\n\n";
    }

    s += "Section \"-App\"\n";
    for (const auto& [rel, files] : files_by_dir) {
        s += "  SetOutPath \"" + inst(rel) + "\"\n";
        for (const auto& f : files) s += "  File \"" + nsis_str(f.string()) + "\"\n";
    }
    s += "  SetOutPath \"$INSTDIR\"\n";
    s += "  WriteUninstaller \"$INSTDIR\\Uninstall.exe\"\n";
    s += "  CreateShortCut \"$SMPROGRAMS\\" + nsis_str(name) + ".lnk\" \"$INSTDIR\\" +
         nsis_str(exe) + "\"\n";
    s += "  CreateShortCut \"$DESKTOP\\" + nsis_str(name) + ".lnk\" \"$INSTDIR\\" +
         nsis_str(exe) + "\"\n";
    const std::string k = root + " \"" + nsis_str(uninst_key) + "\"";
    s += "  WriteRegStr " + k + " \"DisplayName\" \"" + nsis_str(name) + "\"\n";
    s += "  WriteRegStr " + k + " \"DisplayVersion\" \"" + nsis_str(opts.version) + "\"\n";
    s += "  WriteRegStr " + k + " \"Publisher\" \"" + nsis_str(publisher) + "\"\n";
    s += "  WriteRegStr " + k + " \"InstallLocation\" \"$INSTDIR\"\n";
    s += "  WriteRegStr " + k + " \"DisplayIcon\" \"$INSTDIR\\" + nsis_str(exe) + "\"\n";
    s += "  WriteRegStr " + k + " \"UninstallString\" '\"$INSTDIR\\Uninstall.exe\"'\n";
    s += "  WriteRegStr " + k + " \"QuietUninstallString\" '\"$INSTDIR\\Uninstall.exe\" /S'\n";
    s += "  WriteRegDWORD " + k + " \"NoModify\" 1\n";
    s += "  WriteRegDWORD " + k + " \"NoRepair\" 1\n";
    s += "  WriteRegDWORD " + k + " \"EstimatedSize\" " + std::to_string(total / 1024) + "\n";
    s += "SectionEnd\n\n";

    // Uninstall exactly what was installed — never RMDir /r $INSTDIR (the
    // user may have picked a directory that holds other files).
    s += "Section \"Uninstall\"\n";
    s += "  Delete \"$SMPROGRAMS\\" + nsis_str(name) + ".lnk\"\n";
    s += "  Delete \"$DESKTOP\\" + nsis_str(name) + ".lnk\"\n";
    for (const auto& [rel, files] : files_by_dir) {
        for (const auto& f : files) {
            s += "  Delete \"" + inst(rel) + "\\" + nsis_str(f.filename().string()) + "\"\n";
        }
    }
    std::vector<std::string> dir_list(dirs.begin(), dirs.end());
    std::sort(dir_list.begin(), dir_list.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& d : dir_list) s += "  RMDir \"" + inst(d) + "\"\n";  // deepest first
    s += "  Delete \"$INSTDIR\\Uninstall.exe\"\n";
    s += "  RMDir \"$INSTDIR\"\n";
    s += "  DeleteRegKey " + k + "\n";
    s += "SectionEnd\n";
    return s;
}

int package_installer(const std::string& name, const fs::path& build_dir,
                      const PackageOptions& opts) {
    print_header("Packaging " + name + " (installer, " + opts.install_scope + " scope)");

    const std::string makensis = find_makensis();
    if (makensis.empty()) {
        print_error("makensis (NSIS) not found.");
        print_info("Install NSIS 3 (winget install NSIS.NSIS, or https://nsis.sourceforge.io)");
        print_info("or point NSIS_HOME at a directory containing makensis.exe.");
        return 1;
    }

    Staged st;
    if (!stage_app(name, build_dir, opts, st)) return 1;

    fs::path bootstrapper;
    if (opts.webview2 == "bootstrapper") {
        bootstrapper = webview2_bootstrapper();
        if (bootstrapper.empty()) {
            print_info("Re-run with --webview2 skip to build without it.");
            return 1;
        }
    }

    const fs::path out_exe = fs::absolute(build_dir / (name + "-" + opts.version + "-setup.exe"));
    const fs::path nsi = st.stage_root / (st.stem + ".nsi");
    {
        std::ofstream f(nsi, std::ios::binary);
        f << generate_nsi(name, opts, st, fs::absolute(bootstrapper), out_exe);
    }
    std::error_code ec;
    fs::remove(out_exe, ec);

    print_step("Running makensis...");
    int rc = run(shell_quote(makensis) + " /V2 /INPUTCHARSET UTF8 " + shell_quote(nsi.string()));
    if (rc != 0 || !fs::exists(out_exe)) {
        print_error("makensis failed (" + std::to_string(rc) + ") — script: " + nsi.string());
        return 1;
    }
    print_success("Installer: " + out_exe.string() + " (" + human_size(fs::file_size(out_exe)) + ")");
    if (opts.sign) {
        const SignatureInfo s = verify_signature(out_exe);
        if (!s.signed_) {
            print_error("The installer is not signed — check the !finalize output above");
            return 1;
        }
        print_success("Installer " + describe(s));
    }
    print_info("Silent install: " + out_exe.filename().string() + " /S   " +
               "(uninstall: \"<install dir>\\Uninstall.exe\" /S)");
    return 0;
}

} // namespace

int package_app(const PackageOptions& opts,
                const std::string& project_name,
                const fs::path& /*project_dir*/,
                const fs::path& build_dir) {
    if (opts.format == "zip") return package_zip(project_name, build_dir, opts);
    if (opts.format == "installer" || opts.format == "nsis") {
        return package_installer(project_name, build_dir, opts);
    }
    if (opts.format == "all") {
        int rc = package_zip(project_name, build_dir, opts);
        if (rc != 0) return rc;
        std::cout << std::endl;
        return package_installer(project_name, build_dir, opts);
    }
    print_error("Unsupported package format on Windows: " + opts.format);
    print_info("Supported formats: zip, installer, all  (deb / appimage are Linux-only)");
    return 1;
}

} // namespace anyar_cli
