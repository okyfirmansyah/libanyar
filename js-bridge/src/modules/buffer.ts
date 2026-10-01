// ---------------------------------------------------------------------------
// @libanyar/api/buffer — Shared Memory Buffer module
//
// Provides zero-copy (or near-zero-copy) binary data transfer between the
// C++ backend and the webview frontend via shared memory.
//
// Transport, picked per platform by fetchBuffer():
//   Windows (WebView2): the buffer memory itself, mapped into the page via
//     `buffer:attach` + the `sharedbufferreceived` event (zero-copy)
//   Linux (WebKitGTK):  anyar-shm:// custom URI scheme
//   Browser dev mode / fallback: HTTP GET /__anyar__/buffer/<name>
// ---------------------------------------------------------------------------

import { invoke } from '../invoke';
import { listen } from '../events';
import { isNativeIpc, getBaseUrl } from '../config';
import type { UnlistenFn, EventHandler } from '../types';

// ── Types ──────────────────────────────────────────────────────────────────

/** Information about a shared buffer returned from the backend. */
export interface SharedBufferInfo {
  /** Unique name of the buffer. */
  name: string;
  /** Size in bytes. */
  size: number;
  /** URI for zero-copy fetch (e.g. "anyar-shm://my-buffer"). */
  url: string;
  /** Generation id (distinguishes a recreated buffer with the same name). */
  id?: number;
}

/** Payload for the `buffer:ready` event. */
export interface BufferReadyEvent {
  /** Buffer name. */
  name: string;
  /** Pool name (if buffer belongs to a pool). */
  pool?: string;
  /** URI for zero-copy fetch. */
  url: string;
  /** Buffer generation id — pass to fetchBuffer() to skip revalidation. */
  id?: number;
  /** Buffer size in bytes. */
  size: number;
  /** Arbitrary metadata from the producer. */
  metadata: Record<string, unknown>;
}

/** Information about a shared buffer pool. */
export interface SharedBufferPoolInfo {
  /** Pool base name. */
  name: string;
  /** Size of each buffer in bytes. */
  bufferSize: number;
  /** Number of buffers in the pool. */
  count: number;
  /** Individual buffer infos. */
  buffers: SharedBufferInfo[];
}

// ── Buffer CRUD ────────────────────────────────────────────────────────────

/**
 * Create a named shared memory buffer on the backend.
 *
 * @param name  Unique buffer name.
 * @param size  Size in bytes.
 * @returns     Buffer info including the anyar-shm:// URL.
 */
export async function createBuffer(
  name: string,
  size: number,
): Promise<SharedBufferInfo> {
  return invoke<SharedBufferInfo>('buffer:create', { name, size });
}

/**
 * Write data into a shared buffer from the JS side.
 * The data is base64-encoded and sent via IPC.
 * For high-performance writes, prefer writing from C++ directly.
 *
 * @param name   Buffer name.
 * @param data   Data to write (Uint8Array or ArrayBuffer).
 * @param offset Byte offset into the buffer (default 0).
 */
export async function writeBuffer(
  name: string,
  data: Uint8Array | ArrayBuffer,
  offset = 0,
): Promise<{ ok: boolean; bytes_written: number }> {
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  const b64 = uint8ArrayToBase64(bytes);
  return invoke('buffer:write', { name, data: b64, offset });
}

/**
 * Destroy a shared buffer on the backend.
 * After this call, the anyar-shm:// URL will no longer resolve.
 *
 * @param name Buffer name.
 */
export async function destroyBuffer(name: string): Promise<void> {
  await invoke('buffer:destroy', { name });
}

/**
 * List all active shared buffers.
 */
export async function listBuffers(): Promise<SharedBufferInfo[]> {
  const result = await invoke<{ buffers: SharedBufferInfo[] }>('buffer:list');
  return result.buffers;
}

/**
 * Ask the backend to emit a `buffer:ready` event for a specific buffer.
 *
 * @param name     Buffer name.
 * @param metadata Optional metadata to include.
 */
export async function notifyBuffer(
  name: string,
  metadata?: Record<string, unknown>,
): Promise<void> {
  await invoke('buffer:notify', { name, metadata: metadata ?? {} });
}

// ── Fetching buffer data ───────────────────────────────────────────────────

/**
 * True when `anyar-shm://` can be fetched: a native webview whose backend
 * did not opt out.  An absent flag means an older backend → assume yes.
 */
function hasShmScheme(): boolean {
  return isNativeIpc() && window.__LIBANYAR_SHM_SCHEME__ !== false;
}

// ── WebView2 shared buffers (Windows) ──────────────────────────────────────
//
// The backend allocates SharedBuffers as WebView2 shared memory and, on
// `buffer:attach`, posts the buffer into this page (`sharedbufferreceived`).
// The resulting ArrayBuffer IS the producer's memory: reading it costs
// nothing.  Cached per name + generation id; a new generation (buffer
// recreated under the same name) is re-attached.

interface LiveBuffer {
  id: number;
  buffer: ArrayBuffer;
}

interface AttachReply {
  attached: boolean;
  id: number;
  posted: boolean;
}

/** Minimal slice of `window.chrome.webview` used here. */
interface WebView2Bridge {
  addEventListener(type: 'sharedbufferreceived', fn: (e: any) => void): void;
  releaseBuffer?(buffer: ArrayBuffer): void;
}

const liveBuffers = new Map<string, LiveBuffer>();
const liveWaiters = new Map<string, (buf: ArrayBuffer) => void>();
let liveListening = false;
/** Names the backend could not attach (file-mapping buffers) → skip retries. */
const notAttachable = new Map<string, number>();

function webview2(): WebView2Bridge | undefined {
  return (window as any).chrome?.webview as WebView2Bridge | undefined;
}

function hasSharedBuffers(): boolean {
  return isNativeIpc() && window.__LIBANYAR_SHARED_BUFFERS__ === true && !!webview2();
}

function ensureLiveListener(): void {
  if (liveListening) return;
  liveListening = true;
  webview2()!.addEventListener('sharedbufferreceived', (e: any) => {
    const tag = (e && e.additionalData) || {};
    if (typeof tag.name !== 'string') return;  // not ours
    const buf: ArrayBuffer = e.getBuffer();
    const prev = liveBuffers.get(tag.name);
    if (prev && prev.buffer !== buf) {
      try { webview2()!.releaseBuffer?.(prev.buffer); } catch { /* already gone */ }
    }
    liveBuffers.set(tag.name, { id: tag.id, buffer: buf });
    const key = `${tag.name}#${tag.id}`;
    const waiter = liveWaiters.get(key);
    if (waiter) {
      liveWaiters.delete(key);
      waiter(buf);
    }
  });
}

/**
 * The live (zero-copy) ArrayBuffer for @p name, or null if this buffer is
 * not WebView2-backed.  @p id (from a `buffer:ready` payload) lets a cached
 * generation skip the `buffer:attach` round trip.
 */
async function liveBuffer(name: string, id?: number): Promise<ArrayBuffer | null> {
  const cached = liveBuffers.get(name);
  if (cached && id !== undefined && cached.id === id) return cached.buffer;
  if (id !== undefined && notAttachable.get(name) === id) return null;

  ensureLiveListener();
  const reply = await invoke<AttachReply>('buffer:attach', { name, have: cached?.id ?? 0 });
  if (!reply.attached) {
    notAttachable.set(name, reply.id);
    return null;
  }
  if (!reply.posted && cached && cached.id === reply.id) return cached.buffer;

  const hit = liveBuffers.get(name);  // event may already have arrived
  if (hit && hit.id === reply.id) return hit.buffer;
  const key = `${name}#${reply.id}`;
  return new Promise<ArrayBuffer>((resolve, reject) => {
    liveWaiters.set(key, resolve);
    setTimeout(() => {
      if (liveWaiters.delete(key)) reject(new Error(`buffer:attach timed out for ${name}`));
    }, 5000);
  });
}

/** Options for {@link fetchBuffer}. */
export interface FetchBufferOptions {
  /**
   * `true` (default): return a private snapshot (safe to keep).
   * `false`: return the live shared memory when the platform maps it
   * directly (Windows) — no copy at all, but the contents change when the
   * producer writes again.  Use it for per-frame consumers that read
   * immediately and then release the slot (`buffer:pool-release-read`).
   * Elsewhere the result is a fresh fetch either way.
   */
  copy?: boolean;
  /** Generation id from a `buffer:ready` payload — skips revalidation. */
  id?: number;
}

/**
 * Fetch the raw bytes of a shared buffer.
 *
 * - Windows (WebView2): maps the buffer's memory into the page via
 *   `buffer:attach` — zero-copy with `{ copy: false }`, one in-page copy
 *   otherwise.
 * - Linux (WebKitGTK): the `anyar-shm://` URI scheme.
 * - Browser dev mode, or a buffer created before any window existed: an
 *   HTTP GET to the backend (`/__anyar__/buffer/<name>`).
 *
 * @param nameOrUrl  Buffer name or full anyar-shm:// URL.
 * @param options    Copy semantics and generation id (see FetchBufferOptions).
 * @returns          The buffer bytes as an ArrayBuffer.
 * @example
 * const bytes = new Uint8Array(await fetchBuffer('video-frame'));
 * @example
 * // per-frame, zero-copy where supported:
 * onBufferReady(async (e) => {
 *   renderer.drawFrame(await fetchBuffer(e.url, { copy: false, id: e.id }));
 *   await poolReleaseRead(e.pool!, e.name);
 * });
 */
export async function fetchBuffer(
  nameOrUrl: string,
  options: FetchBufferOptions = {},
): Promise<ArrayBuffer> {
  // Extract just the buffer name from a full URL if needed
  const name = nameOrUrl.startsWith('anyar-shm://')
    ? nameOrUrl.slice('anyar-shm://'.length)
    : nameOrUrl;

  if (hasSharedBuffers()) {
    try {
      const live = await liveBuffer(name, options.id);
      if (live) return options.copy === false ? live : live.slice(0);
    } catch {
      // fall back to HTTP below
    }
  }

  const url = hasShmScheme()
    ? `anyar-shm://${name}`
    : `${getBaseUrl()}/__anyar__/buffer/${encodeURIComponent(name)}`;

  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Failed to fetch buffer: ${response.status} ${response.statusText}`);
  }
  return response.arrayBuffer();
}

// ── Event listeners ────────────────────────────────────────────────────────

/**
 * Listen for `buffer:ready` events emitted when a buffer's data is updated.
 *
 * @param handler  Callback receiving the buffer ready event payload.
 * @returns        Function to remove the listener.
 */
export function onBufferReady(
  handler: EventHandler<BufferReadyEvent>,
): UnlistenFn {
  return listen<BufferReadyEvent>('buffer:ready', handler);
}

// ── Pool operations ────────────────────────────────────────────────────────

/**
 * Create a shared buffer pool for streaming use cases.
 *
 * @param name       Base name for the pool (individual buffers: name_0, name_1, ...).
 * @param bufferSize Size of each buffer in bytes.
 * @param count      Number of buffers (default 3).
 * @returns          Pool info with individual buffer details.
 */
export async function createPool(
  name: string,
  bufferSize: number,
  count = 3,
): Promise<SharedBufferPoolInfo> {
  return invoke<SharedBufferPoolInfo>('buffer:pool-create', {
    name,
    bufferSize,
    count,
  });
}

/**
 * Destroy a shared buffer pool and all its buffers.
 *
 * @param name Pool base name.
 */
export async function destroyPool(name: string): Promise<void> {
  await invoke('buffer:pool-destroy', { name });
}

/**
 * Acquire a writable buffer from the pool (C++ producer side).
 * Typically called via C++ directly for performance; this JS wrapper
 * is provided for testing and prototyping.
 *
 * @param name Pool base name.
 * @returns    Info about the acquired buffer.
 */
export async function poolAcquire(
  name: string,
): Promise<SharedBufferInfo> {
  return invoke<SharedBufferInfo>('buffer:pool-acquire', { name });
}

/**
 * Release a written buffer and notify the frontend (C++ producer side).
 *
 * @param pool     Pool base name.
 * @param name     Buffer name to release.
 * @param metadata Metadata to include in the notification.
 */
export async function poolReleaseWrite(
  pool: string,
  name: string,
  metadata?: Record<string, unknown>,
): Promise<void> {
  await invoke('buffer:pool-release-write', {
    pool,
    name,
    metadata: metadata ?? {},
  });
}

/**
 * Release a buffer back to the pool after reading (consumer side).
 * Call this after processing data from a `buffer:ready` event
 * to allow the producer to reuse the buffer slot.
 *
 * @param pool Pool base name.
 * @param name Buffer name to release.
 */
export async function poolReleaseRead(
  pool: string,
  name: string,
): Promise<void> {
  await invoke('buffer:pool-release-read', { pool, name });
}

// ── Utilities ──────────────────────────────────────────────────────────────

/** Convert Uint8Array to base64 string. */
function uint8ArrayToBase64(bytes: Uint8Array): string {
  let binary = '';
  for (let i = 0; i < bytes.length; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary);
}
