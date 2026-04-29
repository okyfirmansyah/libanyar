# js-bridge/ — @libanyar/api TypeScript Package

NPM package providing typed JS bridge to the C++ backend. Builds dual ESM + CJS + `.d.ts`.

## Layout
- `package.json` — `"prepare":"npm run build"` auto-builds on `npm link`
- `tsconfig.{json,esm.json,cjs.json}`, `vitest.config.ts`, `typedoc.json`
- `src/index.ts` — public re-exports
- `src/invoke.ts` — `invoke<T>()`, native primary, HTTP fallback
- `src/events.ts` — `listen/emit/once/onReady`, native primary, WS fallback
- `src/config.ts` — port, `isNativeIpc()`
- `src/react.ts` — `useInvoke / useEvent / useEventCallback`
- `src/types.ts`, `global.d.ts`
- `src/modules/`: `fs.ts dialog.ts shell.ts db.ts`; `window.ts` (`createWindow, emitTo, listenGlobal, onWindowFocused`); `buffer.ts` (`SharedBuffer + fetchBuffer` via `anyar-shm://` or HTTP); `canvas.ts` (WebGL `FrameRenderer` — RGBA/RGB/BGRA/Gray/YUV420/NV12/NV21)

## Conventions
- Named exports only — no default exports
- JSDoc `@param @returns @example` on public API
- TS strict; `tsc --noEmit` must pass
- Tests collocated as `*.test.ts`

Subpath imports: `@libanyar/api/{fs,dialog,shell,db,buffer,canvas,react}`.

## IPC Detection
- `window.__LIBANYAR_NATIVE__===true` set by C++ via `webview_init()`
- `window.__anyar_ipc__` bound via `webview_bind()` — Promise-returning
- `window.__anyar_dispatch_event__` invoked by C++ via `webview_eval`
- Fallback: `POST /__anyar__/invoke` + WS `/__anyar_ws__`
- Port from `window.__LIBANYAR_PORT__` or `<meta name="anyar-port">`

## Window Awareness & Echo
`window.__LIBANYAR_WINDOW_LABEL__` injected per-window. `listen()` skips events targeted at OTHER windows (via `EventMessage.target`); `listenGlobal()` receives all. JS-emitted events go through `invoke('anyar:emit_event', ...)` → C++ `events_.emit_local()` (C++ subscribers only — no echo back to JS).

## Commands
```bash
npm install && npm run build      # tsc → dist/{esm,cjs}/
npm run typecheck                  # tsc --noEmit
npm test                           # vitest run (jsdom)
npm run docs                       # typedoc → docs/api/js/
```

## Testing
Vitest + jsdom; mock `window.__anyar_ipc__` and `fetch`. 123+ tests across config, invoke, events, fs, dialog, shell, db, buffer, window, react.

## Adding a Module
1. `src/modules/my_mod.ts` — typed wrappers over `invoke()`
2. `src/modules/my_mod.test.ts` — Vitest with mocked transport
3. Add subpath export in `package.json` `"exports"`; re-export from `src/index.ts`
4. `npm run build && npm test`

## Vite Alias for Examples
Examples consume source directly via `resolve.alias`: `'@libanyar/api': path.resolve('../../js-bridge/src')`. No `npm install` needed for in-tree dev.
