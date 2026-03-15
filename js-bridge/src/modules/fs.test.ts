// ---------------------------------------------------------------------------
// @libanyar/api/fs — unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach } from 'vitest';
import {
  readFile,
  writeFile,
  readDir,
  exists,
  mkdir,
  remove,
  metadata,
} from './fs';

vi.mock('../invoke', () => ({
  invoke: vi.fn(),
}));

import { invoke } from '../invoke';
const mockInvoke = vi.mocked(invoke);

describe('fs module', () => {
  beforeEach(() => {
    mockInvoke.mockReset();
  });

  it('readFile invokes fs:readFile', async () => {
    mockInvoke.mockResolvedValue('file content');
    const result = await readFile('/tmp/test.txt');
    expect(result).toBe('file content');
    expect(mockInvoke).toHaveBeenCalledWith('fs:readFile', {
      path: '/tmp/test.txt',
      encoding: 'utf-8',
    });
  });

  it('readFile passes encoding option', async () => {
    mockInvoke.mockResolvedValue('base64data');
    await readFile('/tmp/img.png', { encoding: 'base64' });
    expect(mockInvoke).toHaveBeenCalledWith('fs:readFile', {
      path: '/tmp/img.png',
      encoding: 'base64',
    });
  });

  it('writeFile invokes fs:writeFile', async () => {
    mockInvoke.mockResolvedValue(undefined);
    await writeFile('/tmp/out.txt', 'hello');
    expect(mockInvoke).toHaveBeenCalledWith('fs:writeFile', {
      path: '/tmp/out.txt',
      content: 'hello',
    });
  });

  it('readDir invokes fs:readDir', async () => {
    const entries = [
      { name: 'file.txt', isDirectory: false, isFile: true },
      { name: 'subdir', isDirectory: true, isFile: false },
    ];
    mockInvoke.mockResolvedValue(entries);
    const result = await readDir('/tmp');
    expect(result).toEqual(entries);
    expect(mockInvoke).toHaveBeenCalledWith('fs:readDir', { path: '/tmp' });
  });

  it('exists invokes fs:exists', async () => {
    mockInvoke.mockResolvedValue(true);
    const result = await exists('/tmp/test.txt');
    expect(result).toBe(true);
    expect(mockInvoke).toHaveBeenCalledWith('fs:exists', {
      path: '/tmp/test.txt',
    });
  });

  it('mkdir invokes fs:mkdir', async () => {
    mockInvoke.mockResolvedValue(undefined);
    await mkdir('/tmp/newdir');
    expect(mockInvoke).toHaveBeenCalledWith('fs:mkdir', {
      path: '/tmp/newdir',
    });
  });

  it('remove invokes fs:remove with recursive flag', async () => {
    mockInvoke.mockResolvedValue(undefined);
    await remove('/tmp/olddir', true);
    expect(mockInvoke).toHaveBeenCalledWith('fs:remove', {
      path: '/tmp/olddir',
      recursive: true,
    });
  });

  it('remove defaults recursive to false', async () => {
    mockInvoke.mockResolvedValue(undefined);
    await remove('/tmp/file.txt');
    expect(mockInvoke).toHaveBeenCalledWith('fs:remove', {
      path: '/tmp/file.txt',
      recursive: false,
    });
  });

  it('metadata invokes fs:metadata', async () => {
    const meta = {
      size: 1024,
      isDirectory: false,
      isFile: true,
      modifiedTime: '2026-01-01T00:00:00Z',
    };
    mockInvoke.mockResolvedValue(meta);
    const result = await metadata('/tmp/test.txt');
    expect(result).toEqual(meta);
    expect(mockInvoke).toHaveBeenCalledWith('fs:metadata', {
      path: '/tmp/test.txt',
    });
  });
});
