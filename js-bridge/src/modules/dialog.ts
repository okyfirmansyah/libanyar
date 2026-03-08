// ---------------------------------------------------------------------------
// @libanyar/api/dialog — Native dialog commands
//
// Modeled after the Tauri v2.4 dialog plugin API.
// ---------------------------------------------------------------------------

import { invoke } from '../invoke';

// ── File dialog types ──────────────────────────────────────────────────────

/** Filter for file dialogs (e.g. { name: "Images", extensions: ["png","jpg"] }). */
export interface DialogFilter {
  name: string;
  extensions: string[];
}

/** Options for the open-file dialog. */
export interface OpenDialogOptions {
  title?: string;
  defaultPath?: string;
  filters?: DialogFilter[];
  multiple?: boolean;
  directory?: boolean;
}

/** Options for the save-file dialog. */
export interface SaveDialogOptions {
  title?: string;
  defaultPath?: string;
  filters?: DialogFilter[];
}

// ── Message dialog types ───────────────────────────────────────────────────

/** Preset button sets (matches Tauri's MessageDialogDefaultButtons). */
export type MessageDialogDefaultButtons = 'Ok' | 'OkCancel' | 'YesNo' | 'YesNoCancel';

/** Custom button labels — Ok only. */
export interface MessageDialogButtonsOk {
  ok: string;
}

/** Custom button labels — Ok + Cancel. */
export interface MessageDialogButtonsOkCancel {
  ok: string;
  cancel: string;
}

/** Custom button labels — Yes + No. */
export interface MessageDialogButtonsYesNo {
  yes: string;
  no: string;
}

/** Custom button labels — Yes + No + Cancel. */
export interface MessageDialogButtonsYesNoCancel {
  yes: string;
  no: string;
  cancel: string;
}

/** All possible custom button shapes. */
export type MessageDialogCustomButtons =
  | MessageDialogButtonsOk
  | MessageDialogButtonsOkCancel
  | MessageDialogButtonsYesNo
  | MessageDialogButtonsYesNoCancel;

/** Buttons parameter: preset string or custom label object. */
export type MessageDialogButtons = MessageDialogDefaultButtons | MessageDialogCustomButtons;

/** Result returned by the message dialog. */
export type MessageDialogResult = 'Ok' | 'Cancel' | 'Yes' | 'No';

/** Options for the flexible message dialog. */
export interface MessageDialogOptions {
  title?: string;
  /** Dialog type affects the icon shown. */
  kind?: 'info' | 'warning' | 'error';
  /**
   * Button configuration.
   * - Preset: `"Ok"`, `"OkCancel"`, `"YesNo"`, `"YesNoCancel"`
   * - Custom: `{ ok: "Save" }`, `{ ok: "Accept", cancel: "Decline" }`,
   *           `{ yes: "Allow", no: "Deny", cancel: "Skip" }`
   */
  buttons?: MessageDialogButtons;
}

/** Options for the confirm dialog (Ok/Cancel). */
export interface ConfirmDialogOptions {
  title?: string;
  kind?: 'info' | 'warning' | 'error';
  /** Custom label for the Ok button. */
  okLabel?: string;
  /** Custom label for the Cancel button. */
  cancelLabel?: string;
}

// ── File dialogs ───────────────────────────────────────────────────────────

/**
 * Show a native file-open dialog.
 *
 * @returns Selected file path(s), or `null` if cancelled.
 */
export async function open(
  opts: OpenDialogOptions = {},
): Promise<string[] | null> {
  return invoke<string[] | null>('dialog:open', opts);
}

/**
 * Show a native file-save dialog.
 *
 * @returns Selected save path, or `null` if cancelled.
 */
export async function save(
  opts: SaveDialogOptions = {},
): Promise<string | null> {
  return invoke<string | null>('dialog:save', opts);
}

// ── Message dialogs ────────────────────────────────────────────────────────

/**
 * Show a native message dialog with configurable buttons.
 *
 * @param msg     The message text to display.
 * @param options Optional title, kind, and buttons configuration.
 * @returns       The button that was clicked: `"Ok"`, `"Cancel"`, `"Yes"`, or `"No"`.
 *
 * @example
 * ```ts
 * // Simple info message
 * await message('Operation complete');
 *
 * // Warning with Ok/Cancel
 * const result = await message('Discard changes?', {
 *   kind: 'warning',
 *   buttons: 'OkCancel',
 * });
 *
 * // Custom button labels
 * const result = await message('Save before closing?', {
 *   buttons: { yes: 'Save', no: 'Discard', cancel: 'Cancel' },
 * });
 * ```
 */
export async function message(
  msg: string,
  options?: string | MessageDialogOptions,
): Promise<MessageDialogResult> {
  const opts: Record<string, unknown> =
    typeof options === 'string'
      ? { title: options, message: msg }
      : { ...(options ?? {}), message: msg };
  return invoke<MessageDialogResult>('dialog:message', opts);
}

/**
 * Show a Yes/No question dialog.
 *
 * @param msg     The question text.
 * @param options Optional title and kind. If a string, used as title.
 * @returns       `true` if the user clicked Yes, `false` otherwise.
 *
 * @example
 * ```ts
 * const yes = await ask('Delete this entry?', { kind: 'warning' });
 * ```
 */
export async function ask(
  msg: string,
  options?: string | ConfirmDialogOptions,
): Promise<boolean> {
  const opts: Record<string, unknown> =
    typeof options === 'string'
      ? { title: options, message: msg }
      : { ...(options ?? {}), message: msg };
  return invoke<boolean>('dialog:ask', opts);
}

/**
 * Show an Ok/Cancel confirmation dialog.
 *
 * @param msg     The confirmation text.
 * @param options Optional title, kind, and custom labels. If a string, used as title.
 * @returns       `true` if the user clicked Ok, `false` otherwise.
 *
 * @example
 * ```ts
 * const ok = await confirm('Discard unsaved changes?', {
 *   title: 'Unsaved Changes',
 *   kind: 'warning',
 * });
 * ```
 */
export async function confirm(
  msg: string,
  options?: string | ConfirmDialogOptions,
): Promise<boolean> {
  const opts: Record<string, unknown> =
    typeof options === 'string'
      ? { title: options, message: msg }
      : { ...(options ?? {}), message: msg };
  return invoke<boolean>('dialog:confirm', opts);
}
