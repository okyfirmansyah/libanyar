# cli/ — `anyar` CLI Tool

## Purpose
C++ CLI binary that scaffolds, runs, builds, and packages LibAnyar projects. Builds on Linux (GCC) and Windows (MSVC).

## Layout

```
cli/
├── CMakeLists.txt          # platform source split; cli_init_smoke CTest (ANYAR_BUILD_TESTS)
├── tests/init_smoke.cmake  # cmake -P: --version, build --help, init --no-install, path checks
└── src/
    ├── main.cpp            # arg parsing → dispatch; init_console() first
    ├── cli.h               # shared decls: ChildProcess, run/run_bg/wait_child/kill_child, package_app, build helpers
    ├── cmd_init.cpp        # scaffold project (svelte-ts/react-ts/vanilla), --no-install
    ├── cmd_dev.cpp         # vite + C++ backend concurrently
    ├── cmd_build.cpp       # frontend build + cmake build (--embed via cmrc) + --package
    ├── cmd_package.cpp     # Linux: DEB + AppImage (linuxdeploy)
    ├── package_win32.cpp   # Windows: portable zip + NSIS installer (WebView2 bootstrapper)
    ├── process_posix.cpp   # fork/exec `sh -c`, signals, /proc/self/exe
    ├── process_win32.cpp   # CreateProcess `cmd.exe /d /s /c`, job objects, Ctrl+C, VT console
    ├── templates.cpp       # gen_cmake / gen_main_cpp / gen_agent_instructions / gen_gitignore / gen_readme / gen_{svelte_ts,react_ts,vanilla}
    └── util.cpp            # colours, prompts, find_libanyar_root, read_project_name, platform_configure_args, find_app_binary
```

## Commands
```bash
anyar init <name> [-t svelte-ts|react-ts|vanilla] [--no-install]   # interactive picker if -t omitted
anyar dev [--no-frontend] [--no-backend]
anyar build [--release|--debug] [--embed] [--clean] [--no-frontend] [--no-backend] [--package deb|appimage|zip|installer|all] [--version VER] [--publisher NAME] [--install-scope user|machine] [--webview2 bootstrapper|skip]
```

## Templates
- **svelte-ts** (preferred), **react-ts**, **vanilla**
- Tailwind CSS 4 only in svelte-ts; react-ts and vanilla use plain CSS
- Dark theme via CSS custom properties
- Paths embedded in generated files (CMake `LIBANYAR_DIR`, vite `@libanyar/api` alias) use `generic_string()` — a Windows `C:\Users\…` in a CMake/JS string is an escape sequence (checked by `cli_init_smoke`)

## Generated Project Skeleton
```
my-app/
├── CMakeLists.txt
├── src-cpp/main.cpp     # lifecycle comment + app.command / app.http_get / (commented) allow_file_access / app.on_ready
├── frontend/            # Vite project (alias '@libanyar/api' → <libanyar>/js-bridge/src)
├── .github/copilot-instructions.md  # shutdown rules for app/plugin authors (gen_agent_instructions)
├── CLAUDE.md            # #import .github/copilot-instructions.md
├── .gitignore
└── README.md
```

## Conventions
- C++ naming: `snake_case` funcs, `PascalCase` classes
- Generated files emitted via `templates.cpp` helpers
- **Avoid nested raw-string literals**: use `"\"key\":\"value\""` inside an outer `R"(...)"` to prevent parser errors
- `find_libanyar_root()` walks up from cwd, then from the `anyar` binary (`executable_path()`), then `$LIBANYAR_DIR`, looking for `ARCHITECTURE.md` + `core/CMakeLists.txt` (don't rename/move those)
- Process/OS code goes in `process_<os>.cpp` behind `cli.h`; command files stay platform-neutral. Quote paths in command strings with `shell_quote()`
- Builds use `cmake --build . --config <type> --parallel N` (works for make, Ninja and multi-config VS); locate binaries with `find_app_binary()` (`build/<type>/<name>.exe` or `build/<name>`)

## Windows specifics
- Commands run via `cmd.exe /d /s /c "<cmd>"` (so `npm` = npm.cmd works); `run_bg()` children are created suspended inside a kill-on-close job object → `kill_child()` ends the whole cmd → npm → node tree
- `platform_configure_args()` on a fresh build dir: `-DCMAKE_TOOLCHAIN_FILE` from `$VCPKG_ROOT` or `C:\vcpkg`, `-DCMAKE_PREFIX_PATH` from `$ANYAR_LIBASYIK_PREFIX` or `<libanyar>/build-deps/libasyik`
- `init_console()`: UTF-8 output code page + ANSI VT sequences; MSVC builds with `/utf-8`

## Build Flags
- `--embed` → `-DANYAR_EMBED_FRONTEND=ON` (otherwise `=OFF` explicitly, so a cached ON doesn't stick)
- Linux: `--package deb` (deps via `ldd` → Debian package map), `--package appimage` (downloads `linuxdeploy`); outputs `build/<deb_name>.deb`, `build/<name>-<ver>-<arch>.AppImage`
- Windows: both formats share a staged tree `build/pkg-win/<name>-<ver>-win64/` (exe + every DLL next to it + `dist/` + README). `--package zip` → `build/<name>-<ver>-win64.zip` via `tar -a` (Compress-Archive fallback). `--package installer` (alias `nsis`) → generated `build/pkg-win/<stem>.nsi` → `makensis` → `build/<name>-<ver>-setup.exe`: MUI2 pages, per-user (default, `%LOCALAPPDATA%Programs<name>`, HKCU, no UAC) or per-machine (`--install-scope machine`: `$PROGRAMFILES64`, HKLM, admin); Start Menu + desktop shortcuts; Add/Remove Programs entry (`--publisher`); uninstaller deletes the generated file list + `RMDir` (never `/r`, so a shared install dir keeps foreign files). WebView2: the Evergreen bootstrapper (cached in `%LOCALAPPDATA%nyarche`, override `ANYAR_WEBVIEW2_BOOTSTRAPPER`) must pass `WinVerifyTrust` with a Microsoft signer; it runs (`/silent /install`) only when `EdgeUpdateClients{F3017226-…}pv` is missing in HKLM (32-bit view) and HKCU; `--webview2 skip` omits it. `makensis` from `$NSIS_HOME`, PATH or Program Files (non-standard layouts also need `NSISDIR`). `VIProductVersion` = first 4 numeric parts of `--version`. MSI: not implemented

## Dev Mode (`anyar dev`)
1. Spawn `npm run dev` in `frontend/` (Vite, port 5173, `strictPort:false`)
2. `cmake .. -DCMAKE_BUILD_TYPE=Debug` + `cmake --build . --config Debug` in `build/`, then run the binary (Linux: via libanyar-root `run.sh` if found)
3. Wait for the app to exit, then kill Vite (tree); Ctrl+C / SIGTERM kills both. `--no-backend` keeps Vite running until Ctrl+C
- No dev-URL redirect exists: the webview still loads the backend's `serve_static` dist, not the Vite server

## Build Mode (`anyar build`)
1. `npm install` if `frontend/node_modules` missing; `npm run build` → `frontend/dist/`
2. in `build/`: `cmake .. -DCMAKE_BUILD_TYPE=Release|Debug -DANYAR_EMBED_FRONTEND=ON|OFF` (+ platform args; `--clean` wipes `build/` first)
3. `cmake --build . --config <type> --parallel <cores>`
4. If `--package`: `package_app()` (per-OS)

## Testing
`cli_init_smoke` (label `cli`, runs in both CI jobs): `--version`, `build --help`, `init smokeapp --template vanilla --no-install`, then checks the files and that `LIBANYAR_DIR` / the vite alias point at the repo with forward slashes. Full flow (manual, needs Node): `anyar init demo` in a temp dir → `anyar build` → `anyar build --package zip|deb` → `anyar dev`.

## Modifying Templates
Update `templates.cpp` `gen_main_cpp()` etc. Keep the generated shutdown guidance (main.cpp lifecycle comment + `gen_agent_instructions`) consistent with docs/graceful-shutdown.md. After change, regenerate an example to verify output compiles:
```bash
cd /tmp && rm -rf cli-smoke && LIBANYAR_DIR=<repo> anyar init cli-smoke -t svelte-ts && cd cli-smoke && anyar build
# init takes a NAME (created under cwd), not a path
```
