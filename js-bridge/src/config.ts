// ---------------------------------------------------------------------------
// @libanyar/api — Port / base-URL resolution
// ---------------------------------------------------------------------------

let _port: number | null = null;

/**
 * Resolve the port the LibAnyar backend is listening on.
 *
 * Priority:
 * 1. Explicit override via `setPort()`
 * 2. `window.__LIBANYAR_PORT__` injected by C++ `webview_init()`
 * 3. `<meta name="libanyar-port" content="...">` in the HTML `<head>`
 * 4. Fallback to `3080`
 */
export function getPort(): number {
  if (_port !== null) return _port;

  // window global (injected by C++ Window)
  if (typeof window !== 'undefined' && window.__LIBANYAR_PORT__) {
    return window.__LIBANYAR_PORT__;
  }

  // <meta> tag fallback
  if (typeof document !== 'undefined') {
    const meta = document.querySelector<HTMLMetaElement>(
      'meta[name="libanyar-port"]',
    );
    if (meta?.content) {
      const p = Number(meta.content);
      if (!Number.isNaN(p) && p > 0) return p;
    }
  }

  return 3080;
}

/**
 * Override the port used for all IPC calls.
 * Useful for development / testing outside a webview.
 */
export function setPort(port: number): void {
  _port = port;
}

/** HTTP base URL for command invocations. */
export function getBaseUrl(): string {
  return `http://127.0.0.1:${getPort()}`;
}

/** WebSocket URL for the event channel. */
export function getWsUrl(): string {
  return `ws://127.0.0.1:${getPort()}/__anyar_ws__`;
}

/**
 * True when running inside a LibAnyar webview with native IPC.
 * Commands go through webview_bind (in-process), events through
 * webview_eval — no TCP/HTTP/WS overhead.
 */
export function isNativeIpc(): boolean {
  return (
    typeof window !== 'undefined' &&
    typeof (window as any).__anyar_ipc__ === 'function'
  );
}
