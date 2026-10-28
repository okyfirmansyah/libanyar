# Build and ship

## New project

```bash
anyar init my-app -t svelte-ts      # or react-ts | vanilla; omit -t for a picker
cd my-app
anyar dev                           # Vite HMR + C++ backend
anyar build                         # release build (frontend + C++)
```

Generated layout:

```
my-app/
├── CMakeLists.txt        # LIBANYAR_DIR → LibAnyar source tree; anyar_embed_frontend() when ANYAR_EMBED_FRONTEND
├── src-cpp/main.cpp      # App setup, a sample command
├── frontend/             # Vite project; alias '@libanyar/api' → <libanyar>/js-bridge/src
├── .github/copilot-instructions.md, CLAUDE.md   # shutdown rules for agents
└── README.md
```

`anyar` finds the LibAnyar tree by walking up from the current directory, then from the `anyar` binary's location, then `$LIBANYAR_DIR`.

Prerequisites:
- **Linux (Ubuntu 22.04):** `build-essential cmake pkg-config libwebkit2gtk-4.0-dev libgtk-3-dev libssl-dev nlohmann-json3-dev`, LibAsyik 1.7.1+ installed (Boost 1.81+, SOCI), Node 18+.
- **Windows:** VS 2022 C++ workload, vcpkg. In the LibAnyar repo, `scripts\setup-windows.ps1` installs the dependencies and LibAsyik 1.8.1. On a fresh build directory the CLI adds the vcpkg toolchain and the LibAsyik prefix itself.

## CMake essentials

```cmake
set(CMAKE_CXX_STANDARD 17)
list(APPEND CMAKE_MODULE_PATH "${LIBANYAR_DIR}/cmake")
add_subdirectory(${LIBANYAR_DIR}/core ${CMAKE_BINARY_DIR}/anyar_core)
add_executable(my_app src-cpp/main.cpp src-cpp/my_plugin.cpp)
target_link_libraries(my_app PRIVATE anyar_core)
# frontend: anyar_embed_frontend(my_app "${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist") when ANYAR_EMBED_FRONTEND,
# otherwise copy frontend/dist next to the binary (POST_BUILD copy_directory)
```

- **Embedding** (`anyar build --embed`, or `-DANYAR_EMBED_FRONTEND=ON`) compiles `dist/` into the binary through CMakeRC; `main.cpp` must call `app.set_frontend_resolver(anyar::make_embedded_resolver())` under `#ifdef ANYAR_EMBED_FRONTEND`.
- The embedded file list is globbed at **configure** time. After each frontend build, re-run `cmake -B build` (the CLI does this for you), or the binary keeps serving stale hashed asset names.
- Your own binary assets (data packs, models) can be embedded the same way with `cmrc_add_resource_library(...)` + `CMRC_DECLARE`. Large ones belong in a download, not in Git (below).

## Packaging

```bash
anyar build --package deb --version 1.0.0          # Linux .deb (deps from ldd)
anyar build --package appimage --version 1.0.0     # Linux AppImage (linuxdeploy)
anyar build --package zip --version 1.0.0          # Windows portable zip
anyar build --package installer --version 1.0.0 --publisher "Acme"   # Windows NSIS setup.exe (+ WebView2 bootstrapper)
anyar build --package msi --version 1.0.0 --publisher "Acme"         # Windows MSI (WiX v4+), per-machine
anyar build --package all --version 1.0.0          # every format for this OS
```

Windows notes:
- Sign with `--sign-cert file.pfx`, `--sign-thumbprint <sha1>` or `--sign-command "signer {file}"`.
- Pin `--upgrade-code <GUID>` before the first MSI release; otherwise it's derived from publisher + name.
- App icon (first match wins, in the project root, then `assets/`, then `frontend/public/`):
  - Linux: `icon.svg` or `icon.png`;
  - Windows: `icon.ico` or `icon.png`, at least 256×256; a PNG is converted to a multi-size `.ico` and embedded in the exe and installers.

## Portability checklist (Linux + Windows today, macOS later)

- C++17 only; MSVC rejects GCC extensions (`a ?: b`). A local `constexpr` used inside a lambda must be `static constexpr` on MSVC.
- No `unistd.h`, `getpid()`, `/proc` or `/tmp` in shared code: use `std::filesystem::temp_directory_path()`, `<anyar/path.h>` for UTF-8 paths, and per-OS files for OS calls.
- Find SQLite with `find_package(SQLite3)`, not pkg-config.
- Windows differences:
  - no `anyar-shm://` (SharedBuffers use WebView2 shared memory instead, transparently through `fetchBuffer`);
  - `anyar-file://` is unavailable (use `/__anyar__/file/`);
  - the first page load waits until `run()`, so register init scripts before it.

## Third-party data and licences

If the app ships data you didn't create (3D models, datasets, fonts, icons):
- **Check the licence at the source, today.** Licences change: BodyParts3D moved from CC BY-SA to CC BY 4.0 while its downloadable files still carried the old header.
- **Only commit permissive data:** CC0, public domain, CC BY, MIT/BSD/Apache. Keep share-alike (CC BY-SA) data as an opt-in download in its own folder, and never ship NC (non-commercial) data.
- **Put each source in its own folder** with a `LICENSE` and a `MANIFEST.json`: source URL, version, licence, exact attribution text, SHA-256 of what you downloaded, and the command that produced your derived files. Show the attribution in the app (About dialog / credits line) and in a root `NOTICE`.
- **Host large packs as GitHub Release assets**, fetched by CMake `file(DOWNLOAD … EXPECTED_HASH SHA256=…)`. Avoid Git LFS: it has bandwidth quotas and makes every clone heavy.
- **Medical, financial or safety-related content:** show a clear "not for clinical/professional use" disclaimer.

`examples/anatomy-atlas` in the LibAnyar repo is a complete worked example of this setup, including a CI licence gate (`scripts/check_assets.py`).
