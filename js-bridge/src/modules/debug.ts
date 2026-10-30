// ---------------------------------------------------------------------------
// @libanyar/api/debug — debugging & UI automation (docs/debugging.md)
// ---------------------------------------------------------------------------
//
// These commands exist only when the app enables automation
// (`AppConfig::debug_tools.automation`, `ANYAR_DEBUG_TOOLS=1` or
// `ANYAR_TEST_SCRIPT=…`); otherwise they reject with "unknown command".
// Capture and input are implemented on Linux (WebKitGTK) and report
// "not implemented" on Windows/macOS for now — check `info()`.

import { invoke } from '../invoke';

/** What the running app supports (result of {@link info}). */
export interface DebugInfo {
  platform: 'linux' | 'windows' | 'macos' | 'unknown';
  /** Console forwarding to the process output is on. */
  console: boolean;
  /** `debug:*` automation commands are registered. */
  automation: boolean;
  /** Injected test script path, or empty. */
  test_script: string;
  /** {@link screenshot} works on this platform. */
  capture: boolean;
  /** {@link click}, {@link key} etc. work on this platform. */
  input: boolean;
}

/** Result of {@link screenshot}. */
export interface Screenshot {
  width: number;
  height: number;
  bytes: number;
  /** Absolute path, when a path was given. */
  path?: string;
  /** PNG as base64, when no path was given. */
  png_base64?: string;
}

/** Options shared by the input helpers. */
export interface InputOptions {
  /** Target window label (default: the calling window). */
  label?: string;
  /** Mouse button: 1 left, 2 middle, 3 right. */
  button?: number;
  shift?: boolean;
  ctrl?: boolean;
  alt?: boolean;
  meta?: boolean;
}

/** A CSS selector, an element, or a point in CSS px relative to the viewport. */
export type Target = string | Element | { x: number; y: number };

/**
 * Resolve a {@link Target} to the centre point of the element (CSS px).
 * @param target  Selector, element or point.
 * @returns       The point.
 * @example
 * const p = targetPoint('#save'); // { x: 412, y: 36 }
 */
export function targetPoint(target: Target): { x: number; y: number } {
  if (typeof target === 'object' && 'x' in target && 'y' in target && !(target instanceof Element)) {
    return { x: target.x, y: target.y };
  }
  const el = typeof target === 'string' ? document.querySelector(target) : target;
  if (!el) throw new Error(`debug: no element matches ${String(target)}`);
  const r = el.getBoundingClientRect();
  return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
}

/**
 * Report which debug tools this app has enabled and the platform supports.
 * @returns Capability flags.
 * @example
 * if ((await debug.info()).capture) await debug.screenshot({ path: 'shot.png' });
 */
export async function info(): Promise<DebugInfo> {
  return invoke<DebugInfo>('debug:info');
}

/**
 * Capture the real webview (DOM, CSS, WebGL) as PNG. Native Pinhole
 * surfaces are not included.
 * @param options  `path` writes the PNG there (relative to the app's working
 *                 directory); otherwise it is returned as base64. `label`
 *                 targets another window.
 * @returns        Size and the file path or base64 data.
 * @example
 * await debug.screenshot({ path: 'artifacts/home.png' });
 */
export async function screenshot(options: { path?: string; label?: string } = {}): Promise<Screenshot> {
  return invoke<Screenshot>('debug:screenshot', options);
}

/**
 * Click at a target through the OS/toolkit event path (a real user gesture,
 * unlike `element.click()`).
 * @param target   Selector, element or `{x, y}` in CSS px.
 * @param options  Button, modifiers, window.
 * @returns        Resolves once the event was delivered.
 * @example
 * await debug.click('#open-file');
 */
export async function click(target: Target, options: InputOptions = {}): Promise<void> {
  const p = targetPoint(target);
  await invoke('debug:input', { type: 'click', ...p, ...options });
}

/**
 * Move the pointer to a target (fires hover / pointermove handlers).
 * @param target   Selector, element or point.
 * @param options  Modifiers, window.
 * @returns        Resolves once delivered.
 * @example
 * await debug.moveTo('canvas');
 */
export async function moveTo(target: Target, options: InputOptions = {}): Promise<void> {
  await invoke('debug:input', { type: 'move', ...targetPoint(target), ...options });
}

/**
 * Press or release a mouse button at a target (for drags: down → moveTo → up).
 * @param kind     'down' or 'up'.
 * @param target   Selector, element or point.
 * @param options  Button, modifiers, window.
 * @returns        Resolves once delivered.
 * @example
 * await debug.mouse('down', { x: 100, y: 100 });
 */
export async function mouse(kind: 'down' | 'up', target: Target, options: InputOptions = {}): Promise<void> {
  await invoke('debug:input', { type: kind, ...targetPoint(target), ...options });
}

/**
 * Scroll (smooth wheel) at a target.
 * @param target   Selector, element or point.
 * @param dy       Vertical delta in wheel notches (positive = down).
 * @param dx       Horizontal delta.
 * @returns        Resolves once delivered.
 * @example
 * await debug.scroll('#list', 3);
 */
export async function scroll(target: Target, dy: number, dx = 0, options: InputOptions = {}): Promise<void> {
  await invoke('debug:input', { type: 'scroll', ...targetPoint(target), dx, dy, ...options });
}

/**
 * Press and release one key in the focused element.
 * @param key      DOM key name: "Enter", "Escape", "Tab", "ArrowDown", "F5", "a" …
 * @param options  Modifiers (e.g. `{ ctrl: true }`), window.
 * @returns        Resolves once delivered.
 * @example
 * await debug.key('Enter');
 */
export async function key(key: string, options: InputOptions = {}): Promise<void> {
  await invoke('debug:input', { type: 'key', key, ...options });
}

/**
 * Type text into the focused element, one key press per character.
 * @param text     Text to type.
 * @param options  Window.
 * @returns        Resolves once delivered.
 * @example
 * await debug.typeText('left femur');
 */
export async function typeText(text: string, options: InputOptions = {}): Promise<void> {
  await invoke('debug:input', { type: 'text', text, ...options });
}

/**
 * Close every window and make the app's `run()` return @p code — the end of
 * an automated test.
 * @param code  Process exit code (0 = pass).
 * @returns     Resolves when the request was accepted.
 * @example
 * await debug.exit(failures ? 1 : 0);
 */
export async function exit(code = 0): Promise<void> {
  await invoke('debug:exit', { code });
}
