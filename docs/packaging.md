# Packaging & Distribution

LibAnyar apps can be packaged as **DEB** packages (Ubuntu/Debian) and **AppImage** bundles (portable Linux) directly from the CLI.

## Quick Start

```bash
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
| `--package` | `deb`, `appimage`, `all` | *(none — skip packaging)* | Package format(s) to produce |
| `--version` | Semver string | `0.1.0` | Application version embedded in the package |

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

## Custom Icons

The packager looks for an application icon in these locations (first match wins):

1. `icon.svg` or `icon.png` in the project root
2. `assets/icon.svg` or `assets/icon.png`
3. `frontend/public/icon.svg` or `frontend/public/icon.png`

If no icon is found, a **placeholder SVG** is generated using the first letter of the project name with a gradient background.

**Recommendation:** Place a 256×256 SVG or PNG icon at `icon.svg` in your project root for best results across both DEB and AppImage.

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
