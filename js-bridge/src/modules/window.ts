// ---------------------------------------------------------------------------
// @libanyar/api — Window management module
//
// Provides functions to create, close, and manage multiple windows.
// Uses native IPC commands registered by the C++ WindowManager.
// ---------------------------------------------------------------------------

import { invoke } from '../invoke';
import { listen } from '../events';
import type { UnlistenFn, EventHandler } from '../types';

// ── Types ──────────────────────────────────────────────────────────────────

/** Options for creating a new window. */
export interface WindowOptions {
  /** Unique window identifier (alphanumeric + `-/:_`). */
  label: string;
  /** Window title. Default: "LibAnyar" */
  title?: string;
  /** Width in pixels. Default: 800 */
  width?: number;
  /** Height in pixels. Default: 600 */
  height?: number;
  /** URL path to load (e.g. "/" or "/#/settings"). Default: "/" */
  url?: string;
  /** Parent window label. Empty = top-level window. */
  parent?: string;
  /** Block interaction with parent until closed. Default: false */
  modal?: boolean;
  /** Allow user resize. Default: true */
  resizable?: boolean;
  /** Center on screen/parent. Default: true */
  center?: boolean;
  /** Keep above other windows. Default: false */
  alwaysOnTop?: boolean;
  /** Show title bar and borders. Default: true */
  decorations?: boolean;
  /** Allow user close via title-bar X. Default: true */
  closable?: boolean;
  /** Allow user minimize. Default: true */
  minimizable?: boolean;
}

/** Information about an open window. */
export interface WindowInfo {
  label: string;
}

/** Event emitted when close is requested (interceptable). */
export interface CloseRequestedEvent {
  preventDefault: () => void;
}

// ── Window Operations ──────────────────────────────────────────────────────

/**
 * Create a new window.
 *
 * @param opts  Window creation options. `label` is required.
 * @returns     The label of the created window.
 */
export async function createWindow(opts: WindowOptions): Promise<string> {
  const result = await invoke<{ label: string }>('window:create', opts);
  return result.label;
}

/**
 * Close a window. If no label is given, closes the current window.
 *
 * @param label  Window label to close. Default: current window's label.
 */
export async function closeWindow(label?: string): Promise<void> {
  const target = label ?? getLabel();
  await invoke('window:close', { label: target });
}

/**
 * Close all windows and exit the application.
 */
export async function closeAll(): Promise<void> {
  await invoke('window:close-all', {});
}

/**
 * List all open window labels.
 */
export async function listWindows(): Promise<WindowInfo[]> {
  return invoke<WindowInfo[]>('window:list', {});
}

/**
 * Change a window's title.
 *
 * @param label  Window label.
 * @param title  New title text.
 */
export async function setTitle(label: string, title: string): Promise<void> {
  await invoke('window:set-title', { label, title });
}

/**
 * Resize a window.
 *
 * @param label   Window label.
 * @param width   New width in pixels.
 * @param height  New height in pixels.
 */
export async function setSize(
  label: string,
  width: number,
  height: number,
): Promise<void> {
  await invoke('window:set-size', { label, width, height });
}

/**
 * Bring a window to the front and give it focus.
 *
 * @param label  Window label.
 */
export async function focusWindow(label: string): Promise<void> {
  await invoke('window:focus', { label });
}

/**
 * Enable or disable input on a window (pseudo-modal pattern).
 *
 * @param label    Window label.
 * @param enabled  Whether input is enabled.
 */
export async function setEnabled(
  label: string,
  enabled: boolean,
): Promise<void> {
  await invoke('window:set-enabled', { label, enabled });
}

/**
 * Toggle always-on-top for a window.
 *
 * @param label       Window label.
 * @param alwaysOnTop Whether to keep the window on top.
 */
export async function setAlwaysOnTop(
  label: string,
  alwaysOnTop: boolean,
): Promise<void> {
  await invoke('window:set-always-on-top', { label, alwaysOnTop });
}

/**
 * Get the current window's label. Synchronous — reads from
 * `window.__LIBANYAR_WINDOW_LABEL__` injected by the C++ bridge.
 *
 * @returns The window label string.
 */
export function getLabel(): string {
  if (typeof window !== 'undefined') {
    return (window as any).__LIBANYAR_WINDOW_LABEL__ ?? 'main';
  }
  return 'main';
}

/**
 * Listen for the `window:closed` event.
 *
 * @param handler  Called with `{ label }` when any window closes.
 */
export function onWindowClosed(
  handler: EventHandler<{ label: string }>,
): UnlistenFn {
  return listen('window:closed', handler);
}

/**
 * Listen for the `window:created` event.
 *
 * @param handler  Called with `{ label, title }` when a window is created.
 */
export function onWindowCreated(
  handler: EventHandler<{ label: string; title: string }>,
): UnlistenFn {
  return listen('window:created', handler);
}
