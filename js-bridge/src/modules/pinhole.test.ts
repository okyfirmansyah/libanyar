// ---------------------------------------------------------------------------
// @libanyar/api/pinhole — unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach } from 'vitest';
import {
  onPinholeMounted,
  updatePinholeRect,
  setPinholeVisible,
  getPinholeMetrics,
} from './pinhole';
import type { PinholeRect, PinholeMetrics } from './pinhole';

vi.mock('../invoke', () => ({
  invoke: vi.fn(),
}));

import { invoke } from '../invoke';
const mockInvoke = vi.mocked(invoke);

// ── Setup ──────────────────────────────────────────────────────────────────

beforeEach(() => {
  mockInvoke.mockReset();
  // Simulate the window globals injected by C++
  (window as any).__LIBANYAR_WINDOW_LABEL__ = 'main';
  // NOTE: do NOT reset __anyar_pinhole_mounted__ — it is set once at module
  // import time and must remain as the function that dispatches to the
  // _mountedListeners Map.
});

// ── onPinholeMounted ───────────────────────────────────────────────────────

describe('onPinholeMounted', () => {
  it('registers a callback and calls it when __anyar_pinhole_mounted__ is invoked', () => {
    const fn = vi.fn();
    onPinholeMounted('video-overlay', fn);

    // Simulate the injected JS bootstrap calling the mount hook
    (window as any).__anyar_pinhole_mounted__('video-overlay');
    expect(fn).toHaveBeenCalledOnce();
  });

  it('returns an unlisten function that removes the callback', () => {
    const fn = vi.fn();
    const unlisten = onPinholeMounted('rect-overlay', fn);
    unlisten();

    (window as any).__anyar_pinhole_mounted__('rect-overlay');
    expect(fn).not.toHaveBeenCalled();
  });

  it('supports multiple listeners for the same id', () => {
    const fn1 = vi.fn();
    const fn2 = vi.fn();
    onPinholeMounted('shared', fn1);
    onPinholeMounted('shared', fn2);

    (window as any).__anyar_pinhole_mounted__('shared');
    expect(fn1).toHaveBeenCalledOnce();
    expect(fn2).toHaveBeenCalledOnce();
  });

  it('does not call listener for a different id', () => {
    const fn = vi.fn();
    onPinholeMounted('id-a', fn);

    (window as any).__anyar_pinhole_mounted__('id-b');
    expect(fn).not.toHaveBeenCalled();
  });
});

// ── updatePinholeRect ──────────────────────────────────────────────────────

describe('updatePinholeRect', () => {
  it('sends pinhole:update_rect with correct args', async () => {
    mockInvoke.mockResolvedValue({});
    const rect: PinholeRect = { x: 10, y: 20, width: 400, height: 300 };
    await updatePinholeRect('cam-view', rect);

    expect(mockInvoke).toHaveBeenCalledWith('pinhole:update_rect', {
      id: 'cam-view',
      window_label: 'main',
      x: 10,
      y: 20,
      width: 400,
      height: 300,
      dpr: expect.any(Number),
    });
  });

  it('uses provided window label when supplied', async () => {
    mockInvoke.mockResolvedValue({});
    await updatePinholeRect('hud', { x: 0, y: 0, width: 640, height: 480 }, 'secondary');

    const call = mockInvoke.mock.calls[0][1] as Record<string, unknown>;
    expect(call['window_label']).toBe('secondary');
  });

  it('falls back to "main" when no window label in globals', async () => {
    mockInvoke.mockResolvedValue({});
    delete (window as any).__LIBANYAR_WINDOW_LABEL__;
    await updatePinholeRect('hud', { x: 0, y: 0, width: 100, height: 100 });

    const call = mockInvoke.mock.calls[0][1] as Record<string, unknown>;
    expect(call['window_label']).toBe('main');
  });
});

// ── setPinholeVisible ──────────────────────────────────────────────────────

describe('setPinholeVisible', () => {
  it('sends pinhole:set_visible with visible=true', async () => {
    mockInvoke.mockResolvedValue({});
    await setPinholeVisible('cam-view', true);

    expect(mockInvoke).toHaveBeenCalledWith('pinhole:set_visible', {
      id: 'cam-view',
      window_label: 'main',
      visible: true,
    });
  });

  it('sends pinhole:set_visible with visible=false (scroll-hide)', async () => {
    mockInvoke.mockResolvedValue({});
    await setPinholeVisible('cam-view', false);

    const call = mockInvoke.mock.calls[0][1] as Record<string, unknown>;
    expect(call['visible']).toBe(false);
  });

  it('accepts an explicit window label', async () => {
    mockInvoke.mockResolvedValue({});
    await setPinholeVisible('hud', false, 'child');

    const call = mockInvoke.mock.calls[0][1] as Record<string, unknown>;
    expect(call['window_label']).toBe('child');
  });
});

// ── getPinholeMetrics ──────────────────────────────────────────────────────

describe('getPinholeMetrics', () => {
  it('returns metrics from pinhole:get_metrics', async () => {
    const metrics: PinholeMetrics = {
      ok: true,
      id: 'video-overlay',
      is_native: true,
    };
    mockInvoke.mockResolvedValue(metrics);

    const result = await getPinholeMetrics('video-overlay');
    expect(result.ok).toBe(true);
    expect(result.is_native).toBe(true);
    expect(result.id).toBe('video-overlay');
  });

  it('invokes with correct command and window label', async () => {
    mockInvoke.mockResolvedValue({ ok: false, id: 'x', is_native: false });
    await getPinholeMetrics('x', 'popup');

    expect(mockInvoke).toHaveBeenCalledWith('pinhole:get_metrics', {
      id: 'x',
      window_label: 'popup',
    });
  });
});

// ── Type contract ──────────────────────────────────────────────────────────

describe('PinholeRect type', () => {
  it('accepts a complete rect object', () => {
    const r: PinholeRect = { x: 0, y: 0, width: 1920, height: 1080 };
    expect(r.width).toBe(1920);
  });
});

describe('PinholeMetrics type', () => {
  it('includes optional error field', () => {
    const m: PinholeMetrics = {
      ok: false,
      id: 'foo',
      is_native: false,
      error: 'GL context creation failed',
    };
    expect(m.error).toBeTruthy();
  });
});
