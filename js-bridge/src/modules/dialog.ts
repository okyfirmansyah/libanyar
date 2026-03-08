// ---------------------------------------------------------------------------
// @libanyar/api/dialog — Native dialog commands
// ---------------------------------------------------------------------------

import { invoke } from '../invoke';

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

/** Options for message/confirm dialogs. */
export interface MessageDialogOptions {
  title?: string;
  message: string;
  /** Dialog type affects the icon shown. */
  kind?: 'info' | 'warning' | 'error';
}

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

/**
 * Show a native message dialog (info/warning/error).
 */
export async function message(opts: MessageDialogOptions): Promise<void> {
  await invoke<void>('dialog:message', opts);
}

/**
 * Show a native confirmation dialog.
 *
 * @returns `true` if the user clicked OK/Yes, `false` otherwise.
 */
export async function confirm(
  opts: MessageDialogOptions,
): Promise<boolean> {
  return invoke<boolean>('dialog:confirm', opts);
}
