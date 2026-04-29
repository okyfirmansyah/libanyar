// ---------------------------------------------------------------------------
// @libanyar/api/pinhole — Native Overlay Tracking Module
//
// Provides TypeScript-level types and callbacks for the Pinhole native
// overlay API.  The actual DOM observation and IPC protocol are handled by a
// self-contained JS snippet injected automatically by Window::create_pinhole()
// on the C++ side (via webview_init).  This module is therefore optional —
// you only need it if you want typed access to pinhole lifecycle events or
// want to call pinhole IPC commands directly from TypeScript.
//
// Usage:
//   import { onPinholeMounted, getPinholeMetrics } from '@libanyar/api/pinhole';
//
//   onPinholeMounted('my-overlay', () => {
//     console.log('pinhole is live');
//   });
// ---------------------------------------------------------------------------

import { invoke } from '../invoke';
import type { UnlistenFn } from '../types';

// ── Types ──────────────────────────────────────────────────────────────────

/** CSS-pixel rectangle reported by the JS tracking protocol. */
export interface PinholeRect {
  x:      number;
  y:      number;
  width:  number;
  height: number;
}

/** Metrics returned by pinhole:get_metrics */
export interface PinholeMetrics {
  ok:        boolean;
  id:        string;
  is_native: boolean;
  error?:    string;
}

/** Options for initPinholes() — reserved for future configuration. */
export interface PinholeInitOptions {
  /** Scroll-hide debounce in ms. Default: 100. */
  scrollHideDebounceMs?: number;
}

// ── Internal state ─────────────────────────────────────────────────────────

type MountedCallback = () => void;

const _mountedListeners: Map<string, MountedCallback[]> = new Map();

// ── Lifecycle hooks ────────────────────────────────────────────────────────

/**
 * Register a callback invoked when a pinhole with the given id becomes
 * active (i.e. the DOM element with `data-anyar-pinhole="<id>"` is found
 * and the first `pinhole:update_rect` IPC has been sent).
 *
 * @param id  Pinhole id matching the `data-anyar-pinhole` attribute.
 * @param fn  Callback invoked with no arguments.
 * @returns   Unlisten function.
 *
 * @example
 * ```ts
 * const unlisten = onPinholeMounted('video-overlay', () => {
 *   console.log('native surface is live');
 * });
 * // later:
 * unlisten();
 * ```
 */
export function onPinholeMounted(id: string, fn: MountedCallback): UnlistenFn {
  const list = _mountedListeners.get(id) ?? [];
  list.push(fn);
  _mountedListeners.set(id, list);
  return () => {
    const cur = _mountedListeners.get(id);
    if (cur) {
      const idx = cur.indexOf(fn);
      if (idx !== -1) cur.splice(idx, 1);
    }
  };
}

// Called by the injected bootstrap when the first rect is sent for an id.
// Exposed on window so the plain JS snippet can call it.
if (typeof window !== 'undefined') {
  (window as any).__anyar_pinhole_mounted__ = (id: string) => {
    const listeners = _mountedListeners.get(id) ?? [];
    listeners.forEach(fn => fn());
  };
}

// ── IPC helpers ────────────────────────────────────────────────────────────

/**
 * Manually send an updated CSS-pixel rect for a pinhole.
 * Normally called automatically by the injected tracking script.
 * Use this only if you need to override the tracked rect.
 *
 * @param id     Pinhole id.
 * @param rect   New CSS-pixel rectangle.
 * @param label  Window label. Defaults to current window's label.
 */
export async function updatePinholeRect(
  id: string,
  rect: PinholeRect,
  label?: string,
): Promise<void> {
  const win_label =
    label ??
    ((window as any).__LIBANYAR_WINDOW_LABEL__ as string | undefined) ??
    'main';
  await invoke('pinhole:update_rect', {
    id,
    window_label: win_label,
    ...rect,
    dpr: window.devicePixelRatio ?? 1,
  });
}

/**
 * Show or hide a pinhole overlay.
 * Normally called automatically by the scroll-hide protocol.
 *
 * @param id      Pinhole id.
 * @param visible True to show, false to hide.
 * @param label   Window label. Defaults to current window's label.
 */
export async function setPinholeVisible(
  id: string,
  visible: boolean,
  label?: string,
): Promise<void> {
  const win_label =
    label ??
    ((window as any).__LIBANYAR_WINDOW_LABEL__ as string | undefined) ??
    'main';
  await invoke('pinhole:set_visible', { id, window_label: win_label, visible });
}

/**
 * Query current metrics for a pinhole from C++.
 *
 * @param id     Pinhole id.
 * @param label  Window label. Defaults to current window's label.
 * @returns      PinholeMetrics including is_native() status.
 *
 * @example
 * ```ts
 * const m = await getPinholeMetrics('video-overlay');
 * console.log('native GL overlay:', m.is_native);
 * ```
 */
export async function getPinholeMetrics(
  id: string,
  label?: string,
): Promise<PinholeMetrics> {
  const win_label =
    label ??
    ((window as any).__LIBANYAR_WINDOW_LABEL__ as string | undefined) ??
    'main';
  return invoke<PinholeMetrics>('pinhole:get_metrics', {
    id,
    window_label: win_label,
  });
}
