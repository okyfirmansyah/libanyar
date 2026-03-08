// ---------------------------------------------------------------------------
// @libanyar/api — Event channel
//
// Primary path: native push via webview_eval   (in-process, C++ → JS)
//               native invoke for emit         (in-process, JS → C++)
// Fallback:     WebSocket                      (localhost, bidirectional)
// ---------------------------------------------------------------------------

import { getWsUrl } from './config';
import { invoke } from './invoke';
import type { EventMessage, EventHandler, UnlistenFn } from './types';

// ── Internal state ─────────────────────────────────────────────────────────

type Listener = { id: number; event: string; handler: EventHandler<any> };

let _ws: WebSocket | null = null;
let _listeners: Listener[] = [];
let _listenerId = 0;
let _sendQueue: string[] = [];
let _reconnectTimer: ReturnType<typeof setTimeout> | null = null;
let _ready = false;
let _onReadyCallbacks: Array<() => void> = [];
let _nativeBridged = false;

const RECONNECT_MS = 1000;

/** True when running inside a LibAnyar webview with direct IPC. */
function isNative(): boolean {
  return (
    typeof window !== 'undefined' &&
    !!(window as any).__LIBANYAR_NATIVE__
  );
}

// ── Native event bridge ────────────────────────────────────────────────────
//
// The C++ side injects window.__anyar_dispatch_event__(msg) which is called
// via webview_eval() whenever the backend emits an event.  We hook into that
// global so it dispatches into our _listeners array.

function ensureNativeBridge(): void {
  if (_nativeBridged) return;
  _nativeBridged = true;

  const w = window as any;

  // Ensure the listeners map exists (C++ init script also creates it, but
  // this is a safety belt for ordering).
  if (!w.__anyar_event_listeners__) {
    w.__anyar_event_listeners__ = {};
  }

  // Replace the C++-injected dispatcher with one that feeds our _listeners.
  // This runs both the per-event array from the C++ init script AND our own
  // module-level _listeners.
  const origDispatch = w.__anyar_dispatch_event__;
  w.__anyar_dispatch_event__ = function (msg: EventMessage) {
    if (!msg || !msg.event) return;
    // Dispatch to our module listeners
    dispatch(msg.event, msg.payload);
    // Also call C++ init-script listeners (if anything registered there directly)
    if (origDispatch && origDispatch !== w.__anyar_dispatch_event__) {
      try { origDispatch(msg); } catch { /* ignore */ }
    }
  };

  // Mark ready immediately — native IPC is always available.
  _ready = true;
  for (const cb of _onReadyCallbacks) {
    try { cb(); } catch { /* ignore */ }
  }
  _onReadyCallbacks = [];
}

// ── WebSocket fallback ─────────────────────────────────────────────────────

function ensureConnected(): void {
  if (_ws && (_ws.readyState === WebSocket.OPEN || _ws.readyState === WebSocket.CONNECTING)) {
    return;
  }
  connect();
}

function connect(): void {
  try {
    _ws = new WebSocket(getWsUrl());
  } catch {
    scheduleReconnect();
    return;
  }

  _ws.onopen = () => {
    _ready = true;
    for (const msg of _sendQueue) {
      _ws!.send(msg);
    }
    _sendQueue = [];
    for (const cb of _onReadyCallbacks) {
      try { cb(); } catch { /* ignore */ }
    }
    _onReadyCallbacks = [];
  };

  _ws.onmessage = (ev) => {
    try {
      const msg: EventMessage = JSON.parse(ev.data);
      if (msg.type === 'event' && msg.event) {
        dispatch(msg.event, msg.payload);
      }
    } catch {
      // Ignore malformed messages
    }
  };

  _ws.onclose = () => {
    _ready = false;
    scheduleReconnect();
  };

  _ws.onerror = () => {};
}

function scheduleReconnect(): void {
  if (_reconnectTimer) return;
  _reconnectTimer = setTimeout(() => {
    _reconnectTimer = null;
    connect();
  }, RECONNECT_MS);
}

function dispatch(event: string, payload: unknown): void {
  for (const l of _listeners) {
    if (l.event === event || l.event === '*') {
      try {
        l.handler(payload);
      } catch (err) {
        console.error(`[LibAnyar] Event handler error (${event}):`, err);
      }
    }
  }
}

// ── Public API ─────────────────────────────────────────────────────────────

/**
 * Subscribe to a named event from the C++ backend.
 *
 * In a webview, events arrive via the native push channel (webview_eval).
 * Outside a webview (e.g. `vite dev`), they fall back to WebSocket.
 */
export function listen<T = unknown>(
  event: string,
  handler: EventHandler<T>,
): UnlistenFn {
  const id = ++_listenerId;
  _listeners.push({ id, event, handler: handler as EventHandler<any> });

  if (isNative()) {
    ensureNativeBridge();
  } else {
    ensureConnected();
  }

  return () => {
    _listeners = _listeners.filter((l) => l.id !== id);
  };
}

/**
 * Emit an event from the frontend to the C++ backend.
 *
 * In a webview, this routes through the native IPC invoke mechanism.
 * Outside a webview, it falls back to WebSocket.
 */
export function emit(event: string, payload: unknown = null): void {
  if (isNative()) {
    // Route through native IPC — calls anyar:emit_event which dispatches
    // to C++ subscribers only (no echo back to frontends).
    invoke('anyar:emit_event', { event, payload }).catch((err) => {
      console.error('[LibAnyar] Native emit failed:', err);
    });
    return;
  }

  // WebSocket fallback
  const msg: EventMessage = { type: 'event', event, payload };
  const data = JSON.stringify(msg);

  if (_ws && _ws.readyState === WebSocket.OPEN) {
    _ws.send(data);
  } else {
    _sendQueue.push(data);
    ensureConnected();
  }
}

/**
 * Register a callback that fires when the IPC channel is ready.
 * In native mode this fires immediately; in WebSocket mode it fires
 * once the connection is established.
 */
export function onReady(fn: () => void): void {
  if (isNative()) {
    ensureNativeBridge();
    fn();
    return;
  }

  if (_ready) {
    fn();
  } else {
    _onReadyCallbacks.push(fn);
    ensureConnected();
  }
}

/**
 * Convenience: listen to an event only once.
 */
export function once<T = unknown>(
  event: string,
  handler: EventHandler<T>,
): UnlistenFn {
  const unlisten = listen<T>(event, (payload) => {
    unlisten();
    handler(payload);
  });
  return unlisten;
}
