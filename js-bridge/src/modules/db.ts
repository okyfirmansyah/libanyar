// ---------------------------------------------------------------------------
// @libanyar/api/db — Database commands (SQLite / PostgreSQL via SOCI)
// ---------------------------------------------------------------------------

import { invoke } from '../invoke';

/** A row is a plain key-value record. */
export type Row = Record<string, unknown>;

/** Result of a query (SELECT). */
export interface QueryResult<T extends Row = Row> {
  rows: T[];
  columns: string[];
}

/** Result of an exec (INSERT/UPDATE/DELETE). */
export interface ExecResult {
  affectedRows: number;
}

/**
 * Open (or create) a database connection.
 *
 * @param backend  `"sqlite3"` or `"postgresql"`.
 * @param connStr  Connection string (e.g. `"myapp.db"` for SQLite).
 * @param poolSize Number of connections in the pool (default 4).
 * @returns        A handle ID for subsequent operations.
 */
export async function openDatabase(
  backend: 'sqlite3' | 'postgresql',
  connStr: string,
  poolSize = 4,
): Promise<string> {
  return invoke<string>('db:open', { backend, connStr, poolSize });
}

/**
 * Close a database connection pool.
 *
 * @param handle Handle returned by `openDatabase()`.
 */
export async function closeDatabase(handle: string): Promise<void> {
  await invoke('db:close', { handle });
}

/**
 * Execute a SELECT query and return rows.
 *
 * @param handle Handle returned by `openDatabase()`.
 * @param sql    SQL query string.
 * @param params Positional bind parameters.
 */
export async function query<T extends Row = Row>(
  handle: string,
  sql: string,
  params: unknown[] = [],
): Promise<QueryResult<T>> {
  return invoke<QueryResult<T>>('db:query', { handle, sql, params });
}

/**
 * Execute a non-SELECT statement (INSERT, UPDATE, DELETE).
 */
export async function exec(
  handle: string,
  sql: string,
  params: unknown[] = [],
): Promise<ExecResult> {
  return invoke<ExecResult>('db:exec', { handle, sql, params });
}

/**
 * Execute multiple statements in a single transaction.
 *
 * @param handle     Handle returned by `openDatabase()`.
 * @param statements Array of `{ sql, params }` objects.
 * @returns          Array of `ExecResult` (one per statement).
 */
export async function batch(
  handle: string,
  statements: Array<{ sql: string; params?: unknown[] }>,
): Promise<ExecResult[]> {
  return invoke<ExecResult[]>('db:batch', { handle, statements });
}
