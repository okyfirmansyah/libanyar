# LibAnyar Documentation

## Guides

- [Getting Started](getting-started.md) — Installation, prerequisites, and first build
- [Building Your First App](building-first-app.md) — Step-by-step tutorial
- [Writing Plugins](writing-plugins.md) — Create custom C++ plugins
- [Dialog Guide](dialogs.md) — Native message, file open/save, and confirmation dialogs
- [Multi-Window Guide](multi-window.md) — Creating, managing, and communicating between windows
- [Database Integration](database-integration.md) — SQLite and PostgreSQL guide
- [Shared Memory & WebGL Canvas](shared-memory-webgl.md) — Zero-copy binary IPC and WebGL rendering
- [Packaging & Distribution](packaging.md) — Create DEB packages and AppImage bundles

## Reference

- [Architecture Overview](../ARCHITECTURE.md) — System architecture, IPC protocol, threading model
- [C++ API Reference](api/cpp/html/index.html) — Generated from Doxygen (run `doxygen Doxyfile`)
- [JS API Reference](api/js/index.html) — Generated from TypeDoc (run `npx typedoc` in `js-bridge/`)

## Examples

- [Hello World](../examples/hello-world/README.md) — Basic IPC, events, and built-in plugins
- [Video Player](../examples/video-player/README.md) — FFmpeg, WebSocket streaming, Canvas rendering
- [Key Storage](../examples/key-storage/README.md) — Encrypted password manager with custom plugin

## Contributing

- [Contributing Guide](../CONTRIBUTING.md) — Code style, workflow, testing
- [Development Roadmap](../PLAN.md) — Project phases and status
