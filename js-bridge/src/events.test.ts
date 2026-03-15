// ---------------------------------------------------------------------------
// @libanyar/api — events.ts unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// We need to mock WebSocket since jsdom doesn't have a real one
class MockWebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;

  readyState = MockWebSocket.CONNECTING;
  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onmessage: ((ev: { data: string }) => void) | null = null;
  onerror: (() => void) | null = null;

  sent: string[] = [];

  constructor(_url: string) {
    // Auto-open after microtask
    setTimeout(() => {
      this.readyState = MockWebSocket.OPEN;
      this.onopen?.();
    }, 0);
    MockWebSocket._instances.push(this);
  }

  send(data: string) {
    this.sent.push(data);
  }

  close() {
    this.readyState = MockWebSocket.CLOSED;
    this.onclose?.();
  }

  // Test helper: simulate incoming message
  _receiveMessage(data: string) {
    this.onmessage?.({ data });
  }

  static _instances: MockWebSocket[] = [];
  static _reset() {
    MockWebSocket._instances = [];
  }
  static get lastInstance(): MockWebSocket | undefined {
    return MockWebSocket._instances[MockWebSocket._instances.length - 1];
  }
}

describe('events', () => {
  beforeEach(() => {
    vi.resetModules();
    vi.useFakeTimers();
    MockWebSocket._reset();
    delete (window as any).__LIBANYAR_NATIVE__;
    delete (window as any).__anyar_ipc__;
    delete (window as any).__anyar_dispatch_event__;
    delete (window as any).__anyar_event_listeners__;

    // Stub WebSocket globally
    vi.stubGlobal('WebSocket', MockWebSocket);
  });

  afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
  });

  describe('listen (WebSocket mode)', () => {
    it('creates WS connection and dispatches events', async () => {
      const { listen } = await import('./events');
      const { setPort } = await import('./config');
      setPort(5555);

      const handler = vi.fn();
      listen('counter:updated', handler);

      // Let the WS connect
      await vi.advanceTimersByTimeAsync(10);

      const ws = MockWebSocket.lastInstance!;
      expect(ws).toBeDefined();

      // Simulate incoming event
      ws._receiveMessage(
        JSON.stringify({
          type: 'event',
          event: 'counter:updated',
          payload: { count: 42 },
        }),
      );

      expect(handler).toHaveBeenCalledWith({ count: 42 });
    });

    it('returns an unlisten function that removes the handler', async () => {
      const { listen } = await import('./events');
      const handler = vi.fn();
      const unlisten = listen('test-event', handler);

      // Let WS connect
      await vi.advanceTimersByTimeAsync(10);

      const ws = MockWebSocket.lastInstance!;
      ws._receiveMessage(
        JSON.stringify({
          type: 'event',
          event: 'test-event',
          payload: 'first',
        }),
      );
      expect(handler).toHaveBeenCalledTimes(1);

      unlisten();

      ws._receiveMessage(
        JSON.stringify({
          type: 'event',
          event: 'test-event',
          payload: 'second',
        }),
      );
      expect(handler).toHaveBeenCalledTimes(1); // still 1
    });

    it('wildcard listener receives all events', async () => {
      const { listen } = await import('./events');
      const handler = vi.fn();
      listen('*', handler);

      await vi.advanceTimersByTimeAsync(10);
      const ws = MockWebSocket.lastInstance!;

      ws._receiveMessage(
        JSON.stringify({ type: 'event', event: 'foo', payload: 1 }),
      );
      ws._receiveMessage(
        JSON.stringify({ type: 'event', event: 'bar', payload: 2 }),
      );

      expect(handler).toHaveBeenCalledTimes(2);
      expect(handler).toHaveBeenCalledWith(1);
      expect(handler).toHaveBeenCalledWith(2);
    });

    it('ignores malformed messages', async () => {
      const { listen } = await import('./events');
      const handler = vi.fn();
      listen('test', handler);

      await vi.advanceTimersByTimeAsync(10);
      const ws = MockWebSocket.lastInstance!;

      // Malformed JSON
      ws._receiveMessage('not json');
      // Missing event field
      ws._receiveMessage(JSON.stringify({ type: 'event' }));

      expect(handler).not.toHaveBeenCalled();
    });
  });

  describe('emit (WebSocket mode)', () => {
    it('sends event over WebSocket when connected', async () => {
      const { listen, emit } = await import('./events');
      // Need to call listen first to establish connection
      listen('dummy', vi.fn());

      await vi.advanceTimersByTimeAsync(10);
      const ws = MockWebSocket.lastInstance!;

      emit('my-event', { data: 'hello' });

      expect(ws.sent).toHaveLength(1);
      const sent = JSON.parse(ws.sent[0]);
      expect(sent.type).toBe('event');
      expect(sent.event).toBe('my-event');
      expect(sent.payload).toEqual({ data: 'hello' });
    });

    it('queues messages when WebSocket is not yet connected', async () => {
      const { emit } = await import('./events');

      // Emit before any connection
      emit('early-event', { v: 1 });
      emit('early-event', { v: 2 });

      // Now let the WS connect
      await vi.advanceTimersByTimeAsync(10);
      const ws = MockWebSocket.lastInstance!;

      // Queued messages should have been flushed
      expect(ws.sent).toHaveLength(2);
    });
  });

  describe('once', () => {
    it('fires handler once then auto-unsubscribes', async () => {
      const { once } = await import('./events');
      const handler = vi.fn();
      once('one-shot', handler);

      await vi.advanceTimersByTimeAsync(10);
      const ws = MockWebSocket.lastInstance!;

      ws._receiveMessage(
        JSON.stringify({ type: 'event', event: 'one-shot', payload: 'a' }),
      );
      ws._receiveMessage(
        JSON.stringify({ type: 'event', event: 'one-shot', payload: 'b' }),
      );

      expect(handler).toHaveBeenCalledTimes(1);
      expect(handler).toHaveBeenCalledWith('a');
    });

    it('returns manual unlisten function', async () => {
      const { once } = await import('./events');
      const handler = vi.fn();
      const unlisten = once('one-shot', handler);

      // Unlisten before any event fires
      unlisten();

      await vi.advanceTimersByTimeAsync(10);
      const ws = MockWebSocket.lastInstance!;

      ws._receiveMessage(
        JSON.stringify({ type: 'event', event: 'one-shot', payload: 'x' }),
      );
      expect(handler).not.toHaveBeenCalled();
    });
  });

  describe('onReady (WebSocket mode)', () => {
    it('fires callback when WS connects', async () => {
      const { onReady } = await import('./events');
      const callback = vi.fn();
      onReady(callback);

      expect(callback).not.toHaveBeenCalled();

      await vi.advanceTimersByTimeAsync(10);
      expect(callback).toHaveBeenCalledOnce();
    });

    it('fires immediately if already connected', async () => {
      const { listen, onReady } = await import('./events');

      // Establish connection first
      listen('dummy', vi.fn());
      await vi.advanceTimersByTimeAsync(10);

      const callback = vi.fn();
      onReady(callback);
      expect(callback).toHaveBeenCalledOnce();
    });
  });

  describe('native mode', () => {
    it('dispatches events via __anyar_dispatch_event__', async () => {
      (window as any).__LIBANYAR_NATIVE__ = true;

      const { listen } = await import('./events');
      const handler = vi.fn();
      listen('native-event', handler);

      // In native mode, the C++ side calls __anyar_dispatch_event__
      const dispatch = (window as any).__anyar_dispatch_event__;
      expect(dispatch).toBeTypeOf('function');

      dispatch({ event: 'native-event', payload: { native: true } });
      expect(handler).toHaveBeenCalledWith({ native: true });
    });

    it('onReady fires immediately in native mode', async () => {
      (window as any).__LIBANYAR_NATIVE__ = true;

      const { onReady } = await import('./events');
      const callback = vi.fn();
      onReady(callback);
      expect(callback).toHaveBeenCalledOnce();
    });

    it('emit uses invoke in native mode', async () => {
      (window as any).__LIBANYAR_NATIVE__ = true;

      // Mock the native IPC function
      const mockIpc = vi.fn().mockResolvedValue({
        id: 'test',
        data: null,
        error: null,
      });
      (window as any).__anyar_ipc__ = mockIpc;

      const { emit } = await import('./events');
      emit('some-event', { value: 42 });

      // Give the async invoke a tick
      await vi.advanceTimersByTimeAsync(0);

      expect(mockIpc).toHaveBeenCalled();
      const call = JSON.parse(mockIpc.mock.calls[0][0]);
      expect(call.cmd).toBe('anyar:emit_event');
      expect(call.args).toEqual({ event: 'some-event', payload: { value: 42 } });
    });
  });

  describe('WebSocket reconnection', () => {
    it('schedules reconnect on close', async () => {
      const { listen } = await import('./events');
      listen('test', vi.fn());

      await vi.advanceTimersByTimeAsync(10);
      const ws1 = MockWebSocket.lastInstance!;

      // Simulate disconnect
      ws1.close();

      // After RECONNECT_MS (1000ms), a new connection should be created
      const countBefore = MockWebSocket._instances.length;
      await vi.advanceTimersByTimeAsync(1000);
      expect(MockWebSocket._instances.length).toBeGreaterThan(countBefore);
    });
  });

  // ── Per-window targeted events ─────────────────────────────────────────

  describe('emitTo', () => {
    it('invokes anyar:emit_to_window via native IPC', async () => {
      (window as any).__LIBANYAR_NATIVE__ = true;

      const mockIpc = vi.fn().mockResolvedValue({
        id: 'test',
        data: null,
        error: null,
      });
      (window as any).__anyar_ipc__ = mockIpc;

      const { emitTo } = await import('./events');
      emitTo('settings', 'refresh:data', { key: 123 });

      await vi.advanceTimersByTimeAsync(0);

      expect(mockIpc).toHaveBeenCalled();
      const call = JSON.parse(mockIpc.mock.calls[0][0]);
      expect(call.cmd).toBe('anyar:emit_to_window');
      expect(call.args).toEqual({
        label: 'settings',
        event: 'refresh:data',
        payload: { key: 123 },
      });
    });
  });

  describe('listenGlobal', () => {
    it('receives targeted events for other windows', async () => {
      (window as any).__LIBANYAR_NATIVE__ = true;
      (window as any).__LIBANYAR_WINDOW_LABEL__ = 'main';

      const mockIpc = vi.fn().mockResolvedValue({
        id: 'test',
        data: null,
        error: null,
      });
      (window as any).__anyar_ipc__ = mockIpc;

      const { listenGlobal } = await import('./events');
      const handler = vi.fn();
      listenGlobal('secret', handler);

      // Dispatch an event targeted at a different window
      const dispatch = (window as any).__anyar_dispatch_event__;
      dispatch({ event: 'secret', payload: { info: 'classified' }, target: 'settings' });

      // Global listener should still receive it
      expect(handler).toHaveBeenCalledWith({ info: 'classified' });
    });

    it('regular listen does NOT receive events targeted at other windows', async () => {
      (window as any).__LIBANYAR_NATIVE__ = true;
      (window as any).__LIBANYAR_WINDOW_LABEL__ = 'main';

      const { listen } = await import('./events');
      const handler = vi.fn();
      listen('secret', handler);

      const dispatch = (window as any).__anyar_dispatch_event__;
      dispatch({ event: 'secret', payload: {}, target: 'settings' });

      // Regular listener should NOT receive events for another window
      expect(handler).not.toHaveBeenCalled();
    });

    it('regular listen receives events targeted at own window', async () => {
      (window as any).__LIBANYAR_NATIVE__ = true;
      (window as any).__LIBANYAR_WINDOW_LABEL__ = 'main';

      const { listen } = await import('./events');
      const handler = vi.fn();
      listen('update', handler);

      const dispatch = (window as any).__anyar_dispatch_event__;
      dispatch({ event: 'update', payload: { v: 1 }, target: 'main' });

      expect(handler).toHaveBeenCalledWith({ v: 1 });
    });

    it('regular listen receives broadcast events (no target)', async () => {
      (window as any).__LIBANYAR_NATIVE__ = true;
      (window as any).__LIBANYAR_WINDOW_LABEL__ = 'main';

      const { listen } = await import('./events');
      const handler = vi.fn();
      listen('broadcast', handler);

      const dispatch = (window as any).__anyar_dispatch_event__;
      dispatch({ event: 'broadcast', payload: { v: 2 } });

      expect(handler).toHaveBeenCalledWith({ v: 2 });
    });

    it('unlisten disables global listener and calls anyar:enable_global_listener', async () => {
      (window as any).__LIBANYAR_NATIVE__ = true;

      const mockIpc = vi.fn().mockResolvedValue({
        id: 'test',
        data: null,
        error: null,
      });
      (window as any).__anyar_ipc__ = mockIpc;

      const { listenGlobal } = await import('./events');
      const handler = vi.fn();
      const unlisten = listenGlobal('test', handler);

      // Unlisten
      unlisten();

      // Wait for async invoke
      await vi.advanceTimersByTimeAsync(0);

      // Should have called enable_global_listener with enabled: false
      const calls = mockIpc.mock.calls.map((c: any) => JSON.parse(c[0]));
      const disableCall = calls.find(
        (c: any) => c.cmd === 'anyar:enable_global_listener' && c.args.enabled === false,
      );
      expect(disableCall).toBeDefined();
    });
  });
});
