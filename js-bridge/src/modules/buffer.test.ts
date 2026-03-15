// ---------------------------------------------------------------------------
// @libanyar/api/buffer — unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach } from 'vitest';
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

import { invoke } from '../invoke';
import { listen } from '../events';
const mockInvoke = vi.mocked(invoke);
const mockListen = vi.mocked(listen);

describe('buffer module', () => {
  beforeEach(() => {
    mockInvoke.mockReset();
    mockListen.mockReset();
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
    it('fetches from anyar-shm:// URL', async () => {
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

    it('passes through full anyar-shm:// URLs', async () => {
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
