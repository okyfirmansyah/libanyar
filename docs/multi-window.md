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
| `minWidth` | `number` | `0` | Minimum width in pixels (0 = no minimum) |
| `minHeight` | `number` | `0` | Minimum height in pixels (0 = no minimum) |

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
Set `parent` + `modal: true` to block all interaction with the parent window until the modal is closed. LibAnyar disables the parent window's input via `gtk_widget_set_sensitive(parent, FALSE)` and positions the child above the parent using `GDK_WINDOW_TYPE_HINT_DIALOG` + `gtk_window_set_keep_above`. When the modal child closes, the parent is automatically re-enabled.

```typescript
await createWindow({
  label: 'entry-detail',
  parent: 'main',
  modal: true,
  url: `/#/entry/${entryId}`,
  width: 550,
  height: 700,
  minWidth: 450,
  minHeight: 500,
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

## Close Confirmation

You can show a native confirmation dialog when the user attempts to close a window (via the X button or Alt+F4). This is useful for preventing accidental data loss.

```typescript
import { setCloseConfirmation } from '@libanyar/api';

// Enable close confirmation on the current window
await setCloseConfirmation({
  enabled: true,
  message: 'You have unsaved changes.\nClose anyway?',
  title: 'Confirm Close',
});

// Disable close confirmation
await setCloseConfirmation({ enabled: false });

// Enable on a specific window
await setCloseConfirmation({
  label: 'editor',
  enabled: true,
  message: 'Discard changes?',
});
```

**CloseConfirmationOptions:**

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `label` | `string` | current window | Target window label |
| `enabled` | `boolean` | _(required)_ | Enable or disable the confirmation |
| `message` | `string` | `"You have unsaved changes.\nClose anyway?"` | Message shown in the dialog |
| `title` | `string` | `"Confirm Close"` | Dialog title |

When enabled, the GTK `delete-event` handler shows a native Ok/Cancel dialog. If the user clicks Cancel, the close is prevented.

### Reactive Close Confirmation (Svelte 5 Example)

Use a reactive effect to auto-enable/disable based on app state:

```typescript
import { setCloseConfirmation } from '@libanyar/api';

// Enable only when there are unsaved changes
$effect(() => {
  setCloseConfirmation({
    enabled: hasUnsavedChanges,
    message: 'You have unsaved changes.\nClose anyway?',
    title: 'Unsaved Changes',
  });
});
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
  → User edits fields, clicks Save → invoke('ks:update_entry')
  → emit('entry:updated', { id }) → main window refreshes list
  → Or clicks Cancel → closeWindow() discards changes
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

### Modal Implementation Details

When `modal: true` is set on a child window, LibAnyar applies these native GTK operations:

1. **Parent disabled**: `gtk_widget_set_sensitive(parent_window, FALSE)` — grays out and blocks all input
2. **Child type hint**: `gdk_window_set_type_hint(child, GDK_WINDOW_TYPE_HINT_DIALOG)` — OS treats it as a dialog
3. **Child stays on top**: `gtk_window_set_keep_above(child, TRUE)` — ensures visibility over parent
4. **Auto-restore on close**: When the child window closes, the parent is automatically re-enabled via `gtk_widget_set_sensitive(parent, TRUE)`

You can also use `setEnabled()` directly for manual pseudo-modal patterns:

```typescript
// Manual pseudo-modal (not recommended — use modal: true instead)
await setEnabled('main', false);  // disable parent
await createWindow({ label: 'popup', ... });
onWindowClosed((e) => {
  if (e.payload.label === 'popup') setEnabled('main', true);
});
```

**Recommendation**: Use `modal: true` in `createWindow()` for all modal scenarios. The manual `setEnabled()` approach is available but not needed in most cases.
