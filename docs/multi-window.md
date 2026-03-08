# Multi-Window Guide

> LibAnyar supports multiple native windows — top-level, transient child, and modal dialogs. This guide covers creating, managing, and communicating between windows.

## Overview

LibAnyar's multi-window system mirrors Tauri's label-based approach, with the addition of **native modal dialogs** (a feature Tauri lacks). Each window:

- Has a unique **string label** (e.g. `"main"`, `"settings"`, `"entry-42"`)
- Loads content from the **same** local HTTP server (different URL paths)
- Gets its own **native IPC bridge** (`__anyar_ipc__`) bound per webview
- Can send/receive **events** to/from other windows

## Creating Windows

### From JavaScript

```typescript
import { createWindow, closeWindow, closeAll } from '@libanyar/api';

// Simple top-level window
await createWindow({
  label: 'settings',
  title: 'Settings',
  url: '/#/settings',
  width: 600,
  height: 400,
});

// Modal child window (blocks parent until closed)
await createWindow({
  label: 'confirm-delete',
  title: 'Confirm Delete',
  url: '/#/confirm',
  parent: 'main',        // parent window label
  modal: true,            // native GTK modal — blocks parent input
  width: 400,
  height: 250,
  resizable: false,
  center: true,
});

// Close a specific window
await closeWindow('settings');

// Close current window (from inside a child window)
await closeWindow();      // closes the calling window

// Close all windows (exits the app)
await closeAll();
```

### From C++

```cpp
#include <anyar/app.h>

// Inside your app setup or command handler:
WindowCreateOptions opts;
opts.label = "settings";
opts.title = "Settings";
opts.url = "/#/settings";
opts.width = 600;
opts.height = 400;
opts.parent = "main";   // optional
opts.modal = true;       // optional

app.create_window(opts);
```

## WindowOptions Reference

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `label` | `string` | _(required)_ | Unique window identifier |
| `title` | `string` | `"LibAnyar"` | Window title bar text |
| `width` | `number` | `800` | Width in pixels |
| `height` | `number` | `600` | Height in pixels |
| `url` | `string` | `"/"` | URL path to load |
| `parent` | `string` | `""` | Parent window label (empty = top-level) |
| `modal` | `boolean` | `false` | Block parent until this window closes |
| `resizable` | `boolean` | `true` | Allow user resize |
| `center` | `boolean` | `true` | Center on screen (or on parent) |
| `alwaysOnTop` | `boolean` | `false` | Keep above other windows |
| `decorations` | `boolean` | `true` | Show title bar and borders |
| `closable` | `boolean` | `true` | Allow user to close via title bar X |
| `minimizable` | `boolean` | `true` | Allow minimize |

## Window Types

### Top-Level Windows
No `parent` set — independent windows at the OS level.

```typescript
await createWindow({
  label: 'about',
  title: 'About',
  url: '/#/about',
});
```

### Child (Transient) Windows
Set `parent` to keep the child above its parent and minimize/restore together.

```typescript
await createWindow({
  label: 'preferences',
  parent: 'main',
  url: '/#/preferences',
});
```

### Modal Windows
Set `parent` + `modal: true` to block all interaction with the parent window until the modal is closed. LibAnyar uses **native GTK modal** (`gtk_window_set_modal`) — this is more robust than Tauri's approach (which fakes modality via `set_enabled`).

```typescript
await createWindow({
  label: 'entry-detail',
  parent: 'main',
  modal: true,
  url: `/#/entry/${entryId}`,
  width: 550,
  height: 700,
});
```

## Frontend Routing for Child Windows

Child windows load different content than the main window. Two approaches are supported:

### Approach A: Hash Routing (Recommended for SPAs)

Each window navigates to the same `index.html` but with a different hash route. Your frontend router renders the appropriate component.

**How it works:**
1. Main window loads `http://127.0.0.1:<port>/` → hash = `#/` or empty
2. Child window loads `http://127.0.0.1:<port>/#/entry/42` → hash = `#/entry/42`
3. Both share the same `index.html`, Svelte/React/Vue app, and JS bundle
4. A simple hash router in `main.js` dispatches to the right root component

**Example (Svelte 5, no external router library needed):**

```javascript
// main.js
import App from './App.svelte';
import EntryDetailPage from './EntryDetailPage.svelte';
import { mount } from 'svelte';

function getRoute() {
  const hash = window.location.hash || '';
  const entryMatch = hash.match(/^#\/entry\/(\d+)$/);
  if (entryMatch) {
    return { page: 'entry-detail', entryId: parseInt(entryMatch[1], 10) };
  }
  return { page: 'main' };
}

const route = getRoute();

if (route.page === 'entry-detail') {
  mount(EntryDetailPage, {
    target: document.getElementById('app'),
    props: { entryId: route.entryId },
  });
} else {
  mount(App, {
    target: document.getElementById('app'),
  });
}
```

**Pros:**
- Single build output — no extra Vite config
- Shares CSS, JS bundle, and the `@libanyar/api` bridge
- Works with any SPA framework

**Cons:**
- All pages share the same JS bundle size (not a problem for most desktop apps)

### Approach B: Separate HTML Entry (Multi-Page)

Use Vite's multi-page mode to create separate HTML entry points with independent JS bundles.

**Vite config:**
```javascript
// vite.config.js
import { defineConfig } from 'vite';

export default defineConfig({
  build: {
    rollupOptions: {
      input: {
        main: 'index.html',
        popup: 'popup.html',
      },
    },
  },
});
```

**Create window:**
```typescript
await createWindow({
  label: 'popup',
  url: '/popup.html',
  width: 400,
  height: 300,
});
```

**Pros:**
- Minimal JS bundle for small popups
- Fully isolated — different frameworks per page if needed

**Cons:**
- More build configuration
- Separate HTML files to maintain
- No shared state via JS imports (must use IPC events)

### Recommendation

**Use Approach A (hash routing)** for most apps. It's simpler, requires no build config changes, and works naturally with SPA frameworks. Use Approach B only when you need a dramatically smaller bundle for a popup window.

## Cross-Window Communication

Windows communicate via LibAnyar's event system. Events are broadcast to all windows by default.

### Sending Events

```typescript
import { emit } from '@libanyar/api';

// Emit from child window after saving
emit('entry:updated', { id: 42 });

// Emit from child window after deleting
emit('entry:deleted', { id: 42 });
```

### Listening for Events

```typescript
import { listen } from '@libanyar/api';

// In main window — react to child window actions
listen('entry:updated', (msg) => {
  console.log('Entry updated:', msg.payload.id);
  refreshEntryList();
});

listen('entry:deleted', (msg) => {
  console.log('Entry deleted:', msg.payload.id);
  refreshEntryList();
});
```

### Window Lifecycle Events

These are emitted automatically by the framework:

```typescript
import { onWindowClosed, onWindowCreated } from '@libanyar/api';

onWindowCreated((msg) => {
  console.log('New window:', msg.payload.label);
});

onWindowClosed((msg) => {
  console.log('Window closed:', msg.payload.label);
});
```

## Window Management API

```typescript
import {
  listWindows,
  setTitle,
  setSize,
  focusWindow,
  setEnabled,
  setAlwaysOnTop,
  getLabel,
} from '@libanyar/api';

// Get current window's label
const myLabel = getLabel();  // sync — returns e.g. "main" or "entry-42"

// List all open windows
const windows = await listWindows();
// → [{ label: "main" }, { label: "entry-42" }]

// Change title of any window
await setTitle('main', 'My App — Edited');

// Resize a window
await setSize('settings', 800, 600);

// Bring a window to front
await focusWindow('main');

// Enable/disable window input (pseudo-modal pattern)
await setEnabled('main', false);  // disable
await setEnabled('main', true);   // re-enable

// Toggle always-on-top
await setAlwaysOnTop('popup', true);
```

## Browser/Dev Mode Fallback

When running in browser mode (`anyar dev` or `vite dev`), native windows are unavailable because there's no webview host. Use `isNativeIpc()` to detect the environment and fall back to CSS overlay modals:

```typescript
import { isNativeIpc } from '@libanyar/api';
import { createWindow } from '@libanyar/api';

async function openEntryDetail(id) {
  if (isNativeIpc()) {
    // Native mode — real GTK modal window
    await createWindow({
      label: `entry-${id}`,
      parent: 'main',
      modal: true,
      url: `/#/entry/${id}`,
      width: 550,
      height: 700,
    });
  } else {
    // Browser fallback — CSS overlay modal
    showOverlayModal(id);
  }
}
```

The key-storage example demonstrates this pattern — see `App.svelte`'s `handleOpenEntry()` function.

## Complete Example: Key-Storage Native Modal

The `examples/key-storage/` app demonstrates the full multi-window pattern:

1. **Main window** (`App.svelte`) — shows the vault with groups and entry list
2. **Entry detail** (`EntryDetailPage.svelte`) — standalone page for `/#/entry/:id`
3. **Hash router** (`main.js`) — routes to the right component based on URL hash
4. **Cross-window events** — child emits `entry:updated`/`entry:deleted`, main listens and refreshes

### Flow: Opening an Entry

```
User double-clicks entry in main window
  → handleOpenEntry(id)
  → isNativeIpc() ? createWindow({...}) : showCssOverlay()
  → C++ creates GTK child window (modal, transient to main)
  → Child loads http://127.0.0.1:<port>/#/entry/42
  → EntryDetailPage mounts, loads entry via invoke('ks:get_entry')
  → User edits fields → auto-save via invoke('ks:update_entry')
  → emit('entry:updated', { id }) → main window refreshes list
  → User closes child window → GTK destroys it
  → window:closed event broadcast → main window cleans up
```

## Architecture Details

### How Multiple Windows Share One Server

All windows connect to the same LibAsyik HTTP server on `127.0.0.1:<port>`. The C++ backend runs on a background thread with Boost fibers; the GTK main loop on the main thread serves all windows.

```
Main Thread (GTK)           Background Thread (LibAsyik)
┌──────────────────┐        ┌──────────────────────────┐
│  GtkWindow "main"│───────▶│  HTTP Server :port       │
│  GtkWindow "child"│──────▶│  ┌─ /__anyar__/invoke   │
│  g_main_loop     │        │  ├─ /__anyar_ws__       │
│  g_idle_add()    │        │  └─ /* (static files)   │
└──────────────────┘        │  CommandRegistry         │
                            │  EventBus (WS sinks)     │
                            └──────────────────────────┘
```

### Per-Window IPC

Each window gets its own `__anyar_ipc__` binding via `webview_bind()`. The C++ side injects `_caller_label` into every IPC request so command handlers know which window made the call.

### Native Modal vs Pseudo-Modal

| Feature | Native Modal (`modal: true`) | Pseudo-Modal (`setEnabled`) |
|---------|------------------------------|------------------------------|
| Implementation | `gtk_window_set_modal()` | `gtk_widget_set_sensitive()` |
| Parent blocked | Yes (OS-level) | Yes (input disabled) |
| Modal overlay | OS provides dimming | Must dim manually via CSS |
| Close behavior | OS handles focus return | Must re-enable parent manually |
| Keyboard (Alt+F4) | Works naturally | Must handle explicitly |

**Recommendation**: Use native modal for confirmations and detail editors. Use pseudo-modal only when you need custom dimming animations.
