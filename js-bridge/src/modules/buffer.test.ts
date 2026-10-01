// ---------------------------------------------------------------------------
// @libanyar/api/buffer — unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
  createBuffer,
  writeBuffer,
  destroyBuffer,
  listBuffers,
  notifyBuffer,
  fetchBuffer,
  onBufferReady,
  createPool,
  destroyPool,
  poolAcquire,
  poolReleaseWrite,
  poolReleaseRead,
} from './buffer';

vi.mock('../invoke', () => ({
  invoke: vi.fn(),
}));

vi.mock('../events', () => ({
  listen: vi.fn(),
}));

vi.mock('../config', () => ({
  isNativeIpc: vi.fn(() => true),
  getBaseUrl: vi.fn(() => 'http://127.0.0.1:3080'),
}));

import { invoke } from '../invoke';
import { listen } from '../events';
import { isNativeIpc, getBaseUrl } from '../config';
const mockInvoke = vi.mocked(invoke);
const mockListen = vi.mocked(listen);
const mockIsNativeIpc = vi.mocked(isNativeIpc);
const mockGetBaseUrl = vi.mocked(getBaseUrl);

describe('buffer module', () => {
  beforeEach(() => {
    mockInvoke.mockReset();
    mockListen.mockReset();
    mockIsNativeIpc.mockReset();
    mockGetBaseUrl.mockReset();
    // Default to native mode
    mockIsNativeIpc.mockReturnValue(true);
    mockGetBaseUrl.mockReturnValue('http://127.0.0.1:3080');
  });

  describe('createBuffer', () => {
    it('invokes buffer:create', async () => {
      const info = { name: 'fb', size: 4096, url: 'anyar-shm://fb' };
      mockInvoke.mockResolvedValue(info);
      const result = await createBuffer('fb', 4096);
      expect(result).toEqual(info);
      expect(mockInvoke).toHaveBeenCalledWith('buffer:create', {
        name: 'fb',
        size: 4096,
      });
    });
  });

  describe('writeBuffer', () => {
    it('sends base64-encoded data via IPC', async () => {
      mockInvoke.mockResolvedValue({ ok: true, bytes_written: 3 });
      const data = new Uint8Array([1, 2, 3]);
      const result = await writeBuffer('fb', data, 0);
      expect(result).toEqual({ ok: true, bytes_written: 3 });

      const call = mockInvoke.mock.calls[0];
      expect(call[0]).toBe('buffer:write');
      const args = call[1] as Record<string, unknown>;
      expect(args.name).toBe('fb');
      expect(args.offset).toBe(0);
      expect(typeof args.data).toBe('string'); // base64
    });

    it('accepts ArrayBuffer input', async () => {
      mockInvoke.mockResolvedValue({ ok: true, bytes_written: 4 });
      const ab = new ArrayBuffer(4);
      new Uint8Array(ab).set([10, 20, 30, 40]);
      await writeBuffer('fb', ab);
      expect(mockInvoke).toHaveBeenCalled();
    });
  });

  describe('destroyBuffer', () => {
    it('invokes buffer:destroy', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await destroyBuffer('fb');
      expect(mockInvoke).toHaveBeenCalledWith('buffer:destroy', {
        name: 'fb',
      });
    });
  });

  describe('listBuffers', () => {
    it('invokes buffer:list and returns array', async () => {
      const buffers = [{ name: 'a', size: 100, url: 'anyar-shm://a' }];
      mockInvoke.mockResolvedValue({ buffers });
      const result = await listBuffers();
      expect(result).toEqual(buffers);
    });
  });

  describe('notifyBuffer', () => {
    it('invokes buffer:notify with metadata', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await notifyBuffer('fb', { frame: 1 });
      expect(mockInvoke).toHaveBeenCalledWith('buffer:notify', {
        name: 'fb',
        metadata: { frame: 1 },
      });
    });

    it('defaults metadata to empty object', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await notifyBuffer('fb');
      expect(mockInvoke).toHaveBeenCalledWith('buffer:notify', {
        name: 'fb',
        metadata: {},
      });
    });
  });

  describe('fetchBuffer', () => {
    describe('native mode (anyar-shm://)', () => {
      beforeEach(() => {
        mockIsNativeIpc.mockReturnValue(true);
      });

      it('fetches from anyar-shm:// URL by name', async () => {
        const mockArrayBuffer = new ArrayBuffer(8);
        const mockResponse = {
          ok: true,
          arrayBuffer: vi.fn().mockResolvedValue(mockArrayBuffer),
        };
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(mockResponse));

        const result = await fetchBuffer('my-buffer');
        expect(globalThis.fetch).toHaveBeenCalledWith('anyar-shm://my-buffer');
        expect(result).toBe(mockArrayBuffer);
      });

      it('extracts name from full anyar-shm:// URL', async () => {
        const mockResponse = {
          ok: true,
          arrayBuffer: vi.fn().mockResolvedValue(new ArrayBuffer(0)),
        };
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(mockResponse));

        await fetchBuffer('anyar-shm://existing-buf');
        expect(globalThis.fetch).toHaveBeenCalledWith(
          'anyar-shm://existing-buf',
        );
      });

      it('throws on fetch failure', async () => {
        vi.stubGlobal(
          'fetch',
          vi.fn().mockResolvedValue({
            ok: false,
            status: 404,
            statusText: 'Not Found',
          }),
        );

        await expect(fetchBuffer('missing')).rejects.toThrow(
          'Failed to fetch buffer: 404 Not Found',
        );
      });
    });

    describe('WebView2 shared buffers (buffer:attach)', () => {
      // One fake chrome.webview for the whole block: the module registers
      // its 'sharedbufferreceived' listener once.
      let onShared: ((e: any) => void) | null = null;
      const releaseBuffer = vi.fn();
      const bytes = (...v: number[]) => new Uint8Array(v).buffer;

      /** Make buffer:attach reply, and deliver the buffer like WebView2 does. */
      function attachReplies(name: string, id: number, buf: ArrayBuffer, attached = true) {
        mockInvoke.mockImplementation(async (cmd: string, args: any) => {
          if (cmd !== 'buffer:attach') return undefined;
          if (!attached) return { attached: false, id, posted: false };
          if (args.have === id) return { attached: true, id, posted: false };
          setTimeout(() => onShared?.({ additionalData: { name, id }, getBuffer: () => buf }), 0);
          return { attached: true, id, posted: true };
        });
      }

      beforeEach(() => {
        mockIsNativeIpc.mockReturnValue(true);
        mockGetBaseUrl.mockReturnValue('http://127.0.0.1:4321');
        mockInvoke.mockReset();
        window.__LIBANYAR_SHARED_BUFFERS__ = true;
        (window as any).chrome = {
          webview: {
            addEventListener: (_t: string, fn: (e: any) => void) => { onShared = fn; },
            releaseBuffer,
          },
        };
        vi.stubGlobal('fetch', vi.fn());
      });

      afterEach(() => {
        delete window.__LIBANYAR_SHARED_BUFFERS__;
        delete window.__LIBANYAR_SHM_SCHEME__;
      });

      it('returns a snapshot copy by default', async () => {
        const live = bytes(1, 2, 3);
        attachReplies('sb-copy', 5, live);
        const got = await fetchBuffer('anyar-shm://sb-copy');
        expect(mockInvoke).toHaveBeenCalledWith('buffer:attach', { name: 'sb-copy', have: 0 });
        expect(got).not.toBe(live);
        expect([...new Uint8Array(got)]).toEqual([1, 2, 3]);
        expect(globalThis.fetch).not.toHaveBeenCalled();
      });

      it('returns the live shared memory with copy:false', async () => {
        const live = bytes(9);
        attachReplies('sb-live', 6, live);
        expect(await fetchBuffer('sb-live', { copy: false })).toBe(live);
      });

      it('skips buffer:attach when the cached generation id matches', async () => {
        const live = bytes(4);
        attachReplies('sb-cached', 7, live);
        await fetchBuffer('sb-cached', { copy: false });
        mockInvoke.mockClear();
        expect(await fetchBuffer('sb-cached', { copy: false, id: 7 })).toBe(live);
        expect(mockInvoke).not.toHaveBeenCalled();
      });

      it('re-attaches a recreated buffer and releases the old one', async () => {
        const first = bytes(1);
        const second = bytes(2);
        attachReplies('sb-gen', 8, first);
        await fetchBuffer('sb-gen', { copy: false });
        attachReplies('sb-gen', 9, second);
        expect(await fetchBuffer('sb-gen', { copy: false, id: 9 })).toBe(second);
        expect(mockInvoke).toHaveBeenLastCalledWith('buffer:attach', { name: 'sb-gen', have: 8 });
        expect(releaseBuffer).toHaveBeenCalledWith(first);
      });

      it('falls back to HTTP for buffers that cannot be attached', async () => {
        window.__LIBANYAR_SHM_SCHEME__ = false;  // as injected on Windows
        attachReplies('sb-http', 11, bytes(0), false);
        const httpBody = new ArrayBuffer(2);
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
          ok: true,
          arrayBuffer: vi.fn().mockResolvedValue(httpBody),
        }));
        expect(await fetchBuffer('sb-http', { id: 11 })).toBe(httpBody);
        expect(globalThis.fetch).toHaveBeenCalledWith(
          'http://127.0.0.1:4321/__anyar__/buffer/sb-http',
        );
        // Known non-attachable generation → no second buffer:attach
        mockInvoke.mockClear();
        await fetchBuffer('sb-http', { id: 11 });
        expect(mockInvoke).not.toHaveBeenCalled();
      });
    });

    describe('native mode without anyar-shm:// (Windows)', () => {
      beforeEach(() => {
        mockIsNativeIpc.mockReturnValue(true);
        mockGetBaseUrl.mockReturnValue('http://127.0.0.1:4321');
        window.__LIBANYAR_SHM_SCHEME__ = false;
      });

      afterEach(() => {
        delete window.__LIBANYAR_SHM_SCHEME__;
      });

      it('falls back to the HTTP endpoint', async () => {
        const mockResponse = {
          ok: true,
          arrayBuffer: vi.fn().mockResolvedValue(new ArrayBuffer(4)),
        };
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(mockResponse));

        await fetchBuffer('anyar-shm://frame_0');
        expect(globalThis.fetch).toHaveBeenCalledWith(
          'http://127.0.0.1:4321/__anyar__/buffer/frame_0',
        );
      });

      it('uses anyar-shm:// when the backend opts in', async () => {
        window.__LIBANYAR_SHM_SCHEME__ = true;
        const mockResponse = {
          ok: true,
          arrayBuffer: vi.fn().mockResolvedValue(new ArrayBuffer(4)),
        };
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(mockResponse));

        await fetchBuffer('frame_0');
        expect(globalThis.fetch).toHaveBeenCalledWith('anyar-shm://frame_0');
      });
    });

    describe('browser dev mode (HTTP fallback)', () => {
      beforeEach(() => {
        mockIsNativeIpc.mockReturnValue(false);
        mockGetBaseUrl.mockReturnValue('http://127.0.0.1:4321');
      });

      it('fetches from HTTP endpoint by name', async () => {
        const mockArrayBuffer = new ArrayBuffer(16);
        const mockResponse = {
          ok: true,
          arrayBuffer: vi.fn().mockResolvedValue(mockArrayBuffer),
        };
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(mockResponse));

        const result = await fetchBuffer('video-frame');
        expect(globalThis.fetch).toHaveBeenCalledWith(
          'http://127.0.0.1:4321/__anyar__/buffer/video-frame',
        );
        expect(result).toBe(mockArrayBuffer);
      });

      it('extracts name from anyar-shm:// URL and uses HTTP', async () => {
        const mockResponse = {
          ok: true,
          arrayBuffer: vi.fn().mockResolvedValue(new ArrayBuffer(0)),
        };
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(mockResponse));

        await fetchBuffer('anyar-shm://my-buf');
        expect(globalThis.fetch).toHaveBeenCalledWith(
          'http://127.0.0.1:4321/__anyar__/buffer/my-buf',
        );
      });

      it('encodes special characters in buffer name', async () => {
        const mockResponse = {
          ok: true,
          arrayBuffer: vi.fn().mockResolvedValue(new ArrayBuffer(0)),
        };
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(mockResponse));

        await fetchBuffer('my buffer/special');
        expect(globalThis.fetch).toHaveBeenCalledWith(
          'http://127.0.0.1:4321/__anyar__/buffer/my%20buffer%2Fspecial',
        );
      });

      it('throws on HTTP failure', async () => {
        vi.stubGlobal(
          'fetch',
          vi.fn().mockResolvedValue({
            ok: false,
            status: 500,
            statusText: 'Internal Server Error',
          }),
        );

        await expect(fetchBuffer('broken')).rejects.toThrow(
          'Failed to fetch buffer: 500 Internal Server Error',
        );
      });
    });
  });

  describe('onBufferReady', () => {
    it('subscribes to buffer:ready event', () => {
      const handler = vi.fn();
      const unlisten = vi.fn();
      mockListen.mockReturnValue(unlisten);

      const result = onBufferReady(handler);
      expect(mockListen).toHaveBeenCalledWith('buffer:ready', handler);
      expect(result).toBe(unlisten);
    });
  });

  describe('pool operations', () => {
    it('createPool invokes buffer:pool-create', async () => {
      const poolInfo = {
        name: 'video',
        bufferSize: 1920 * 1080 * 4,
        count: 3,
        buffers: [],
      };
      mockInvoke.mockResolvedValue(poolInfo);
      const result = await createPool('video', 1920 * 1080 * 4, 3);
      expect(result).toEqual(poolInfo);
      expect(mockInvoke).toHaveBeenCalledWith('buffer:pool-create', {
        name: 'video',
        bufferSize: 1920 * 1080 * 4,
        count: 3,
      });
    });

    it('createPool defaults count to 3', async () => {
      mockInvoke.mockResolvedValue({});
      await createPool('p', 100);
      expect(mockInvoke).toHaveBeenCalledWith('buffer:pool-create', {
        name: 'p',
        bufferSize: 100,
        count: 3,
      });
    });

    it('destroyPool invokes buffer:pool-destroy', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await destroyPool('video');
      expect(mockInvoke).toHaveBeenCalledWith('buffer:pool-destroy', {
        name: 'video',
      });
    });

    it('poolAcquire invokes buffer:pool-acquire', async () => {
      const info = { name: 'video_0', size: 100, url: 'anyar-shm://video_0' };
      mockInvoke.mockResolvedValue(info);
      const result = await poolAcquire('video');
      expect(result).toEqual(info);
      expect(mockInvoke).toHaveBeenCalledWith('buffer:pool-acquire', {
        name: 'video',
      });
    });

    it('poolReleaseWrite invokes buffer:pool-release-write', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await poolReleaseWrite('video', 'video_0', { frame: 1 });
      expect(mockInvoke).toHaveBeenCalledWith('buffer:pool-release-write', {
        pool: 'video',
        name: 'video_0',
        metadata: { frame: 1 },
      });
    });

    it('poolReleaseWrite defaults metadata', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await poolReleaseWrite('video', 'video_0');
      expect(mockInvoke).toHaveBeenCalledWith('buffer:pool-release-write', {
        pool: 'video',
        name: 'video_0',
        metadata: {},
      });
    });

    it('poolReleaseRead invokes buffer:pool-release-read', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await poolReleaseRead('video', 'video_0');
      expect(mockInvoke).toHaveBeenCalledWith('buffer:pool-release-read', {
        pool: 'video',
        name: 'video_0',
      });
    });
  });
});
