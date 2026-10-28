# Frontend reference (`@libanyar/api`)

In a project, Vite aliases `@libanyar/api` to `<libanyar>/js-bridge/src` (the `anyar init` templates set this up). The same API works in the native webview (direct binding, ~0.01 ms) and in a normal browser pointed at the app's port (HTTP + WebSocket fallback), so the UI can be developed with `anyar dev` / Vite HMR.

## Core

```ts
import { invoke, listen, listenGlobal, once, emit, emitTo, onReady, isNativeIpc } from '@libanyar/api';

const res = await invoke<Result>('notes:get', { id: 3 });  // rejects with the C++ error message
const off = listen<Progress>('job:progress', (p) => update(p)); off();   // always unsubscribe
await emit('ui:ready', { at: Date.now() });                // JS → backend/other windows
```

Wrap commands in one typed module instead of scattering string names:

```ts
// src/api.ts
import { invoke } from '@libanyar/api';
export const getNote = (id: number) => invoke<Note>('notes:get', { id });
```

## Modules

| Import | Contents |
|---|---|
| `@libanyar/api` (root) | `invoke`, events, windows (`createWindow, closeWindow, closeAll, listWindows, setTitle, setSize, focusWindow, setEnabled, setAlwaysOnTop, getLabel, onWindowClosed/Created/Focused, setCloseConfirmation`), namespaces `dialog`, `buffer`, `canvas`, `pinhole`, `debug` |
| `@libanyar/api/fs` | `readFile(path, {encoding:'utf-8'|'base64'})`, `writeFile`, `readDir`, `exists`, `mkdir`, `remove`, `metadata` |
| `@libanyar/api/dialog` | `open`, `save`, `message`, `ask`, `confirm` (native dialogs) |
| `@libanyar/api/shell` | `openUrl`, `openPath`, `execute(program, args, {cwd, env})` |
| `@libanyar/api/db` | `openDatabase`, `query`, `exec`, `batch`, `closeDatabase` (SQLite/PostgreSQL via the C++ pool) |
| `@libanyar/api/buffer` | `fetchBuffer`, `onBufferReady`, pools (see `binary-data-and-rendering.md`) |
| `@libanyar/api/canvas` | `createFrameRenderer`, `createBufferRenderer` (WebGL for RGBA/RGB/BGRA/gray/YUV420/NV12/NV21) |
| `@libanyar/api/pinhole` | helpers for native overlays |
| `@libanyar/api/debug` | `info, screenshot, click, moveTo, mouse, scroll, key, typeText, exit` (only when automation is on) |
| `@libanyar/api/react` | `useInvoke(cmd, args)`, `useEvent(event)`, `useEventCallback(event, fn)` |

## Multi-window

```ts
import { createWindow, emitTo, closeWindow, getLabel } from '@libanyar/api';
await createWindow({ label: 'editor', url: '/#/edit/42', title: 'Edit', width: 600, height: 500,
                     parent: 'main', modal: true });
// in the child, hand the result back, then close:
await emitTo('main', 'editor:saved', { id: 42 });
await closeWindow();
```

Hash routes (`/#/route`) work with the single built `index.html`.

## UI conventions that age well

- Keep the backend authoritative. The UI renders state it got from commands and events; it doesn't keep a second copy of truth.
- Show progress through events, and keep the UI responsive while a command is in flight (the backend never blocks the UI).
- Big binary data arrives as buffers (`fetchBuffer`), not as base64 strings.
- The production build runs inside the webview, not Chrome. Test there (`ANYAR_CONSOLE=1`, test scripts) before assuming a CSS or WebGL feature works.
