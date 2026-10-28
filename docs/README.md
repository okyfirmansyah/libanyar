# LibAnyar Documentation

## Quick Start

```bash
anyar init myapp        # scaffold a new project (React, Vue, or Svelte)
cd myapp && anyar dev   # start Vite HMR + C++ backend
```

See [Getting Started](getting-started.md) for prerequisites and full setup instructions.

## Guides

- [Getting Started](getting-started.md) — Installation, prerequisites, and first build
- [Building Your First App](building-first-app.md) — Step-by-step tutorial
- [Graceful Shutdown](graceful-shutdown.md) — Brief rules for plugin teardown and background work
- [Writing Plugins](writing-plugins.md) — Create custom C++ plugins
- [Dialog Guide](dialogs.md) — Native message, file open/save, and confirmation dialogs
- [Multi-Window Guide](multi-window.md) — Creating, managing, and communicating between windows
- [Database Integration](database-integration.md) — SQLite and PostgreSQL guide
- [Shared Memory & WebGL Canvas](shared-memory-webgl.md) — Zero-copy binary IPC and WebGL rendering
- [Pinhole Native Overlay Rendering](pinhole-rendering.md) — Sub-millisecond GL / D3D overlay surfaces (Linux + Windows; macOS Phase 7.2)
- [Packaging & Distribution](packaging.md) — Create DEB packages and AppImage bundles
- [Debugging & UI Automation](debugging.md) — DevTools, console forwarding, screenshots, synthetic input, scripted UI tests in the real webview

## For AI coding assistants

- [LibAnyar skill for Claude Code](../skills/README.md) — install `skills/libanyar/` so Claude knows LibAnyar's threading model, IPC/buffer patterns, shutdown rules, debugging tools and packaging

## Reference

- [Architecture Overview](../ARCHITECTURE.md) — System architecture, IPC protocol, threading model
- [Architecture Decisions](decisions.md) — ADR-001..016
- [Roadmap](roadmap.md) — Phased plan and status
- [Progress](progress.md) — Current progress tracking
- [C++ API Reference](api/cpp/html/index.html) — Generated from Doxygen (run `doxygen Doxyfile`)
- [JS API Reference](api/js/index.html) — Generated from TypeDoc (run `npx typedoc` in `js-bridge/`)

## Examples

- [Hello World](../examples/hello-world/README.md) — Basic IPC, events, and built-in plugins
- [Video Player](../examples/video-player/README.md) — FFmpeg, WebSocket streaming, Canvas rendering
- [Key Storage](../examples/key-storage/README.md) — Encrypted password manager with custom plugin
- [Anatomy Atlas](../examples/anatomy-atlas/README.md) — Whole-body 3D anatomy, SharedBuffer LOD streaming, GPU picking (also the debugging-tools showcase)

## Contributing

- [Contributing Guide](../CONTRIBUTING.md) — Code style, workflow, testing
- [Architecture](../ARCHITECTURE.md) — System design and component details
