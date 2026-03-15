// ---------------------------------------------------------------------------
// @libanyar/api/react — React hooks unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import React from 'react';
import { renderHook, act, waitFor } from '@testing-library/react';

// Mock invoke and events before importing hooks
vi.mock('./invoke', () => ({
  invoke: vi.fn(),
}));

vi.mock('./events', () => ({
  listen: vi.fn(),
}));

import { invoke } from './invoke';
import { listen } from './events';
import { useInvoke, useEvent, useEventCallback } from './react';

const mockInvoke = vi.mocked(invoke);
const mockListen = vi.mocked(listen);

describe('React hooks', () => {
  beforeEach(() => {
    mockInvoke.mockReset();
    mockListen.mockReset();
  });

  describe('useInvoke', () => {
    it('fetches data on mount', async () => {
      mockInvoke.mockResolvedValue({ version: '1.0' });

      const { result } = renderHook(() =>
        useInvoke<{ version: string }>('get_info'),
      );

      // Initially loading
      expect(result.current.loading).toBe(true);
      expect(result.current.data).toBeNull();
      expect(result.current.error).toBeNull();

      // Wait for the async invoke to resolve
      await waitFor(() => {
        expect(result.current.loading).toBe(false);
      });

      expect(result.current.data).toEqual({ version: '1.0' });
      expect(result.current.error).toBeNull();
      expect(mockInvoke).toHaveBeenCalledWith('get_info', {});
    });

    it('sets error on failure', async () => {
      mockInvoke.mockRejectedValue(new Error('Network error'));

      const { result } = renderHook(() => useInvoke('fail_cmd'));

      await waitFor(() => {
        expect(result.current.loading).toBe(false);
      });

      expect(result.current.data).toBeNull();
      expect(result.current.error).toBeInstanceOf(Error);
      expect(result.current.error!.message).toBe('Network error');
    });

    it('re-invokes when cmd changes', async () => {
      mockInvoke.mockResolvedValue('result-1');

      const { result, rerender } = renderHook(
        ({ cmd }) => useInvoke<string>(cmd),
        { initialProps: { cmd: 'cmd-a' } },
      );

      await waitFor(() => {
        expect(result.current.loading).toBe(false);
      });
      expect(mockInvoke).toHaveBeenCalledWith('cmd-a', {});

      mockInvoke.mockResolvedValue('result-2');
      rerender({ cmd: 'cmd-b' });

      await waitFor(() => {
        expect(result.current.data).toBe('result-2');
      });
      expect(mockInvoke).toHaveBeenCalledWith('cmd-b', {});
    });

    it('re-invokes when args change', async () => {
      mockInvoke.mockResolvedValue('hello Alice');

      const { result, rerender } = renderHook(
        ({ args }) => useInvoke<string>('greet', args),
        { initialProps: { args: { name: 'Alice' } as Record<string, unknown> } },
      );

      await waitFor(() => {
        expect(result.current.loading).toBe(false);
      });

      mockInvoke.mockResolvedValue('hello Bob');
      rerender({ args: { name: 'Bob' } });

      await waitFor(() => {
        expect(result.current.data).toBe('hello Bob');
      });
    });

    it('refetch re-invokes the command', async () => {
      let callCount = 0;
      mockInvoke.mockImplementation(async () => {
        callCount++;
        return `result-${callCount}`;
      });

      const { result } = renderHook(() => useInvoke<string>('counter'));

      await waitFor(() => {
        expect(result.current.loading).toBe(false);
      });
      expect(result.current.data).toBe('result-1');

      act(() => {
        result.current.refetch();
      });

      await waitFor(() => {
        expect(result.current.data).toBe('result-2');
      });
    });

    it('converts non-Error rejections to Error', async () => {
      mockInvoke.mockRejectedValue('string error');

      const { result } = renderHook(() => useInvoke('fail'));

      await waitFor(() => {
        expect(result.current.loading).toBe(false);
      });

      expect(result.current.error).toBeInstanceOf(Error);
      expect(result.current.error!.message).toBe('string error');
    });
  });

  describe('useEvent', () => {
    it('subscribes to event and returns latest payload', async () => {
      let capturedHandler: ((payload: any) => void) | null = null;
      const unlisten = vi.fn();
      mockListen.mockImplementation((event, handler) => {
        capturedHandler = handler as any;
        return unlisten;
      });

      const { result } = renderHook(() =>
        useEvent<{ count: number }>('counter:updated'),
      );

      expect(result.current).toBeNull();
      expect(mockListen).toHaveBeenCalledWith(
        'counter:updated',
        expect.any(Function),
      );

      // Simulate event
      act(() => {
        capturedHandler!({ count: 5 });
      });

      expect(result.current).toEqual({ count: 5 });

      // Another event
      act(() => {
        capturedHandler!({ count: 10 });
      });
      expect(result.current).toEqual({ count: 10 });
    });

    it('unsubscribes on unmount', () => {
      const unlisten = vi.fn();
      mockListen.mockReturnValue(unlisten);

      const { unmount } = renderHook(() => useEvent('test'));
      expect(unlisten).not.toHaveBeenCalled();

      unmount();
      expect(unlisten).toHaveBeenCalledOnce();
    });

    it('resubscribes when event name changes', () => {
      const unlisten1 = vi.fn();
      const unlisten2 = vi.fn();
      mockListen
        .mockReturnValueOnce(unlisten1)
        .mockReturnValueOnce(unlisten2);

      const { rerender } = renderHook(
        ({ event }) => useEvent(event),
        { initialProps: { event: 'event-a' } },
      );

      expect(mockListen).toHaveBeenCalledWith('event-a', expect.any(Function));

      rerender({ event: 'event-b' });

      expect(unlisten1).toHaveBeenCalledOnce();
      expect(mockListen).toHaveBeenCalledWith('event-b', expect.any(Function));
    });
  });

  describe('useEventCallback', () => {
    it('calls handler without causing re-renders', () => {
      let capturedHandler: ((payload: any) => void) | null = null;
      mockListen.mockImplementation((_event, handler) => {
        capturedHandler = handler as any;
        return vi.fn();
      });

      const handler = vi.fn();
      const { result } = renderHook(() =>
        useEventCallback('notification', handler),
      );

      // Simulate event
      act(() => {
        capturedHandler!({ text: 'hello' });
      });

      expect(handler).toHaveBeenCalledWith({ text: 'hello' });
    });

    it('uses latest handler ref', () => {
      let capturedHandler: ((payload: any) => void) | null = null;
      mockListen.mockImplementation((_event, handler) => {
        capturedHandler = handler as any;
        return vi.fn();
      });

      const handler1 = vi.fn();
      const handler2 = vi.fn();

      const { rerender } = renderHook(
        ({ handler }) => useEventCallback('test', handler),
        { initialProps: { handler: handler1 } },
      );

      // Update handler
      rerender({ handler: handler2 });

      // Trigger event — should use handler2
      act(() => {
        capturedHandler!('payload');
      });

      expect(handler1).not.toHaveBeenCalled();
      expect(handler2).toHaveBeenCalledWith('payload');
    });

    it('unsubscribes on unmount', () => {
      const unlisten = vi.fn();
      mockListen.mockReturnValue(unlisten);

      const { unmount } = renderHook(() =>
        useEventCallback('test', vi.fn()),
      );

      unmount();
      expect(unlisten).toHaveBeenCalledOnce();
    });
  });
});
