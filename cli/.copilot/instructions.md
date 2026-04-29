# cli/ — `anyar` CLI Tool

## Purpose
C++ CLI binary that scaffolds, runs, builds, and packages LibAnyar projects.

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
    ├── templates.cpp   # gen_main_cpp / gen_cmakelists / gen_package_json
    └── util.cpp        # exe-path finder (readlink /proc/self/exe — guarded)
```

## Commands
```bash
anyar init <name>                   # interactive: framework + template
anyar dev                           # vite HMR + C++ backend
anyar build [--embed] [--package deb|appimage|all]
```

## Templates
- **svelte-ts** (preferred), **react-ts**, **vanilla**
- All include Tailwind CSS 4 (Svelte/React) or inline styles (vanilla)
- Dark theme via CSS custom properties

## Generated Project Skeleton
```
my-app/
├── CMakeLists.txt
├── src-cpp/main.cpp     # uses app.http_get / app.allow_file_access / app.on_ready
├── frontend/            # Vite project
└── README.md
```

## Conventions
- C++ naming: `snake_case` funcs, `PascalCase` classes
- Generated files emitted via `templates.cpp` helpers
- **Avoid nested raw-string literals**: use `"\"key\":\"value\""` inside an outer `R"(...)"` to prevent parser errors
- Exe-path discovery is platform-guarded (`#ifdef __linux__` for `/proc/self/exe`); add Win32 (`GetModuleFileNameW`) / macOS (`_NSGetExecutablePath`) branches before porting

## Build Flags
- `--embed` → passes `-DANYAR_EMBED_FRONTEND=ON` to CMake (cmrc compiles `dist/` into binary)
- `--package deb` → builds DEB with auto-detected deps via `ldd` → Debian package map
- `--package appimage` → downloads `linuxdeploy`, creates AppDir, bundles libs
- Output goes under `build/dist/`

## Dev Mode (`anyar dev`)
1. Spawn `vite` in `frontend/` (HMR on dev port, e.g., 5173)
2. Spawn C++ backend; backend reads `__LIBANYAR_DEV__` to navigate webview to vite URL instead of bundled assets
3. Forward stdout/stderr; on either child exit, kill the other

## Build Mode (`anyar build`)
1. `cd frontend && npm run build` → emits `dist/`
2. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release [-DANYAR_EMBED_FRONTEND=ON]`
3. `cmake --build build -j`
4. If `--package`: copy binary + assets, run packaging step

## Testing
No dedicated CLI unit tests — exercised via integration on the example projects in CI. Smoke-test by running `anyar init` against a temp dir and verifying it builds.

## Modifying Templates
Update `templates.cpp` `gen_main_cpp()` etc. After change, regenerate an example to verify output compiles:
```bash
rm -rf /tmp/cli-smoke && anyar init /tmp/cli-smoke && cd /tmp/cli-smoke && anyar build
```
