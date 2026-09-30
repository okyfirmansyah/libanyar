# cli/ — `anyar` CLI Tool

## Purpose
C++ CLI binary that scaffolds, runs, builds, and packages LibAnyar projects.

**Linux-only for now**: uses fork/exec, `popen`, `/proc/self/exe`, `chmod`, dpkg. The root `CMakeLists.txt` skips `cli/` on Windows (ADR-010); a port needs `CreateProcess`/job objects for `dev`, and MSI/NSIS for `package`.

## Layout

```
cli/
├── CMakeLists.txt
└── src/
    ├── main.cpp        # arg parsing → dispatch
    ├── cli.h           # shared types, util decls
    ├── cmd_init.cpp    # scaffold project (svelte-ts/react-ts/vanilla)
    ├── cmd_dev.cpp     # vite + C++ backend concurrently
    ├── cmd_build.cpp   # frontend build + cmake build (--embed via cmrc)
    ├── cmd_package.cpp # DEB + AppImage (linuxdeploy)
    ├── templates.cpp   # gen_cmake / gen_main_cpp / gen_agent_instructions / gen_gitignore / gen_readme / gen_{svelte_ts,react_ts,vanilla}
    └── util.cpp        # exe-path finder (readlink /proc/self/exe — guarded)
```

## Commands
```bash
anyar init <name> [-t svelte-ts|react-ts|vanilla]   # interactive picker if -t omitted
anyar dev [--no-frontend] [--no-backend]
anyar build [--release|--debug] [--embed] [--clean] [--no-frontend] [--no-backend] [--package deb|appimage|all] [--version VER]
```

## Templates
- **svelte-ts** (preferred), **react-ts**, **vanilla**
- Tailwind CSS 4 only in svelte-ts; react-ts and vanilla use plain CSS
- Dark theme via CSS custom properties

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
- `find_libanyar_root()` walks up from cwd, then from the `anyar` binary, looking for `ARCHITECTURE.md` + `core/CMakeLists.txt` (don't rename/move those); `init` falls back to `$LIBANYAR_DIR`
- Exe-path discovery is platform-guarded (`#ifdef __linux__` for `/proc/self/exe`); add Win32 (`GetModuleFileNameW`) / macOS (`_NSGetExecutablePath`) branches before porting

## Build Flags
- `--embed` → passes `-DANYAR_EMBED_FRONTEND=ON` to CMake (cmrc compiles `dist/` into binary)
- `--package deb` → builds DEB with auto-detected deps via `ldd` → Debian package map
- `--package appimage` → downloads `linuxdeploy`, creates AppDir, bundles libs
- Outputs land in project `build/`: `build/<name>`, `build/<deb_name>.deb`, `build/<name>-<ver>-<arch>.AppImage`

## Dev Mode (`anyar dev`)
1. Spawn `npm run dev` in `frontend/` (Vite, port 5173, `strictPort:false`)
2. `cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j` in `build/`, then run `build/<name>` (via libanyar-root `run.sh` if found)
3. Wait for the app to exit, then SIGTERM Vite; SIGINT/SIGTERM handler kills both
- No dev-URL redirect exists: the webview still loads the backend's `serve_static` dist, not the Vite server

## Build Mode (`anyar build`)
1. `npm install` if `frontend/node_modules` missing; `npm run build` → `frontend/dist/`
2. in `build/`: `cmake .. -DCMAKE_BUILD_TYPE=Release|Debug [-DANYAR_EMBED_FRONTEND=ON]` (`--clean` wipes `build/` first)
3. `make -j<cores>`
4. If `--package`: copy binary + assets, run packaging step

## Testing
No dedicated CLI unit tests — exercised via integration on the example projects in CI. Smoke-test by running `anyar init` against a temp dir and verifying it builds.

## Modifying Templates
Update `templates.cpp` `gen_main_cpp()` etc. Keep the generated shutdown guidance (main.cpp lifecycle comment + `gen_agent_instructions`) consistent with docs/graceful-shutdown.md. After change, regenerate an example to verify output compiles:
```bash
cd /tmp && rm -rf cli-smoke && LIBANYAR_DIR=<repo> anyar init cli-smoke -t svelte-ts && cd cli-smoke && anyar build
# init takes a NAME (created under cwd), not a path
```
