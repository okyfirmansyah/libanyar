// ---------------------------------------------------------------------------
// @libanyar/api — invoke.ts unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

describe('invoke', () => {
  beforeEach(() => {
    vi.resetModules();
    delete (window as any).__anyar_ipc__;
    delete (window as any).__LIBANYAR_PORT__;
  });

  afterEach(() => {
    vi.restoreAllMocks();
    delete (window as any).__anyar_ipc__;
  });

  describe('native IPC path', () => {
    it('calls __anyar_ipc__ when available', async () => {
      const mockIpc = vi.fn().mockResolvedValue({
        id: 'test',
        data: { greeting: 'Hello!' },
        error: null,
      });
      (window as any).__anyar_ipc__ = mockIpc;

      const { invoke } = await import('./invoke');
      const result = await invoke<{ greeting: string }>('greet', {
        name: 'World',
      });

      expect(result).toEqual({ greeting: 'Hello!' });
      expect(mockIpc).toHaveBeenCalledOnce();

      // Verify the payload shape
      const call = JSON.parse(mockIpc.mock.calls[0][0]);
      expect(call.cmd).toBe('greet');
      expect(call.args).toEqual({ name: 'World' });
      expect(call.id).toMatch(/^anyar_/);
    });

    it('throws on native IPC error response', async () => {
      const mockIpc = vi.fn().mockResolvedValue({
        id: 'test',
        data: null,
        error: { code: 404, message: 'Command not found' },
      });
      (window as any).__anyar_ipc__ = mockIpc;

      const { invoke } = await import('./invoke');
      await expect(invoke('unknown')).rejects.toThrow('Command not found');

      try {
        await invoke('unknown');
      } catch (e: any) {
        expect(e.code).toBe(404);
      }
    });

    it('sends empty args object by default', async () => {
      const mockIpc = vi.fn().mockResolvedValue({
        id: 'test',
        data: null,
        error: null,
      });
      (window as any).__anyar_ipc__ = mockIpc;

      const { invoke } = await import('./invoke');
      await invoke('ping');

      const call = JSON.parse(mockIpc.mock.calls[0][0]);
      expect(call.args).toEqual({});
    });
  });

  describe('HTTP fallback path', () => {
    it('POSTs to /__anyar__/invoke when no native IPC', async () => {
      const mockFetch = vi.fn().mockResolvedValue({
        ok: true,
        json: () =>
          Promise.resolve({
            id: 'test',
            data: { version: '0.1.0' },
            error: null,
          }),
      });
      vi.stubGlobal('fetch', mockFetch);

      const { invoke } = await import('./invoke');
      const { setPort } = await import('./config');
      setPort(8080);

      const result = await invoke<{ version: string }>('get_info');

      expect(result).toEqual({ version: '0.1.0' });
      expect(mockFetch).toHaveBeenCalledOnce();
      expect(mockFetch.mock.calls[0][0]).toBe(
        'http://127.0.0.1:8080/__anyar__/invoke',
      );

      const fetchOpts = mockFetch.mock.calls[0][1];
      expect(fetchOpts.method).toBe('POST');
      expect(fetchOpts.headers['Content-Type']).toBe('application/json');

      const body = JSON.parse(fetchOpts.body);
      expect(body.cmd).toBe('get_info');
      expect(body.args).toEqual({});
    });

    it('throws on non-ok HTTP response', async () => {
      const mockFetch = vi.fn().mockResolvedValue({
        ok: false,
        status: 500,
        statusText: 'Internal Server Error',
      });
      vi.stubGlobal('fetch', mockFetch);

      const { invoke } = await import('./invoke');

      await expect(invoke('fail')).rejects.toThrow(
        'LibAnyar IPC HTTP error: 500 Internal Server Error',
      );
    });

    it('throws on HTTP error response body', async () => {
      const mockFetch = vi.fn().mockResolvedValue({
        ok: true,
        json: () =>
          Promise.resolve({
            id: 'test',
            data: null,
            error: { code: 400, message: 'Bad arguments' },
          }),
      });
      vi.stubGlobal('fetch', mockFetch);

      const { invoke } = await import('./invoke');

      await expect(invoke('bad_cmd')).rejects.toThrow('Bad arguments');
    });
  });

  describe('id generation', () => {
    it('generates unique IDs for each call', async () => {
      const ids: string[] = [];
      const mockIpc = vi.fn().mockImplementation((json: string) => {
        const parsed = JSON.parse(json);
        ids.push(parsed.id);
        return Promise.resolve({ id: parsed.id, data: null, error: null });
      });
      (window as any).__anyar_ipc__ = mockIpc;

      const { invoke } = await import('./invoke');
      await invoke('a');
      await invoke('b');
      await invoke('c');

      expect(new Set(ids).size).toBe(3);
      ids.forEach((id) => expect(id).toMatch(/^anyar_\d+_\d+$/));
    });
  });
});
