# Dialog Guide

> LibAnyar provides native OS dialog commands modeled after the [Tauri v2 dialog plugin](https://v2.tauri.app/plugin/dialog/) API. All dialogs run on the GTK main thread and block the calling fiber until the user responds.

## Overview

The dialog module provides five functions:

| Function | Purpose | Returns |
|----------|---------|---------|
| `message()` | Flexible message box with configurable buttons | `MessageDialogResult` (`"Ok"`, `"Cancel"`, `"Yes"`, `"No"`) |
| `ask()` | Yes/No question dialog | `boolean` (`true` = Yes) |
| `confirm()` | Ok/Cancel confirmation dialog | `boolean` (`true` = Ok) |
| `open()` | Native file-open dialog | `string[]` or `null` |
| `save()` | Native file-save dialog | `string` or `null` |

## Import

```typescript
import { dialog } from '@libanyar/api';

// Or import functions individually:
// import { dialog } from '@libanyar/api';
// const { message, ask, confirm, open, save } = dialog;
```

All functions are also available on `window.__anyar__.dialog` for non-module scripts.

---

## Message Dialogs

### `message(msg, options?)`

Show a native message dialog with configurable buttons.

```typescript
async function message(
  msg: string,
  options?: string | MessageDialogOptions,
): Promise<MessageDialogResult>
```

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `msg` | `string` | The message text to display |
| `options` | `string \| MessageDialogOptions` | Title string, or options object |

**MessageDialogOptions:**

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `title` | `string` | `"Message"` | Dialog title bar text |
| `kind` | `"info" \| "warning" \| "error"` | `"info"` | Affects the icon shown |
| `buttons` | `MessageDialogButtons` | `"Ok"` | Button configuration (see below) |

**Returns:** `MessageDialogResult` — the button that was clicked: `"Ok"`, `"Cancel"`, `"Yes"`, or `"No"`.

#### Button Configuration

The `buttons` parameter accepts either a **preset string** or a **custom label object**.

**Preset strings:**

| Preset | Buttons shown |
|--------|---------------|
| `"Ok"` | Ok |
| `"OkCancel"` | Cancel, Ok |
| `"YesNo"` | No, Yes |
| `"YesNoCancel"` | Cancel, No, Yes |

**Custom label objects:**

```typescript
// Ok only with custom label
{ ok: "Got it" }

// Ok + Cancel with custom labels
{ ok: "Save", cancel: "Discard" }

// Yes + No with custom labels
{ yes: "Allow", no: "Deny" }

// Yes + No + Cancel with custom labels
{ yes: "Save", no: "Discard", cancel: "Go Back" }
```

The shape of the object determines which buttons are shown:
- Has `yes` key → Yes/No (or Yes/No/Cancel if `cancel` is also present)
- Has `ok` key only → Ok (or Ok/Cancel if `cancel` is also present)

#### Examples

```typescript
import { dialog } from '@libanyar/api';

// Simple message (Ok button)
await dialog.message('Operation completed.');

// With title (shorthand)
await dialog.message('File not found.', 'Error');

// With title and kind
await dialog.message('File not found.', {
  title: 'Error',
  kind: 'error',
});

// Ok/Cancel
const result = await dialog.message('Overwrite existing file?', {
  title: 'Confirm',
  buttons: 'OkCancel',
});
if (result === 'Ok') { /* overwrite */ }

// Yes/No/Cancel with custom labels
const result = await dialog.message('Save changes before closing?', {
  title: 'Unsaved Changes',
  kind: 'warning',
  buttons: { yes: 'Save', no: 'Discard', cancel: 'Cancel' },
});
if (result === 'Yes') { /* save */ }
else if (result === 'No') { /* discard */ }
else { /* cancel — do nothing */ }
```

---

### `ask(msg, options?)`

Show a Yes/No question dialog. Returns `true` if the user clicked Yes.

```typescript
async function ask(
  msg: string,
  options?: string | ConfirmDialogOptions,
): Promise<boolean>
```

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `msg` | `string` | The question text |
| `options` | `string \| ConfirmDialogOptions` | Title string, or options object |

**ConfirmDialogOptions:**

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `title` | `string` | `"Question"` | Dialog title bar text |
| `kind` | `"info" \| "warning" \| "error"` | auto | Affects the icon shown |

#### Examples

```typescript
import { dialog } from '@libanyar/api';

// Simple yes/no question
const yes = await dialog.ask('Delete this entry?');
if (yes) { /* delete it */ }

// With title
const yes = await dialog.ask('Really delete?', 'Confirm Delete');

// With kind
const yes = await dialog.ask('Delete this entry permanently?', {
  title: 'Warning',
  kind: 'warning',
});
```

---

### `confirm(msg, options?)`

Show an Ok/Cancel confirmation dialog. Returns `true` if the user clicked Ok.

```typescript
async function confirm(
  msg: string,
  options?: string | ConfirmDialogOptions,
): Promise<boolean>
```

**Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `msg` | `string` | The confirmation text |
| `options` | `string \| ConfirmDialogOptions` | Title string, or options object |

**ConfirmDialogOptions:**

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `title` | `string` | `"Confirm"` | Dialog title bar text |
| `kind` | `"info" \| "warning" \| "error"` | auto | Affects the icon shown |
| `okLabel` | `string` | `"OK"` | Custom label for the Ok button |
| `cancelLabel` | `string` | `"Cancel"` | Custom label for the Cancel button |

#### Examples

```typescript
import { dialog } from '@libanyar/api';

// Simple ok/cancel
const ok = await dialog.confirm('Discard unsaved changes?');
if (ok) { /* discard */ }

// With title
const ok = await dialog.confirm('Apply settings?', 'Confirm');

// With custom button labels
const ok = await dialog.confirm('Apply these changes?', {
  title: 'Confirm',
  okLabel: 'Apply',
  cancelLabel: 'Go Back',
});
```

---

## File Dialogs

### `open(opts?)`

Show a native file-open dialog.

```typescript
async function open(opts?: OpenDialogOptions): Promise<string[] | null>
```

**OpenDialogOptions:**

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `title` | `string` | `"Open File"` | Dialog title |
| `defaultPath` | `string` | `""` | Starting directory |
| `filters` | `DialogFilter[]` | `[]` | File type filters |
| `multiple` | `boolean` | `false` | Allow selecting multiple files |
| `directory` | `boolean` | `false` | Select directories instead of files |

**DialogFilter:**

```typescript
interface DialogFilter {
  name: string;         // Display name (e.g. "Images")
  extensions: string[]; // File extensions without dots (e.g. ["png", "jpg"])
}
```

**Returns:** Array of selected file paths, or `null` if cancelled.

#### Examples

```typescript
import { dialog } from '@libanyar/api';

// Simple open
const files = await dialog.open();
if (files) {
  console.log('Selected:', files[0]);
}

// With filters
const images = await dialog.open({
  title: 'Select Image',
  filters: [
    { name: 'Images', extensions: ['png', 'jpg', 'gif', 'webp'] },
    { name: 'All Files', extensions: ['*'] },
  ],
});

// Multiple files
const docs = await dialog.open({
  title: 'Select Documents',
  multiple: true,
  filters: [{ name: 'Documents', extensions: ['pdf', 'docx', 'txt'] }],
});

// Select directory
const folder = await dialog.open({
  title: 'Select Folder',
  directory: true,
});
```

---

### `save(opts?)`

Show a native file-save dialog.

```typescript
async function save(opts?: SaveDialogOptions): Promise<string | null>
```

**SaveDialogOptions:**

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `title` | `string` | `"Save File"` | Dialog title |
| `defaultPath` | `string` | `""` | Suggested directory and/or filename |
| `filters` | `DialogFilter[]` | `[]` | File type filters |

**Returns:** Selected save path, or `null` if cancelled. Includes overwrite confirmation.

#### Examples

```typescript
import { dialog } from '@libanyar/api';

// Simple save
const path = await dialog.save();
if (path) {
  console.log('Save to:', path);
}

// With default filename and filter
const path = await dialog.save({
  title: 'Export Database',
  defaultPath: '/home/user/backup.kdbx',
  filters: [{ name: 'KeePass Files', extensions: ['kdbx'] }],
});
```

---

## TypeScript Types

All types are exported from `@libanyar/api`:

```typescript
import type {
  DialogFilter,
  OpenDialogOptions,
  SaveDialogOptions,
  MessageDialogOptions,
  MessageDialogButtons,
  MessageDialogResult,
  ConfirmDialogOptions,
} from '@libanyar/api';
```

### Type Definitions Summary

```typescript
type MessageDialogDefaultButtons = 'Ok' | 'OkCancel' | 'YesNo' | 'YesNoCancel';

// Custom button label shapes:
interface MessageDialogButtonsOk        { ok: string }
interface MessageDialogButtonsOkCancel  { ok: string; cancel: string }
interface MessageDialogButtonsYesNo     { yes: string; no: string }
interface MessageDialogButtonsYesNoCancel { yes: string; no: string; cancel: string }

type MessageDialogCustomButtons =
  | MessageDialogButtonsOk
  | MessageDialogButtonsOkCancel
  | MessageDialogButtonsYesNo
  | MessageDialogButtonsYesNoCancel;

type MessageDialogButtons = MessageDialogDefaultButtons | MessageDialogCustomButtons;
type MessageDialogResult = 'Ok' | 'Cancel' | 'Yes' | 'No';
```

---

## IPC Commands Reference

These are the raw IPC commands registered by `DialogPlugin`. Most users should use the JS bridge functions above, but this reference is useful for writing C++ plugins or debugging.

| Command | Required Args | Optional Args | Returns |
|---------|--------------|---------------|---------|
| `dialog:open` | — | `title`, `multiple`, `directory`, `defaultPath`, `filters` | `string[]` or `null` |
| `dialog:save` | — | `title`, `defaultPath`, `filters` | `string` or `null` |
| `dialog:message` | `message` | `title`, `kind`, `buttons` | `string` (`"Ok"`, `"Cancel"`, `"Yes"`, `"No"`) |
| `dialog:ask` | `message` | `title`, `kind` | `bool` (`true` = Yes) |
| `dialog:confirm` | `message` | `title`, `kind`, `okLabel`, `cancelLabel` | `bool` (`true` = Ok) |

### `buttons` parameter detail (dialog:message)

When `buttons` is a **string**: `"Ok"`, `"OkCancel"`, `"YesNo"`, or `"YesNoCancel"`.

When `buttons` is an **object**:

```json
{ "ok": "Save" }
{ "ok": "Accept", "cancel": "Decline" }
{ "yes": "Allow", "no": "Deny" }
{ "yes": "Save", "no": "Discard", "cancel": "Cancel" }
```

### `kind` parameter

Maps to GTK message types:

| Value | GTK Type | Icon |
|-------|----------|------|
| `"info"` | `GTK_MESSAGE_INFO` | Information (ℹ) |
| `"warning"` | `GTK_MESSAGE_WARNING` | Warning (⚠) |
| `"error"` | `GTK_MESSAGE_ERROR` | Error (✖) |
| _(auto)_ | `GTK_MESSAGE_QUESTION` | Question (?) — used for Yes/No buttons |

---

## Comparison with Tauri v2.4

LibAnyar's dialog API is intentionally modeled after Tauri v2.4 to make it easy for developers familiar with Tauri.

| Feature | Tauri v2.4 | LibAnyar |
|---------|-----------|----------|
| `message()` | ✅ | ✅ Same signature |
| `ask()` (Yes/No → bool) | ✅ | ✅ Same signature |
| `confirm()` (Ok/Cancel → bool) | ✅ | ✅ Same signature + custom labels |
| `open()` / `save()` | ✅ | ✅ Same options |
| Button presets | `Ok`, `OkCancel`, `YesNo`, `YesNoCancel` | ✅ Same presets |
| Custom button labels | ✅ via object | ✅ Same pattern |
| Dialog kind/icon | ✅ `info`, `warning`, `error` | ✅ Same values |
| File filters | ✅ `{ name, extensions }` | ✅ Same shape |

### Key difference

Tauri uses Rust's `rfd` crate for cross-platform dialogs. LibAnyar uses **native GTK3 dialogs** directly via `gtk_message_dialog_new()` and `gtk_file_chooser_dialog_new()`, running on the GTK main thread via `run_on_main_thread()`.

---

## C++ Backend Reference

The dialog plugin is implemented in `core/src/plugins/dialog_linux.cpp` and registered automatically by the framework. The header is at `core/include/anyar/plugins/dialog_plugin.h`.

```cpp
#include <anyar/plugins/dialog_plugin.h>

namespace anyar {

class DialogPlugin : public IAnyarPlugin {
public:
    std::string name() const override { return "dialog"; }
    void initialize(PluginContext& ctx) override;
};

} // namespace anyar
```

All dialog commands run on the GTK main thread via `run_on_main_thread()` (fiber-blocking call) to satisfy GTK's single-threaded requirement.
