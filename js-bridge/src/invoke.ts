// ---------------------------------------------------------------------------
// @libanyar/api — Command invocation
//
// Primary path: native IPC via webview_bind  (in-process, ~0.01–0.1 ms)
// Fallback:     HTTP POST                    (localhost,   ~0.5–2 ms)
// ---------------------------------------------------------------------------

import { getBaseUrl } from './config';
import type { IpcRequest, IpcResponse } from './types';

let _idCounter = 0;

function makeId(): string {
  return `anyar_${Date.now()}_${++_idCounter}`;
}

/**
 * Invoke a C++ command registered on the backend.
 *
 * When running inside a LibAnyar webview the call goes through a direct
 * `webview_bind` bridge (no networking).  Outside the webview (e.g. `vite
 * dev` in a browser) it falls back to an HTTP POST to the backend server.
 *
 * @typeParam T  Expected return type from the command handler.
 * @param cmd    Command name (e.g. `"greet"`, `"fs:readFile"`).
 * @param args   Optional arguments object passed to the handler.
 * @returns      The `data` field from the backend response.
 * @throws       If the backend returns an error or the network request fails.
 */
export async function invoke<T = unknown>(
  cmd: string,
  args: Record<string, unknown> | object = {},
): Promise<T> {
  const body: IpcRequest = { id: makeId(), cmd, args: args as Record<string, unknown> };

  // ── Native IPC (direct webview binding — fastest path) ────────────────
  if (
    typeof window !== 'undefined' &&
    typeof (window as any).__anyar_ipc__ === 'function'
  ) {
    // webview_bind's onReply already JSON.parse()s the result before
    // resolving the Promise, so the resolved value is a plain object —
    // not a JSON string.  No need to parse again.
    const result = (await (window as any).__anyar_ipc__(
      JSON.stringify(body),
    )) as IpcResponse<T>;
    if (result.error) {
      const err = new Error(result.error.message);
      (err as any).code = result.error.code;
      throw err;
    }
    return result.data as T;
  }

  // ── HTTP fallback (development mode / non-webview) ────────────────────
  const res = await fetch(`${getBaseUrl()}/__anyar__/invoke`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });

  if (!res.ok) {
    throw new Error(`LibAnyar IPC HTTP error: ${res.status} ${res.statusText}`);
  }

  const result: IpcResponse<T> = await res.json();

  if (result.error) {
    const err = new Error(result.error.message);
    (err as any).code = result.error.code;
    throw err;
  }

  return result.data as T;
}
