// ---------------------------------------------------------------------------
// @libanyar/api/shell — unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach } from 'vitest';
import { openUrl, openPath, execute } from './shell';

vi.mock('../invoke', () => ({
  invoke: vi.fn(),
}));

import { invoke } from '../invoke';
const mockInvoke = vi.mocked(invoke);

describe('shell module', () => {
  beforeEach(() => {
    mockInvoke.mockReset();
  });

  it('openUrl invokes shell:openUrl', async () => {
    mockInvoke.mockResolvedValue(undefined);
    await openUrl('https://example.com');
    expect(mockInvoke).toHaveBeenCalledWith('shell:openUrl', {
      url: 'https://example.com',
    });
  });

  it('openPath invokes shell:openPath', async () => {
    mockInvoke.mockResolvedValue(undefined);
    await openPath('/home/user/documents');
    expect(mockInvoke).toHaveBeenCalledWith('shell:openPath', {
      path: '/home/user/documents',
    });
  });

  it('execute invokes shell:execute with args', async () => {
    const output = { code: 0, stdout: 'hello\n', stderr: '' };
    mockInvoke.mockResolvedValue(output);

    const result = await execute('echo', ['hello']);
    expect(result).toEqual(output);
    expect(mockInvoke).toHaveBeenCalledWith('shell:execute', {
      program: 'echo',
      args: ['hello'],
    });
  });

  it('execute passes cwd and env options', async () => {
    mockInvoke.mockResolvedValue({ code: 0, stdout: '', stderr: '' });
    await execute('ls', ['-la'], {
      cwd: '/tmp',
      env: { PATH: '/usr/bin' },
    });
    expect(mockInvoke).toHaveBeenCalledWith('shell:execute', {
      program: 'ls',
      args: ['-la'],
      cwd: '/tmp',
      env: { PATH: '/usr/bin' },
    });
  });

  it('execute defaults to empty args', async () => {
    mockInvoke.mockResolvedValue({ code: 0, stdout: '', stderr: '' });
    await execute('whoami');
    expect(mockInvoke).toHaveBeenCalledWith('shell:execute', {
      program: 'whoami',
      args: [],
    });
  });
});
