/**
 * LibAnyar JS Bridge
 *
 * Provides `invoke()` for commands (via HTTP POST) and
 * `listen()` / `emit()` for events (via WebSocket).
 *
 * Usage:
 *   import { invoke, listen, emit } from '@anyar/bridge';
 *   const result = await invoke('greet', { name: 'Alice' });
 *   const unsub = listen('counter:updated', (payload) => { ... });
 */

// ── Configuration ───────────────────────────────────────────────────────────

function getPort() {
  // Injected by C++ Window via webview_init()
  return window.__LIBANYAR_PORT__ || 3080;
}

function getBaseUrl() {
  return `http://127.0.0.1:${getPort()}`;
}

// ── Command Invocation (HTTP POST) ─────────────────────────────────────────

let requestId = 0;

/**
 * Invoke a C++ backend command.
 * @param {string} cmd  — Command name (e.g. 'greet')
 * @param {object} args — Arguments to pass to the command
 * @returns {Promise<any>} — Resolved with the command's return data
 */
export async function invoke(cmd, args = {}) {
  const id = String(++requestId);

  const response = await fetch(`${getBaseUrl()}/__anyar__/invoke`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id, cmd, args }),
  });

  const result = await response.json();

  if (result.error) {
    throw new Error(result.error);
  }

  return result.data;
}

// ── Event System (WebSocket) ────────────────────────────────────────────────

let ws = null;
let wsReady = false;
const wsQueue = [];
const wsListeners = new Map();

function ensureWebSocket() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
    return;
  }

  ws = new WebSocket(`ws://127.0.0.1:${getPort()}/__anyar_ws__`);

  ws.onopen = () => {
    wsReady = true;
    // Flush queued messages
    while (wsQueue.length > 0) {
      ws.send(wsQueue.shift());
    }
  };

  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg.event) {
        const handlers = wsListeners.get(msg.event);
        if (handlers) {
          handlers.forEach((handler) => {
            try {
              handler(msg.payload);
            } catch (e) {
              console.error(`[anyar] Event handler error for '${msg.event}':`, e);
            }
          });
        }
      }
    } catch (e) {
      console.error('[anyar] Failed to parse WebSocket message:', e);
    }
  };

  ws.onclose = () => {
    wsReady = false;
    // Auto-reconnect after 1 second
    setTimeout(() => ensureWebSocket(), 1000);
  };

  ws.onerror = (err) => {
    console.error('[anyar] WebSocket error:', err);
  };
}

/**
 * Listen for events from the C++ backend.
 * @param {string} event — Event name
 * @param {function} handler — Callback receiving the event payload
 * @returns {function} — Unsubscribe function
 */
export function listen(event, handler) {
  ensureWebSocket();

  if (!wsListeners.has(event)) {
    wsListeners.set(event, new Set());
  }
  wsListeners.get(event).add(handler);

  return () => {
    const handlers = wsListeners.get(event);
    if (handlers) {
      handlers.delete(handler);
      if (handlers.size === 0) {
        wsListeners.delete(event);
      }
    }
  };
}

/**
 * Emit an event to the C++ backend via WebSocket.
 * @param {string} event — Event name
 * @param {object} payload — Event data
 */
export function emit(event, payload = {}) {
  ensureWebSocket();

  const message = JSON.stringify({ event, payload });
  if (wsReady && ws.readyState === WebSocket.OPEN) {
    ws.send(message);
  } else {
    wsQueue.push(message);
  }
}

// ── Convenience: attach to window for non-module usage ──────────────────────

if (typeof window !== 'undefined') {
  window.__anyar__ = { invoke, listen, emit };
}
