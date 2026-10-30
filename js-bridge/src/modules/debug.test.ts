// ---------------------------------------------------------------------------
// @libanyar/api/debug — unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach } from 'vitest';
import { info, screenshot, click, moveTo, mouse, scroll, key, typeText, exit, targetPoint } from './debug';

vi.mock('../invoke', () => ({ invoke: vi.fn() }));

import { invoke } from '../invoke';
const mockInvoke = vi.mocked(invoke);

describe('debug', () => {
  beforeEach(() => {
    mockInvoke.mockReset();
    mockInvoke.mockResolvedValue({ ok: true });
  });

  it('info → debug:info', async () => {
    mockInvoke.mockResolvedValueOnce({ platform: 'linux', capture: true });
    expect((await info()).platform).toBe('linux');
    expect(mockInvoke).toHaveBeenCalledWith('debug:info');
  });

  it('screenshot passes options through', async () => {
    await screenshot({ path: 'a.png' });
    expect(mockInvoke).toHaveBeenCalledWith('debug:screenshot', { path: 'a.png' });
  });

  it('click resolves a point target', async () => {
    await click({ x: 10, y: 20 }, { button: 3 });
    expect(mockInvoke).toHaveBeenCalledWith('debug:input', { type: 'click', x: 10, y: 20, button: 3 });
  });

  it('click resolves a selector to the element centre', async () => {
    const el = document.createElement('button');
    el.id = 'b';
    el.getBoundingClientRect = () => ({ left: 100, top: 50, width: 40, height: 20 }) as DOMRect;
    document.body.appendChild(el);
    await click('#b');
    expect(mockInvoke).toHaveBeenCalledWith('debug:input', { type: 'click', x: 120, y: 60 });
    el.remove();
  });

  it('targetPoint throws for a missing element', () => {
    expect(() => targetPoint('#nope')).toThrow(/no element/);
  });

  it('moveTo, mouse, scroll, key, typeText map to debug:input', async () => {
    await moveTo({ x: 1, y: 2 });
    await mouse('down', { x: 1, y: 2 });
    await scroll({ x: 1, y: 2 }, 3);
    await key('Enter', { ctrl: true });
    await typeText('hi');
    expect(mockInvoke.mock.calls.map((c) => (c[1] as { type: string }).type)).toEqual([
      'move', 'down', 'scroll', 'key', 'text',
    ]);
    expect(mockInvoke).toHaveBeenCalledWith('debug:input', { type: 'scroll', x: 1, y: 2, dx: 0, dy: 3 });
    expect(mockInvoke).toHaveBeenCalledWith('debug:input', { type: 'key', key: 'Enter', ctrl: true });
  });

  it('exit sends the code', async () => {
    await exit(3);
    expect(mockInvoke).toHaveBeenCalledWith('debug:exit', { code: 3 });
  });
});
