// ---------------------------------------------------------------------------
// @libanyar/api — Main entry point
// ---------------------------------------------------------------------------

// ── Core API ───────────────────────────────────────────────────────────────
export { invoke } from './invoke';
export { listen, emit, emitTo, listenGlobal, once, onReady } from './events';
export { getPort, setPort, getBaseUrl, getWsUrl, isNativeIpc } from './config';

// ── Window Management ──────────────────────────────────────────────────────
export {
  createWindow,
  closeWindow,
  closeAll,
  listWindows,
  setTitle,
  setSize,
  focusWindow,
  setEnabled,
  setAlwaysOnTop,
  getLabel,
  onWindowClosed,
  onWindowCreated,
  setCloseConfirmation,
} from './modules/window';

export type {
  WindowOptions,
  WindowInfo,
  CloseRequestedEvent,
  CloseConfirmationOptions,
} from './modules/window';

// ── Dialog ─────────────────────────────────────────────────────────────────
import * as dialog from './modules/dialog';
export { dialog };

// ── Buffer ─────────────────────────────────────────────────────────────────
import * as buffer from './modules/buffer';
export { buffer };

// ── Canvas ─────────────────────────────────────────────────────────────────
import * as canvas from './modules/canvas';
export { canvas };

export type {
  DialogFilter,
  OpenDialogOptions,
  SaveDialogOptions,
  MessageDialogOptions,
  MessageDialogButtons,
  MessageDialogResult,
  ConfirmDialogOptions,
} from './modules/dialog';

// ── Types ──────────────────────────────────────────────────────────────────
export type {
  IpcRequest,
  IpcResponse,
  IpcError,
  EventMessage,
  EventHandler,
  UnlistenFn,
} from './types';

// ── Expose on window for non-module usage ──────────────────────────────────
import { invoke } from './invoke';
import { listen, emit, emitTo, listenGlobal, once, onReady } from './events';
import { getPort, setPort, isNativeIpc } from './config';
import {
  createWindow,
  closeWindow,
  closeAll,
  listWindows,
  getLabel,
} from './modules/window';
import * as _dialog from './modules/dialog';
import * as _buffer from './modules/buffer';
import * as _canvas from './modules/canvas';

if (typeof window !== 'undefined') {
  (window as any).__anyar__ = {
    invoke,
    listen,
    emit,
    emitTo,
    listenGlobal,
    once,
    onReady,
    getPort,
    setPort,
    isNativeIpc,
    createWindow,
    closeWindow,
    closeAll,
    listWindows,
    getLabel,
    dialog: _dialog,
    buffer: _buffer,
    canvas: _canvas,
  };
}
