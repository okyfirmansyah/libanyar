// ---------------------------------------------------------------------------
// @libanyar/api/window — unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
  createWindow,
  closeWindow,
  closeAll,
  listWindows,
  setTitle,
  setSize,
  focusWindow,
  setEnabled,
  setAlwaysOnTop,
  getLabel,
  onWindowClosed,
  onWindowCreated,
  onWindowFocused,
  setCloseConfirmation,
} from './window';

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

describe('window module', () => {
  beforeEach(() => {
    mockInvoke.mockReset();
    mockListen.mockReset();
    delete (window as any).__LIBANYAR_WINDOW_LABEL__;
  });

  afterEach(() => {
    delete (window as any).__LIBANYAR_WINDOW_LABEL__;
  });

  describe('createWindow', () => {
    it('invokes window:create and returns label', async () => {
      mockInvoke.mockResolvedValue({ label: 'settings' });
      const label = await createWindow({
        label: 'settings',
        title: 'Settings',
        width: 600,
        height: 400,
      });
      expect(label).toBe('settings');
      expect(mockInvoke).toHaveBeenCalledWith('window:create', {
        label: 'settings',
        title: 'Settings',
        width: 600,
        height: 400,
      });
    });
  });

  describe('closeWindow', () => {
    it('closes specified window', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await closeWindow('child');
      expect(mockInvoke).toHaveBeenCalledWith('window:close', {
        label: 'child',
      });
    });

    it('closes current window when no label given', async () => {
      (window as any).__LIBANYAR_WINDOW_LABEL__ = 'my-win';
      mockInvoke.mockResolvedValue(undefined);
      await closeWindow();
      expect(mockInvoke).toHaveBeenCalledWith('window:close', {
        label: 'my-win',
      });
    });

    it('defaults to "main" when no window label set', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await closeWindow();
      expect(mockInvoke).toHaveBeenCalledWith('window:close', {
        label: 'main',
      });
    });
  });

  describe('closeAll', () => {
    it('invokes window:close-all', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await closeAll();
      expect(mockInvoke).toHaveBeenCalledWith('window:close-all', {});
    });
  });

  describe('listWindows', () => {
    it('returns window list', async () => {
      const windows = [{ label: 'main' }, { label: 'settings' }];
      mockInvoke.mockResolvedValue(windows);
      const result = await listWindows();
      expect(result).toEqual(windows);
    });
  });

  describe('setTitle', () => {
    it('invokes window:set-title', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await setTitle('main', 'New Title');
      expect(mockInvoke).toHaveBeenCalledWith('window:set-title', {
        label: 'main',
        title: 'New Title',
      });
    });
  });

  describe('setSize', () => {
    it('invokes window:set-size', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await setSize('main', 1024, 768);
      expect(mockInvoke).toHaveBeenCalledWith('window:set-size', {
        label: 'main',
        width: 1024,
        height: 768,
      });
    });
  });

  describe('focusWindow', () => {
    it('invokes window:focus', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await focusWindow('child');
      expect(mockInvoke).toHaveBeenCalledWith('window:focus', {
        label: 'child',
      });
    });
  });

  describe('setEnabled', () => {
    it('invokes window:set-enabled', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await setEnabled('main', false);
      expect(mockInvoke).toHaveBeenCalledWith('window:set-enabled', {
        label: 'main',
        enabled: false,
      });
    });
  });

  describe('setAlwaysOnTop', () => {
    it('invokes window:set-always-on-top', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await setAlwaysOnTop('main', true);
      expect(mockInvoke).toHaveBeenCalledWith('window:set-always-on-top', {
        label: 'main',
        alwaysOnTop: true,
      });
    });
  });

  describe('getLabel', () => {
    it('returns injected window label', () => {
      (window as any).__LIBANYAR_WINDOW_LABEL__ = 'settings';
      expect(getLabel()).toBe('settings');
    });

    it('returns "main" as default', () => {
      expect(getLabel()).toBe('main');
    });
  });

  describe('onWindowClosed', () => {
    it('subscribes to window:closed event', () => {
      const handler = vi.fn();
      const unlisten = vi.fn();
      mockListen.mockReturnValue(unlisten);

      const result = onWindowClosed(handler);
      expect(mockListen).toHaveBeenCalledWith('window:closed', handler);
      expect(result).toBe(unlisten);
    });
  });

  describe('onWindowCreated', () => {
    it('subscribes to window:created event', () => {
      const handler = vi.fn();
      const unlisten = vi.fn();
      mockListen.mockReturnValue(unlisten);

      const result = onWindowCreated(handler);
      expect(mockListen).toHaveBeenCalledWith('window:created', handler);
      expect(result).toBe(unlisten);
    });
  });

  describe('onWindowFocused', () => {
    it('subscribes to window:focused event', () => {
      const handler = vi.fn();
      const unlisten = vi.fn();
      mockListen.mockReturnValue(unlisten);

      const result = onWindowFocused(handler);
      expect(mockListen).toHaveBeenCalledWith('window:focused', handler);
      expect(result).toBe(unlisten);
    });
  });

  describe('setCloseConfirmation', () => {
    it('invokes window:set-close-confirmation', async () => {
      (window as any).__LIBANYAR_WINDOW_LABEL__ = 'my-win';
      mockInvoke.mockResolvedValue(undefined);
      await setCloseConfirmation({
        enabled: true,
        message: 'Unsaved changes!',
      });
      expect(mockInvoke).toHaveBeenCalledWith(
        'window:set-close-confirmation',
        {
          label: 'my-win',
          enabled: true,
          message: 'Unsaved changes!',
        },
      );
    });

    it('uses explicit label when provided', async () => {
      mockInvoke.mockResolvedValue(undefined);
      await setCloseConfirmation({
        label: 'other',
        enabled: false,
      });
      expect(mockInvoke).toHaveBeenCalledWith(
        'window:set-close-confirmation',
        {
          label: 'other',
          enabled: false,
        },
      );
    });
  });
});
