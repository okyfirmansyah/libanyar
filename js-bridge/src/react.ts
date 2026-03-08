// ---------------------------------------------------------------------------
// @libanyar/api/react — React hooks for LibAnyar
// ---------------------------------------------------------------------------

import { useState, useEffect, useCallback, useRef } from 'react';
import { invoke } from './invoke';
import { listen } from './events';
import type { UnlistenFn } from './types';

// ── useInvoke ──────────────────────────────────────────────────────────────

export interface UseInvokeResult<T> {
  /** Latest data returned by the command. */
  data: T | null;
  /** `true` while the request is in flight. */
  loading: boolean;
  /** Error from the latest invocation. */
  error: Error | null;
  /** Manually re-invoke the command. */
  refetch: () => void;
}

/**
 * React hook that invokes a C++ command and manages loading/error state.
 *
 * Re-invokes when `cmd` or `args` change (by JSON comparison).
 *
 * @param cmd  Command name.
 * @param args Arguments object (must be JSON-stable for dependency tracking).
 *
 * @example
 * ```tsx
 * function Info() {
 *   const { data, loading, error } = useInvoke<{ version: string }>('get_info');
 *   if (loading) return <p>Loading…</p>;
 *   if (error) return <p>Error: {error.message}</p>;
 *   return <p>Version: {data?.version}</p>;
 * }
 * ```
 */
export function useInvoke<T = unknown>(
  cmd: string,
  args: Record<string, unknown> = {},
): UseInvokeResult<T> {
  const [data, setData] = useState<T | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<Error | null>(null);
  const argsKey = JSON.stringify(args);

  const fetch = useCallback(() => {
    setLoading(true);
    setError(null);
    invoke<T>(cmd, args)
      .then((d) => {
        setData(d);
        setError(null);
      })
      .catch((e) => {
        setError(e instanceof Error ? e : new Error(String(e)));
      })
      .finally(() => setLoading(false));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cmd, argsKey]);

  useEffect(() => {
    fetch();
  }, [fetch]);

  return { data, loading, error, refetch: fetch };
}

// ── useEvent ───────────────────────────────────────────────────────────────

/**
 * React hook that subscribes to a LibAnyar event and re-renders on every
 * new payload.
 *
 * @param event  Event name (or `"*"` for all).
 * @returns      The latest event payload, or `null` initial.
 *
 * @example
 * ```tsx
 * function Counter() {
 *   const payload = useEvent<{ count: number }>('counter:updated');
 *   return <p>Count: {payload?.count ?? '—'}</p>;
 * }
 * ```
 */
export function useEvent<T = unknown>(event: string): T | null {
  const [payload, setPayload] = useState<T | null>(null);

  useEffect(() => {
    const unlisten: UnlistenFn = listen<T>(event, (p) => setPayload(p));
    return unlisten;
  }, [event]);

  return payload;
}

// ── useEventCallback ───────────────────────────────────────────────────────

/**
 * Subscribe to an event with a stable callback (doesn't cause re-renders).
 * Useful for side-effects (logging, toasts, etc.).
 */
export function useEventCallback<T = unknown>(
  event: string,
  handler: (payload: T) => void,
): void {
  const handlerRef = useRef(handler);
  handlerRef.current = handler;

  useEffect(() => {
    const unlisten = listen<T>(event, (p) => handlerRef.current(p));
    return unlisten;
  }, [event]);
}
