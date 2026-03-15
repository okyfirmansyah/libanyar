// ---------------------------------------------------------------------------
// @libanyar/api — config.ts unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { getPort, setPort, getBaseUrl, getWsUrl, isNativeIpc } from './config';

describe('config', () => {
  // Reset module state between tests by re-importing
  // Note: setPort(null as any) is a hack to reset — the module uses a closure
  // We'll use dynamic import + vi.resetModules for proper isolation

  describe('getPort', () => {
    beforeEach(() => {
      vi.resetModules();
      // Clean up globals
      delete (window as any).__LIBANYAR_PORT__;
      // Remove any meta tags
      document
        .querySelectorAll('meta[name="libanyar-port"]')
        .forEach((el) => el.remove());
    });

    it('returns 3080 as the default fallback', async () => {
      const { getPort } = await import('./config');
      expect(getPort()).toBe(3080);
    });

    it('returns window.__LIBANYAR_PORT__ when set', async () => {
      (window as any).__LIBANYAR_PORT__ = 9999;
      const { getPort } = await import('./config');
      expect(getPort()).toBe(9999);
      delete (window as any).__LIBANYAR_PORT__;
    });

    it('reads port from <meta> tag', async () => {
      const meta = document.createElement('meta');
      meta.setAttribute('name', 'libanyar-port');
      meta.setAttribute('content', '4567');
      document.head.appendChild(meta);

      const { getPort } = await import('./config');
      expect(getPort()).toBe(4567);
    });

    it('setPort overrides all other sources', async () => {
      (window as any).__LIBANYAR_PORT__ = 9999;
      const { getPort, setPort } = await import('./config');
      setPort(1234);
      expect(getPort()).toBe(1234);
      delete (window as any).__LIBANYAR_PORT__;
    });

    it('ignores invalid <meta> content', async () => {
      const meta = document.createElement('meta');
      meta.setAttribute('name', 'libanyar-port');
      meta.setAttribute('content', 'not-a-number');
      document.head.appendChild(meta);

      const { getPort } = await import('./config');
      expect(getPort()).toBe(3080);
    });

    it('ignores negative port in <meta> content', async () => {
      const meta = document.createElement('meta');
      meta.setAttribute('name', 'libanyar-port');
      meta.setAttribute('content', '-1');
      document.head.appendChild(meta);

      const { getPort } = await import('./config');
      expect(getPort()).toBe(3080);
    });
  });

  describe('getBaseUrl', () => {
    beforeEach(() => {
      vi.resetModules();
      delete (window as any).__LIBANYAR_PORT__;
    });

    it('returns HTTP URL with resolved port', async () => {
      const { getBaseUrl, setPort } = await import('./config');
      setPort(5000);
      expect(getBaseUrl()).toBe('http://127.0.0.1:5000');
    });

    it('uses default port when nothing is set', async () => {
      const { getBaseUrl } = await import('./config');
      expect(getBaseUrl()).toBe('http://127.0.0.1:3080');
    });
  });

  describe('getWsUrl', () => {
    beforeEach(() => {
      vi.resetModules();
      delete (window as any).__LIBANYAR_PORT__;
    });

    it('returns WebSocket URL with resolved port', async () => {
      const { getWsUrl, setPort } = await import('./config');
      setPort(5000);
      expect(getWsUrl()).toBe('ws://127.0.0.1:5000/__anyar_ws__');
    });
  });

  describe('isNativeIpc', () => {
    afterEach(() => {
      delete (window as any).__anyar_ipc__;
    });

    it('returns false when __anyar_ipc__ is not set', () => {
      expect(isNativeIpc()).toBe(false);
    });

    it('returns true when __anyar_ipc__ is a function', () => {
      (window as any).__anyar_ipc__ = vi.fn();
      expect(isNativeIpc()).toBe(true);
    });

    it('returns false when __anyar_ipc__ is not a function', () => {
      (window as any).__anyar_ipc__ = 'not a function';
      expect(isNativeIpc()).toBe(false);
    });
  });
});
