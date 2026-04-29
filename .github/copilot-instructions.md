# LibAnyar — Project Context

Tauri-class C++17 desktop framework. Native OS webview (WebKitGTK / WebView2 / WKWebView) hosts a web frontend. C++ backend uses [LibAsyik](https://github.com/okyfirmansyah/libasyik) (fibers, HTTP/WS, SOCI/SQL).

## Architecture
Frontend (Vite SPA, `dist/`) → `@libanyar/api` → OS WebView → C++ Core (`anyar::App`, IPC router, command registry, event bus, window mgr, SharedBuffer) → LibAsyik.

## Stack
C++17 (GCC 11+/Clang 10+/MSVC 2019+), CMake ≥ 3.16. Deps: LibAsyik 1.6.1+, Boost 1.81+, OpenSSL, nlohmann/json 3.11+, nativefiledialog-extended. Frontend: TS + Vite + React/Vue/Svelte 5 + Tailwind 4. Tests: Catch2 + Vitest. CI: CircleCI Ubuntu 22.04.

## Conventions
- C++ `snake_case` funcs/vars, `PascalCase` classes, namespace `anyar::`
- `#pragma once`, include `<anyar/header.h>`
- `using json = nlohmann::json;`; smart pointers only — no raw `new`/`delete`
- Doxygen `///` on every public C++ API
- TS named exports only, JSDoc `@param @returns @example`, strict mode
- IPC errors as structured JSON; exceptions only for unrecoverable
- Conventional Commits: `feat: fix: docs: test: refactor: chore:`

## Commands
- Build: `cmake -B build -DANYAR_BUILD_TESTS=ON && cmake --build build -j && ctest --test-dir build --output-on-failure`
- JS: `cd js-bridge && npm i && npm run build && npm test && npm run typecheck`
- Run: `./run.sh examples/hello-world/hello_world` (clears snap GTK env)
- CLI: `anyar init|dev|build [--embed] [--package deb|appimage|all]`

## Repo Map
`core/` C++ lib · `js-bridge/` `@libanyar/api` · `cli/` `anyar` · `tests/` Catch2+WebGL · `examples/` (hello-world, key-storage, video-player, wifi-analyzer) · `docs/` ADRs+roadmap.

## Per-Module Context
Each module has `<module>/CLAUDE.md` (`#import .copilot/instructions.md`). Modules: `core/`, `js-bridge/`, `cli/`, `tests/`, `examples/`.

## References
[ARCHITECTURE.md](../ARCHITECTURE.md) · [CONTRIBUTING.md](../CONTRIBUTING.md) · [docs/decisions.md](../docs/decisions.md) (ADR-001..007) · [docs/roadmap.md](../docs/roadmap.md)
