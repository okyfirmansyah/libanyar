// ---------------------------------------------------------------------------
// @libanyar/api/buffer — Shared Memory Buffer module
//
// Provides zero-copy (or near-zero-copy) binary data transfer between the
// C++ backend and the webview frontend via shared memory.
//
// On native (webview): uses anyar-shm:// custom URI scheme (zero-copy fetch)
// On dev mode (browser): falls back to IPC invoke with base64 encoding
// ---------------------------------------------------------------------------

import { invoke } from '../invoke';
import { listen } from '../events';
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
}

/** Payload for the `buffer:ready` event. */
export interface BufferReadyEvent {
  /** Buffer name. */
  name: string;
  /** Pool name (if buffer belongs to a pool). */
  pool?: string;
  /** URI for zero-copy fetch. */
  url: string;
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
 * Fetch the raw bytes of a shared buffer.
 *
 * In native mode, this uses the `anyar-shm://` URI scheme for zero-copy
 * access to the mmap'd shared memory. In dev mode, it falls back to
 * fetching via HTTP.
 *
 * @param nameOrUrl  Buffer name or full anyar-shm:// URL.
 * @returns          The raw buffer data as an ArrayBuffer.
 */
export async function fetchBuffer(nameOrUrl: string): Promise<ArrayBuffer> {
  const url = nameOrUrl.startsWith('anyar-shm://')
    ? nameOrUrl
    : `anyar-shm://${nameOrUrl}`;

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
