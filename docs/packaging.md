# Packaging & Distribution

LibAnyar apps can be packaged directly from the CLI:

- **Linux:** **DEB** packages (Ubuntu/Debian) and **AppImage** bundles (portable).
- **Windows:** a portable **zip** and an **NSIS installer** (`setup.exe`). The installer bundles the WebView2 runtime bootstrapper, and you can optionally **code-sign** it. Both formats embed the **app icon** — see [Windows](#windows).

## Quick Start

```bash
# ── Windows ──
anyar build --package installer --version 1.0.0     # build/myapp-1.0.0-setup.exe
anyar build --package zip --version 1.0.0           # build/myapp-1.0.0-win64.zip
anyar build --package all --version 1.0.0 --sign-cert cert.pfx   # both, signed

# ── Linux ──
# DEB package
anyar build --package deb --version 1.0.0

# Portable AppImage
anyar build --package appimage --version 1.0.0

# Both at once
anyar build --package all --version 1.0.0
```

Output files appear in `build/`:

```
build/
├── myapp_1.0.0_amd64.deb
└── myapp-1.0.0-x86_64.AppImage
```

## CLI Reference

| Flag | Values | Default | Description |
|------|--------|---------|-------------|
| `--package` | Linux: `deb`, `appimage`, `all` · Windows: `zip`, `installer` (alias `nsis`), `all` | *(none — skip packaging)* | Package format(s) to produce |
| `--version` | Semver string | `0.1.0` | Application version embedded in the package |
| `--publisher` | Text | app name | Windows installer: publisher in Add/Remove Programs |
| `--install-scope` | `user`, `machine` | `user` | Windows installer: per-user (no admin) or Program Files (admin) |
| `--webview2` | `bootstrapper`, `skip` | `bootstrapper` | Windows installer: bundle the WebView2 runtime bootstrapper |
| `--sign` / `--sign-cert` / `--sign-thumbprint` / `--sign-command` / `--sign-timestamp` | see [Code signing](#code-signing-windows) | off | Windows: Authenticode-sign the app, installer and uninstaller |

These can be combined with other `anyar build` flags:

```bash
anyar build --clean --package deb --version 2.1.0
anyar build --no-frontend --package appimage --version 0.5.0
```

---

## DEB Packages

### What Gets Generated

```
myapp_1.0.0_amd64.deb
└── DEBIAN/
│   └── control          # Package metadata + dependency list
├── usr/
│   ├── bin/
│   │   └── myapp        # Wrapper script (cd /usr/share/myapp && exec ./run)
│   └── share/
│       ├── myapp/
│       │   ├── run      # Actual binary
│       │   └── dist/    # Frontend assets
│       ├── applications/
│       │   └── myapp.desktop
│       └── icons/hicolor/256x256/apps/
│           └── myapp.svg
```

### Automatic Dependency Detection

Dependencies are detected by running `ldd` on your binary and mapping shared libraries to Debian package names. The following runtime libraries are recognized:

| Library | Debian Package |
|---------|---------------|
| `libwebkit2gtk-4.0` | `libwebkit2gtk-4.0-37` |
| `libjavascriptcoregtk-4.0` | `libjavascriptcoregtk-4.0-18` |
| `libgtk-3` / `libgdk-3` | `libgtk-3-0` |
| `libglib-2.0` / `libgio-2.0` / `libgobject-2.0` | `libglib2.0-0` |
| `libssl.so.3` / `libcrypto.so.3` | `libssl3` |
| `libstdc++` | `libstdc++6` |
| `libgcc_s` | `libgcc-s1` |
| `libc.so.6` / `libpthread` / `libm` | `libc6` |

The resulting `DEBIAN/control` `Depends:` line is populated automatically — no manual configuration needed.

### Architecture Detection

The target architecture (e.g., `amd64`, `arm64`) is read from `dpkg --print-architecture`. Falls back to `amd64` if detection fails.

### Installing & Removing

```bash
# Install
sudo dpkg -i build/myapp_1.0.0_amd64.deb

# If dependencies are missing
sudo apt install -f

# Remove
sudo dpkg -r myapp
```

### Package Name Sanitization

DEB package names must contain only lowercase alphanumerics and `+-. `. Project names with underscores or mixed case are automatically sanitized (e.g., `hello_world` → `hello-world`).

### Prerequisites

- `dpkg-deb` — pre-installed on all Debian/Ubuntu systems

---

## AppImage Bundles

### What Gets Generated

```
myapp-1.0.0-x86_64.AppImage    # Self-contained executable
```

The AppImage bundles your binary, frontend assets, all required shared libraries, a `.desktop` file, and an icon into a single portable file.

### How It Works

1. An `AppDir` is created in `build/pkg-appimage/`:
   ```
   AppDir/
   ├── AppRun              # Entry script
   ├── myapp.desktop
   ├── myapp.svg
   └── usr/
       ├── bin/myapp
       ├── share/myapp/dist/   # Frontend assets
       └── lib/                # Bundled shared libraries
   ```

2. **linuxdeploy** is automatically downloaded (if not already cached) to bundle shared libraries and create the AppImage.

3. If linuxdeploy fails, the tool falls back to **appimagetool** for manual bundling with `ldd`-resolved libraries.

### Running an AppImage

```bash
chmod +x myapp-1.0.0-x86_64.AppImage
./myapp-1.0.0-x86_64.AppImage
```

No installation required — AppImages run on any Linux distribution with a compatible glibc version.

### Prerequisites

- `wget` or `curl` — for downloading linuxdeploy on first use
- `fuse` — required by AppImage at runtime (pre-installed on most desktops)
- Optionally: `appimagetool` for manual fallback

---

## Windows

Both Windows formats ship the same staged tree, `build/pkg-win/<name>-<version>-win64/`:

```
myapp.exe      # the app (icon embedded, optionally signed)
*.dll          # runtime deps copied next to the exe by vcpkg
dist/          # frontend (absent with --embed)
README.txt     # WebView2 runtime note
```

### Zip (portable)

```powershell
anyar build --package zip --version 1.0.0    # build/myapp-1.0.0-win64.zip
```

Unzip and run `myapp.exe`. The target machine needs the WebView2 runtime. It ships with Windows 11 and with current Windows 10 builds.

### Installer (NSIS)

```powershell
anyar build --package installer --version 1.0.0 --publisher "Acme Inc."
# build/myapp-1.0.0-setup.exe   (silent install: myapp-1.0.0-setup.exe /S)
```

- **Scope:** `--install-scope user` is the default. It installs to `%LOCALAPPDATA%\Programs\<name>` and needs no admin prompt. `machine` installs to Program Files and asks for elevation.
- **WebView2:** the Evergreen bootstrapper (`MicrosoftEdgeWebview2Setup.exe`) is downloaded once into `%LOCALAPPDATA%\anyar\cache`. Its Microsoft signature is checked before it is bundled. At install time it runs only if no WebView2 runtime is registered. Pass `--webview2 skip` to leave it out.
- You get Start Menu and desktop shortcuts, an Add/Remove Programs entry, and an uninstaller that removes exactly the files it installed.
- **Prerequisite:** NSIS 3.08+ (`winget install NSIS.NSIS`). `makensis` is found on `PATH`, under `%NSIS_HOME%`, or in the default install directories.

### Code signing (Windows)

Unsigned apps trigger SmartScreen's "Windows protected your PC" warning. `anyar build` can Authenticode-sign with `signtool` (from the newest Windows SDK, or `%SIGNTOOL%`). It signs:

- the staged `myapp.exe`, which goes into both the zip and the installer;
- the installer, together with its uninstaller. NSIS signs both through `!finalize` / `!uninstfinalize` hooks that call back into `anyar sign-file`.

After signing, every file is checked with `WinVerifyTrust`. The raw build output in `build/Release/` stays unsigned.

```powershell
# PFX file (keep the password out of the command line / history)
$env:ANYAR_SIGN_PASSWORD = "…"
anyar build --package all --version 1.0.0 --sign-cert C:\keys\codesign.pfx

# Certificate already in the Windows cert store (e.g. on a hardware token)
anyar build --package installer --sign-thumbprint 0123ABCD…

# Any other signer — Azure Trusted Signing, jsign, a token wrapper, …
# "{file}" is replaced by the quoted path (appended if absent)
anyar build --package installer --sign-command "my-signer.cmd {file}"

# Sign just the built exe, no packaging
anyar build --sign-cert C:\keys\codesign.pfx
```

The flags only set environment variables for the run, so CI can set the variables directly and use a bare `--sign`:

| Variable | Flag | Meaning |
|----------|------|---------|
| `ANYAR_SIGN_COMMAND` | `--sign-command` | Custom signing command (wins over the others) |
| `ANYAR_SIGN_THUMBPRINT` | `--sign-thumbprint` | SHA-1 thumbprint of a cert in the user/machine store |
| `ANYAR_SIGN_CERT` | `--sign-cert` | `.pfx` file |
| `ANYAR_SIGN_PASSWORD` | — | `.pfx` password (environment only; never written to generated files) |
| `ANYAR_SIGN_TIMESTAMP` | `--sign-timestamp` | RFC 3161 timestamp URL (default `http://timestamp.digicert.com`; `none` disables it) |
| `SIGNTOOL` | — | Path to `signtool.exe` |

`--sign` with none of these set is an error, so you won't ship unsigned builds by accident. Timestamping keeps a signature valid after the certificate expires. Leave it on for releases.

> **Testing with a self-signed certificate:** `New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Test" -CertStoreLocation Cert:\CurrentUser\My` works with `--sign-thumbprint`. The result reports `untrusted root - test cert?`, which is expected. Only a certificate from a public CA (OV/EV) or Azure Trusted Signing builds SmartScreen reputation.

---

## Custom Icons

### Linux (DEB / AppImage)

The packager looks for an application icon in these locations (first match wins):

1. `icon.svg` or `icon.png` in the project root
2. `assets/icon.svg` or `assets/icon.png`
3. `frontend/public/icon.svg` or `frontend/public/icon.png`

If no icon is found, a **placeholder SVG** is generated using the first letter of the project name with a gradient background.

**Recommendation:** Place a 256×256 SVG or PNG icon at `icon.svg` in your project root for best results across both DEB and AppImage.

### Windows

`anyar build` looks for `icon.ico` or `icon.png` in the project root, then `assets/`, then `frontend/public/`. In each directory `.ico` wins over `.png`.

A PNG is converted to a multi-size `build/app-icon.ico` (16–256 px, PNG-compressed entries). Use a square image at least 256×256. An `.ico` file is used as-is. The icon is applied in three places:

- it is embedded into `myapp.exe` as resource `32512`. That is the id the window class loads, so the title bar, taskbar and Alt-Tab show it too;
- it becomes the installer and uninstaller icon (`MUI_ICON` / `MUI_UNICON`);
- it is used for the shortcuts, via the exe.

Projects created with `anyar init` already contain the CMake hook. For an older project, add it to the app's `CMakeLists.txt`:

```cmake
include(AnyarAppIcon)              # from libanyar/cmake
if(ANYAR_APP_ICON)
    anyar_app_icon(myapp "${ANYAR_APP_ICON}")
endif()
```

`anyar_app_icon(<target> <file.ico>)` does nothing on non-Windows platforms, so you can also call it directly with a checked-in `.ico`.

---

## Full Example

```bash
# Create and build an app
anyar init myapp
cd myapp
anyar build --package all --version 1.0.0

# Output:
#   ✓ DEB package: build/myapp_1.0.0_amd64.deb (4 MB)
#   ✓ AppImage: build/myapp-1.0.0-x86_64.AppImage (75 MB)

# Install DEB
sudo dpkg -i build/myapp_1.0.0_amd64.deb

# Or run AppImage directly
chmod +x build/myapp-1.0.0-x86_64.AppImage
./build/myapp-1.0.0-x86_64.AppImage
```

---

## Troubleshooting

### `dpkg-deb: error: package name has characters that aren't lowercase alphanums`

Your project name contains characters invalid for DEB package names (e.g., underscores). This is handled automatically — the package name is sanitized. If you still see this, ensure you're using the latest `anyar` CLI build.

### AppImage: `fuse: failed to open /dev/fuse`

AppImages require FUSE to mount themselves. Install it:

```bash
sudo apt install fuse libfuse2
```

Or extract and run without FUSE:

```bash
./myapp-1.0.0-x86_64.AppImage --appimage-extract-and-run
```

### linuxdeploy download fails

If behind a proxy or firewall, manually download linuxdeploy:

```bash
wget -O build/linuxdeploy-x86_64.AppImage \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
chmod +x build/linuxdeploy-x86_64.AppImage
```

Then re-run `anyar build --package appimage`.

### Windows: `makensis (NSIS) not found`

Install NSIS 3 (`winget install NSIS.NSIS`), or set `NSIS_HOME` to the directory containing `makensis.exe`. A portable NSIS copy also needs `NSISDIR` pointing at the same directory, so it can find its `Include/` and `Stubs/`.

### Windows: `signtool.exe not found`

Install the Windows SDK's *Signing Tools for Desktop Apps* (part of the VS Build Tools "Windows SDK" component), or set `SIGNTOOL` to a `signtool.exe`.

### Windows: `signed by … (untrusted root - test cert?)`

The file is signed, but the certificate does not chain to a trusted root. That is normal for a self-signed test certificate. Release builds need a certificate from a public CA or Azure Trusted Signing.

### Windows: `Could not download …MicrosoftEdgeWebview2Setup.exe`

Download it manually from https://go.microsoft.com/fwlink/p/?LinkId=2124703 into `%LOCALAPPDATA%\anyar\cache\MicrosoftEdgeWebview2Setup.exe`. Alternatively, build with `--webview2 skip`.
