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
// msi:       build/<name>-<version>-win64.msi        (WiX v4+, generated .wxs)
//   - per-machine (GPO / Intune / SCCM), Start Menu shortcut, major upgrades
//     keyed by a stable UpgradeCode, same WebView2 bootstrapper logic as a
//     deferred custom action

#include "cli.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

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
        f << generate_nsi(name, opts, st,
                          bootstrapper.empty() ? fs::path() : fs::absolute(bootstrapper), out_exe);
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

// ── MSI (WiX Toolset v4+) ────────────────────────────────────────────────────

/// wix.exe on PATH, as a .NET global tool, or in a WiX MSI install.
std::string find_wix() {
    if (has_command("wix")) return "wix";
    if (const char* home = std::getenv("USERPROFILE")) {
        fs::path p = fs::path(home) / ".dotnet" / "tools" / "wix.exe";
        if (fs::exists(p)) return p.string();
    }
    std::vector<fs::path> found;
    for (const char* var : {"ProgramFiles", "ProgramFiles(x86)"}) {
        const char* pf = std::getenv(var);
        if (!pf) continue;
        std::error_code ec;
        for (const auto& d : fs::directory_iterator(pf, ec)) {
            if (d.path().filename().string().rfind("WiX Toolset v", 0) != 0) continue;
            fs::path p = d.path() / "bin" / "wix.exe";
            if (fs::exists(p)) found.push_back(p);
        }
    }
    if (found.empty()) return {};
    std::sort(found.begin(), found.end());
    return found.back().string();
}

/// XML attribute text, with WiX preprocessor/bind-variable openers escaped.
std::string wix_str(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool opener = i + 1 < s.size() && s[i + 1] == '(';
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if ((c == '$' || c == '!') && opener) { out += c; out += c; }
        else out += c;
    }
    return out;
}

/// "1.2.3-beta" → "1.2.3": MSI ProductVersion is major.minor.build
/// (≤255.≤255.≤65535; Windows Installer ignores a 4th field for upgrades).
/// Empty if out of range.
std::string msi_version(const std::string& v) {
    const std::string v4 = four_part_version(v);
    unsigned long p[3] = {};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        size_t dot = v4.find('.', pos);
        p[i] = std::stoul(v4.substr(pos, dot - pos));
        pos = dot + 1;
    }
    if (p[0] > 255 || p[1] > 255 || p[2] > 65535) return {};
    return std::to_string(p[0]) + "." + std::to_string(p[1]) + "." + std::to_string(p[2]);
}

/// "{xxxxxxxx-…}" / "xxxxxxxx-…" → upper-case without braces; empty if not a GUID.
std::string normalize_guid(std::string g) {
    if (g.size() == 38 && g.front() == '{' && g.back() == '}') g = g.substr(1, 36);
    if (g.size() != 36) return {};
    for (size_t i = 0; i < g.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? g[i] != '-' : !std::isxdigit(static_cast<unsigned char>(g[i]))) return {};
        g[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(g[i])));
    }
    return g;
}

/// Stable UpgradeCode: RFC 4122 name-based (v5, SHA-1) UUID of
/// "<publisher>/<name>" (lower-case) in a LibAnyar namespace.  The same app
/// therefore gets the same code on every machine and release, so a newer MSI
/// replaces the old one (MajorUpgrade).
std::string derived_upgrade_code(const std::string& publisher, const std::string& name) {
    // Namespace UUID (random, fixed forever): 3b0f6f52-9a7c-4f0e-8d1b-5c2e7a4d9f60
    const unsigned char ns[16] = {0x3b, 0x0f, 0x6f, 0x52, 0x9a, 0x7c, 0x4f, 0x0e,
                                  0x8d, 0x1b, 0x5c, 0x2e, 0x7a, 0x4d, 0x9f, 0x60};
    std::string key = publisher + "/" + name;
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::vector<unsigned char> data(ns, ns + 16);
    data.insert(data.end(), key.begin(), key.end());

    unsigned char hash[20] = {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return {};
    const bool ok = BCryptHash(alg, nullptr, 0, data.data(), static_cast<ULONG>(data.size()),
                               hash, sizeof(hash)) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    hash[6] = static_cast<unsigned char>((hash[6] & 0x0F) | 0x50);  // version 5
    hash[8] = static_cast<unsigned char>((hash[8] & 0x3F) | 0x80);  // RFC 4122 variant
    char out[37];
    snprintf(out, sizeof(out),
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], hash[8],
             hash[9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);
    return out;
}

std::string generate_wxs(const std::string& name, const PackageOptions& opts, const Staged& st,
                         const std::string& version, const std::string& upgrade_code,
                         const fs::path& bootstrapper) {
    const std::string publisher = opts.publisher.empty() ? name : opts.publisher;
    const std::string exe = st.exe_name.string();
    // Paths are relative to the .wxs (written into stage_root, wix runs there).
    const std::string stage = wix_str(st.stem);

    std::string s;
    s += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    s += "<!-- Generated by anyar build (package: msi); do not edit. -->\n";  // no "--" in XML comments
    s += "<Wix xmlns=\"http://wixtoolset.org/schemas/v4/wxs\">\n";
    s += "  <Package Name=\"" + wix_str(name) + "\" Manufacturer=\"" + wix_str(publisher) +
         "\" Version=\"" + version + "\" UpgradeCode=\"" + upgrade_code +
         "\" Scope=\"perMachine\" Compressed=\"yes\" Language=\"1033\">\n";
    s += "    <SummaryInformation Description=\"" + wix_str(name) + " " + wix_str(opts.version) +
         "\" />\n";
    s += "    <MajorUpgrade DowngradeErrorMessage=\"A newer version of [ProductName] is already "
         "installed.\" />\n";
    s += "    <MediaTemplate EmbedCab=\"yes\" CompressionLevel=\"high\" />\n";
    s += "    <Property Id=\"ARPNOMODIFY\" Value=\"1\" />\n";
    if (!opts.icon.empty()) {
        s += "    <Icon Id=\"AppIcon.ico\" SourceFile=\"" +
             wix_str(fs::absolute(opts.icon).string()) + "\" />\n";
        s += "    <Property Id=\"ARPPRODUCTICON\" Value=\"AppIcon.ico\" />\n";
    }
    s += "\n    <StandardDirectory Id=\"ProgramFiles64Folder\">\n";
    s += "      <Directory Id=\"INSTALLFOLDER\" Name=\"" + wix_str(name) + "\" />\n";
    s += "    </StandardDirectory>\n";
    s += "    <StandardDirectory Id=\"ProgramMenuFolder\" />\n\n";

    s += "    <ComponentGroup Id=\"AppFiles\" Directory=\"INSTALLFOLDER\">\n";
    s += "      <Component Id=\"MainExecutable\">\n";
    s += "        <File Id=\"MainExecutable\" Source=\"" + stage + "\\" + wix_str(exe) +
         "\" KeyPath=\"yes\">\n";
    s += "          <Shortcut Id=\"StartMenuShortcut\" Directory=\"ProgramMenuFolder\" Name=\"" +
         wix_str(name) + "\" WorkingDirectory=\"INSTALLFOLDER\" Advertise=\"no\" />\n";
    s += "        </File>\n      </Component>\n";
    s += "      <Files Include=\"" + stage + "\\**\">\n";
    s += "        <Exclude Files=\"" + stage + "\\" + wix_str(exe) + "\" />\n";
    s += "      </Files>\n";
    s += "    </ComponentGroup>\n";
    s += "    <Feature Id=\"Main\" Title=\"" + wix_str(name) + "\">\n";
    s += "      <ComponentGroupRef Id=\"AppFiles\" />\n";
    s += "    </Feature>\n";

    if (!bootstrapper.empty()) {
        // Same detection as the NSIS installer: the Evergreen runtime's "pv"
        // per machine (32-bit view) or per user.  The bootstrapper runs as
        // LocalSystem (deferred, no impersonation) → machine-wide runtime.
        // Return="ignore": an offline machine still gets the app; deploy the
        // standalone runtime separately there (docs/packaging.md).
        const std::string key = "SOFTWARE\\" + std::string(kWebView2ClientKey);
        s += "\n    <Property Id=\"WVRT_MACHINE\">\n";
        s += "      <RegistrySearch Id=\"WebView2Machine\" Root=\"HKLM\" Key=\"" + wix_str(key) +
             "\" Name=\"pv\" Type=\"raw\" Bitness=\"always32\" />\n    </Property>\n";
        s += "    <Property Id=\"WVRT_USER\">\n";
        s += "      <RegistrySearch Id=\"WebView2User\" Root=\"HKCU\" Key=\"" + wix_str(key) +
             "\" Name=\"pv\" Type=\"raw\" />\n    </Property>\n";
        s += "    <Binary Id=\"WebView2Setup\" SourceFile=\"" + wix_str(bootstrapper.string()) +
             "\" />\n";
        s += "    <CustomAction Id=\"InstallWebView2\" BinaryRef=\"WebView2Setup\" "
             "ExeCommand=\"/silent /install\" Execute=\"deferred\" Impersonate=\"no\" "
             "Return=\"ignore\" />\n";
        s += "    <InstallExecuteSequence>\n";
        s += "      <Custom Action=\"InstallWebView2\" Before=\"InstallFinalize\" Condition=\""
             "NOT REMOVE AND (NOT WVRT_MACHINE OR WVRT_MACHINE = &quot;0.0.0.0&quot;) AND "
             "(NOT WVRT_USER OR WVRT_USER = &quot;0.0.0.0&quot;)\" />\n";
        s += "    </InstallExecuteSequence>\n";
    }
    s += "  </Package>\n</Wix>\n";
    return s;
}

/// @param optional  `--package all`: skip (with a note) when WiX is missing.
int package_msi(const std::string& name, const fs::path& build_dir, const PackageOptions& opts,
                bool optional) {
    print_header("Packaging " + name + " (msi, per-machine)");

    const std::string wix = find_wix();
    if (wix.empty()) {
        if (optional) {
            print_info("WiX Toolset not found — skipping the MSI "
                       "(dotnet tool install --global wix)");
            return 0;
        }
        print_error("wix (WiX Toolset v4+) not found.");
        print_info("Install it with: dotnet tool install --global wix");
        print_info("(or the WiX MSI from https://wixtoolset.org; needs the .NET 6+ SDK/runtime)");
        return 1;
    }

    const std::string version = msi_version(opts.version);
    if (version.empty()) {
        print_error("Version " + opts.version + " does not fit MSI's major.minor.build "
                    "(max 255.255.65535)");
        return 1;
    }
    const std::string publisher = opts.publisher.empty() ? name : opts.publisher;
    std::string upgrade_code;
    if (!opts.upgrade_code.empty()) {
        upgrade_code = normalize_guid(opts.upgrade_code);
        if (upgrade_code.empty()) {
            print_error("--upgrade-code is not a GUID: " + opts.upgrade_code);
            return 1;
        }
    } else {
        upgrade_code = derived_upgrade_code(publisher, name);
        if (upgrade_code.empty()) {
            print_error("Could not derive an UpgradeCode — pass --upgrade-code");
            return 1;
        }
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

    const fs::path out_msi = fs::absolute(build_dir / (st.stem + ".msi"));
    const fs::path wxs = st.stage_root / (st.stem + ".wxs");
    {
        std::ofstream f(wxs, std::ios::binary);
        f << generate_wxs(name, opts, st, version, upgrade_code,
                          bootstrapper.empty() ? fs::path() : fs::absolute(bootstrapper));
    }
    std::error_code ec;
    fs::remove(out_msi, ec);

    print_step("Running wix build...");
    int rc = run(shell_quote(wix) + " build -nologo -arch x64 -o " + shell_quote(out_msi.string()) +
                 " " + shell_quote(wxs.filename().string()), st.stage_root);
    if (rc != 0 || !fs::exists(out_msi)) {
        print_error("wix build failed (" + std::to_string(rc) + ") — source: " + wxs.string());
        return 1;
    }
    // wix leaves a .wixpdb (debug symbols) next to the output
    fs::path pdb = out_msi;
    pdb.replace_extension(".wixpdb");
    fs::remove(pdb, ec);

    if (opts.sign && !sign_file(out_msi, name + " Setup")) return 1;
    print_success("MSI: " + out_msi.string() + " (" + human_size(fs::file_size(out_msi)) + ")");
    print_info("UpgradeCode " + upgrade_code + (opts.upgrade_code.empty()
                   ? " (derived from publisher + name; pin it with --upgrade-code)" : ""));
    print_info("Silent install: msiexec /i " + out_msi.filename().string() +
               " /qn   (uninstall: msiexec /x " + out_msi.filename().string() + " /qn)");
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
    if (opts.format == "msi") return package_msi(project_name, build_dir, opts, false);
    if (opts.format == "all") {
        int rc = package_zip(project_name, build_dir, opts);
        if (rc != 0) return rc;
        std::cout << std::endl;
        rc = package_installer(project_name, build_dir, opts);
        if (rc != 0) return rc;
        std::cout << std::endl;
        return package_msi(project_name, build_dir, opts, true);
    }
    print_error("Unsupported package format on Windows: " + opts.format);
    print_info("Supported formats: zip, installer, msi, all  (deb / appimage are Linux-only)");
    return 1;
}

} // namespace anyar_cli
