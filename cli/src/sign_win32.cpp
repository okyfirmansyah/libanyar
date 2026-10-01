// anyar CLI — Authenticode signing + verification (Windows)
//
// Configuration comes from environment variables, so that secrets never land
// in generated files.  `anyar build --sign-*` flags only set them for this
// process; NSIS (!finalize / !uninstfinalize) calls back into
// `anyar sign-file <path>`, which inherits them:
//
//   ANYAR_SIGN_COMMAND     custom command; "{file}" is replaced by the quoted
//                          path (else appended) — Azure Trusted Signing, jsign,
//                          a hardware-token wrapper, …
//   ANYAR_SIGN_THUMBPRINT  SHA-1 thumbprint of a cert in the user/machine store
//   ANYAR_SIGN_CERT        .pfx file  (+ ANYAR_SIGN_PASSWORD)
//   ANYAR_SIGN_TIMESTAMP   RFC 3161 URL (default DigiCert; "none" = no timestamp)
//   SIGNTOOL               signtool.exe override (default: newest Windows SDK)
//
// Precedence: command > thumbprint > cert.

#include "cli.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace anyar_cli {

namespace {

constexpr const char* kDefaultTimestamp = "http://timestamp.digicert.com";

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

/// signtool.exe: $SIGNTOOL, PATH, then the newest Windows 10/11 SDK.
std::string find_signtool() {
    std::string override_path = env("SIGNTOOL");
    if (!override_path.empty() && fs::exists(override_path)) return override_path;
    if (has_command("signtool")) return "signtool";
    std::vector<fs::path> found;
    for (const char* var : {"ProgramFiles(x86)", "ProgramFiles"}) {
        std::string pf = env(var);
        if (pf.empty()) continue;
        fs::path bin = fs::path(pf) / "Windows Kits" / "10" / "bin";
        std::error_code ec;
        if (!fs::exists(bin, ec)) continue;
        for (const auto& d : fs::directory_iterator(bin, ec)) {
            fs::path tool = d.path() / "x64" / "signtool.exe";
            if (fs::exists(tool)) found.push_back(tool);
        }
    }
    if (found.empty()) return {};
    std::sort(found.begin(), found.end());  // "10.0.26100.0" sorts after older SDKs
    return found.back().string();
}

} // namespace

bool signing_configured() {
    return !env("ANYAR_SIGN_COMMAND").empty() || !env("ANYAR_SIGN_THUMBPRINT").empty() ||
           !env("ANYAR_SIGN_CERT").empty();
}

SignatureInfo verify_signature(const fs::path& file) {
    SignatureInfo info;
    std::wstring path = file.wstring();
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path.c_str();
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &wd);
    info.status = status;
    info.signed_ = status != TRUST_E_NOSIGNATURE && status != TRUST_E_SUBJECT_FORM_UNKNOWN &&
                   status != TRUST_E_PROVIDER_UNKNOWN;
    info.trusted = status == ERROR_SUCCESS;

    if (CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData)) {
        CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0);
        if (signer && signer->csCertChain > 0 && signer->pasCertChain[0].pCert) {
            wchar_t subject[256] = {};
            CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                               nullptr, subject, 256);
            info.signer = narrow(subject);
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &wd);
    return info;
}

std::string describe(const SignatureInfo& s) {
    if (!s.signed_) return "unsigned";
    std::string who = s.signer.empty() ? "unknown signer" : s.signer;
    if (s.trusted) return "signed by " + who + " (trusted)";
    if (s.status == CERT_E_UNTRUSTEDROOT) return "signed by " + who + " (untrusted root - test cert?)";
    char hex[16];
    snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(s.status));
    return "signed by " + who + " (not trusted: " + hex + ")";
}

bool sign_file(const fs::path& file, const std::string& description) {
    const std::string command = env("ANYAR_SIGN_COMMAND");
    const std::string thumbprint = env("ANYAR_SIGN_THUMBPRINT");
    const std::string cert = env("ANYAR_SIGN_CERT");
    if (command.empty() && thumbprint.empty() && cert.empty()) {
        print_error("Signing requested but not configured (--sign-cert, --sign-thumbprint or "
                    "--sign-command)");
        return false;
    }
    print_step("Signing " + file.filename().string() + "...");

    int rc;
    if (!command.empty()) {
        std::string cmd = command;
        const std::string quoted = shell_quote(file.string());
        auto pos = cmd.find("{file}");
        if (pos != std::string::npos) cmd.replace(pos, 6, quoted);
        else cmd += " " + quoted;
        rc = run(cmd);
    } else {
        const std::string signtool = find_signtool();
        if (signtool.empty()) {
            print_error("signtool.exe not found — install the Windows SDK or set SIGNTOOL");
            return false;
        }
        std::string cmd = shell_quote(signtool) + " sign /q /fd sha256";
        std::string ts = env("ANYAR_SIGN_TIMESTAMP");
        if (ts.empty()) ts = kDefaultTimestamp;
        if (ts != "none") cmd += " /tr " + shell_quote(ts) + " /td sha256";
        if (!thumbprint.empty()) {
            cmd += " /sha1 " + shell_quote(thumbprint);
        } else {
            cmd += " /f " + shell_quote(cert);
            const std::string pwd = env("ANYAR_SIGN_PASSWORD");
            if (!pwd.empty()) cmd += " /p " + shell_quote(pwd);
        }
        if (!description.empty()) cmd += " /d " + shell_quote(description);
        cmd += " " + shell_quote(file.string());
        rc = run(cmd);  // never echoed (the password is on this command line)
    }
    if (rc != 0) {
        print_error("Signing " + file.filename().string() + " failed (" + std::to_string(rc) + ")");
        return false;
    }
    const SignatureInfo s = verify_signature(file);
    if (!s.signed_) {
        print_error(file.filename().string() + " is still unsigned after signing");
        return false;
    }
    print_success(file.filename().string() + ": " + describe(s));
    return true;
}

int cmd_sign_file(int argc, char* argv[]) {
    set_plain_output(true);  // makensis relays (and re-encodes) our output
    if (argc < 2) {
        print_error("usage: anyar sign-file <file> [<description>]");
        return 2;
    }
    return sign_file(argv[1], argc > 2 ? argv[2] : "") ? 0 : 1;
}

} // namespace anyar_cli
