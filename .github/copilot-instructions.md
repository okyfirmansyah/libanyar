# LibAnyar — Project Context

Tauri-class C++17 desktop framework. Native OS webview (WebKitGTK / WebView2 / WKWebView) hosts a web frontend. C++ backend uses [LibAsyik](https://github.com/okyfirmansyah/libasyik) (fibers, HTTP/WS, SOCI/SQL).

## Architecture
Frontend (Vite SPA, `dist/`) → `@libanyar/api` → OS WebView → C++ Core (`anyar::App`, IPC router, command registry, event bus, window mgr, SharedBuffer, Pinhole native overlay) → LibAsyik.

## Stack
C++17 (GCC 11+/Clang 10+/MSVC 2019+), CMake ≥ 3.16. Deps: LibAsyik 1.7.1+ (1.6.x hangs shutdown with open keep-alive connections), Boost 1.81+, OpenSSL, nlohmann/json 3.11+, nativefiledialog-extended. Frontend: TS + Vite + React/Vue/Svelte 5 + Tailwind 4. Tests: Catch2 + Vitest. CI: CircleCI Ubuntu 22.04.

## Conventions
- C++ `snake_case` funcs/vars, `PascalCase` classes, namespace `anyar::`
- `#pragma once`, include `<anyar/header.h>`
- `using json = nlohmann::json;`; smart pointers only — no raw `new`/`delete`
- Doxygen `///` on every public C++ API
- TS named exports only, JSDoc `@param @returns @example`, strict mode
- IPC errors as structured JSON; exceptions only for unrecoverable
- One service thread runs every command/route/event: blocking work → `anyar::run_blocking()`, long-lived loops → `anyar::BackgroundTask` (stop+join in `shutdown()`), cross-thread frames → `anyar::FrameMailbox` (ADR-009)
- Conventional Commits: `feat: fix: docs: test: refactor: chore:`

## Commands
- Build: `cmake -B build -DANYAR_BUILD_TESTS=ON && cmake --build build -j && ctest --test-dir build --output-on-failure`
- JS: `cd js-bridge && npm i && npm run build && npm test && npm run typecheck`
- Run: `./run.sh examples/hello-world/hello_world` (clears snap GTK env)
- CLI: `anyar init|dev|build [--embed] [--package deb|appimage|all]`

## Repo Map
`core/` C++ lib · `js-bridge/` `@libanyar/api` · `cli/` `anyar` · `tests/` Catch2+WebGL · `examples/` (hello-world, key-storage, video-player, wifi-analyzer, pinhole-hello) · `benchmarks/` perf harness · `docs/` ADRs+roadmap.

## Per-Module Context
Each module has `<module>/.copilot/instructions.md`; `<module>/CLAUDE.md` imports it via `@.copilot/instructions.md` (Claude Code import syntax — never `#import`). Modules: `core/`, `js-bridge/`, `cli/`, `tests/`, `examples/`.

## References
[ARCHITECTURE.md](../ARCHITECTURE.md) · [CONTRIBUTING.md](../CONTRIBUTING.md) · [docs/decisions.md](../docs/decisions.md) (ADR-001..009) · [docs/roadmap.md](../docs/roadmap.md)

## Status & Docs Hygiene (MUST)
- Current state, next priorities, open risks: [docs/progress.md](../docs/progress.md) — read before planning work.
- After finishing a task that changes status, adds/removes an API, or finds a risk: update `docs/progress.md` (priorities, risks, session log), tick the checkbox in `docs/roadmap.md`, and fix the affected module `.copilot/instructions.md` (layout lists, APIs). Stale context is worse than none.
- Architectural decisions → new ADR in `docs/decisions.md`.
