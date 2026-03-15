// ---------------------------------------------------------------------------
// @libanyar/api/db — unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach } from 'vitest';
import { openDatabase, closeDatabase, query, exec, batch } from './db';

vi.mock('../invoke', () => ({
  invoke: vi.fn(),
}));

import { invoke } from '../invoke';
const mockInvoke = vi.mocked(invoke);

describe('db module', () => {
  beforeEach(() => {
    mockInvoke.mockReset();
  });

  it('openDatabase invokes db:open', async () => {
    mockInvoke.mockResolvedValue('handle-1');
    const handle = await openDatabase('sqlite3', 'test.db');
    expect(handle).toBe('handle-1');
    expect(mockInvoke).toHaveBeenCalledWith('db:open', {
      backend: 'sqlite3',
      connStr: 'test.db',
      poolSize: 4,
    });
  });

  it('openDatabase passes custom pool size', async () => {
    mockInvoke.mockResolvedValue('handle-2');
    await openDatabase('postgresql', 'host=localhost dbname=test', 8);
    expect(mockInvoke).toHaveBeenCalledWith('db:open', {
      backend: 'postgresql',
      connStr: 'host=localhost dbname=test',
      poolSize: 8,
    });
  });

  it('closeDatabase invokes db:close', async () => {
    mockInvoke.mockResolvedValue(undefined);
    await closeDatabase('handle-1');
    expect(mockInvoke).toHaveBeenCalledWith('db:close', {
      handle: 'handle-1',
    });
  });

  it('query invokes db:query with params', async () => {
    const qr = {
      rows: [{ id: 1, name: 'Alice' }],
      columns: ['id', 'name'],
    };
    mockInvoke.mockResolvedValue(qr);

    const result = await query('h1', 'SELECT * FROM users WHERE id = ?', [1]);
    expect(result).toEqual(qr);
    expect(mockInvoke).toHaveBeenCalledWith('db:query', {
      handle: 'h1',
      sql: 'SELECT * FROM users WHERE id = ?',
      params: [1],
    });
  });

  it('query defaults to empty params', async () => {
    mockInvoke.mockResolvedValue({ rows: [], columns: [] });
    await query('h1', 'SELECT 1');
    expect(mockInvoke).toHaveBeenCalledWith('db:query', {
      handle: 'h1',
      sql: 'SELECT 1',
      params: [],
    });
  });

  it('exec invokes db:exec', async () => {
    mockInvoke.mockResolvedValue({ affectedRows: 1 });
    const result = await exec('h1', 'INSERT INTO t VALUES (?)', ['val']);
    expect(result.affectedRows).toBe(1);
    expect(mockInvoke).toHaveBeenCalledWith('db:exec', {
      handle: 'h1',
      sql: 'INSERT INTO t VALUES (?)',
      params: ['val'],
    });
  });

  it('batch invokes db:batch with statements', async () => {
    mockInvoke.mockResolvedValue([
      { affectedRows: 1 },
      { affectedRows: 1 },
    ]);
    const result = await batch('h1', [
      { sql: 'INSERT INTO t VALUES (1)' },
      { sql: 'INSERT INTO t VALUES (?)', params: [2] },
    ]);
    expect(result).toHaveLength(2);
    expect(mockInvoke).toHaveBeenCalledWith('db:batch', {
      handle: 'h1',
      statements: [
        { sql: 'INSERT INTO t VALUES (1)' },
        { sql: 'INSERT INTO t VALUES (?)', params: [2] },
      ],
    });
  });
});
