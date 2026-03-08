# Database Integration

LibAnyar includes a built-in database plugin (`DbPlugin`) that provides fiber-safe SQLite and PostgreSQL access through IPC commands.

## Quick Start

### Frontend (TypeScript)

```ts
import { db } from '@libanyar/api';

// Open a SQLite database
const handle = await db.openDatabase({
  backend: 'sqlite3',
  connStr: 'myapp.db'
});

// Create a table
await db.exec({
  handle,
  sql: 'CREATE TABLE IF NOT EXISTS tasks (id INTEGER PRIMARY KEY, title TEXT, done INTEGER DEFAULT 0)'
});

// Insert data
await db.exec({
  handle,
  sql: 'INSERT INTO tasks (title) VALUES ($1)',
  params: ['Buy groceries']
});

// Query data
const { rows } = await db.query({
  handle,
  sql: 'SELECT * FROM tasks WHERE done = $1',
  params: [0]
});

console.log(rows);
// [{ id: 1, title: "Buy groceries", done: 0 }]
```

### C++ (Direct Usage)

If you need database access from C++ (e.g., inside a plugin), use the LibAsyik SOCI pool directly:

```cpp
#include <libasyik/sql.hpp>

// In your plugin's initialize():
auto pool = asyik::make_sql_pool(ctx.service, "sqlite3", "myapp.db", 4);
auto session = pool->get_session();

*session << "CREATE TABLE IF NOT EXISTS tasks (id INTEGER PRIMARY KEY, title TEXT)";
*session << "INSERT INTO tasks (title) VALUES (:t)", soci::use(std::string("Hello"));
```

## IPC Commands Reference

### `db:open`

Open a database connection pool.

| Argument | Type | Required | Description |
|----------|------|----------|-------------|
| `backend` | `string` | ✅ | `"sqlite3"` or `"postgresql"` |
| `connStr` | `string` | ✅ | Connection string (see below) |
| `poolSize` | `int` | | Pool size, 1–32 (default: 4) |

**Returns:** `string` — opaque handle for subsequent commands.

```js
// SQLite (file-based)
const handle = await db.openDatabase({ backend: 'sqlite3', connStr: 'data.db' });

// SQLite (in-memory)
const handle = await db.openDatabase({ backend: 'sqlite3', connStr: ':memory:' });

// PostgreSQL
const handle = await db.openDatabase({
  backend: 'postgresql',
  connStr: 'host=localhost dbname=myapp user=app password=secret',
  poolSize: 8
});
```

### `db:close`

Close a database connection pool and release resources.

```js
await db.closeDatabase({ handle });
```

### `db:query`

Execute a SELECT query and return rows.

| Argument | Type | Required | Description |
|----------|------|----------|-------------|
| `handle` | `string` | ✅ | Database handle from `db:open` |
| `sql` | `string` | ✅ | SQL query |
| `params` | `array` | | Positional parameters (`$1`, `$2`, ...) |

**Returns:** `{ rows: object[], columns: string[] }`

```js
const { rows, columns } = await db.query({
  handle,
  sql: 'SELECT id, title FROM tasks WHERE done = $1 ORDER BY id',
  params: [0]
});
// rows: [{ id: 1, title: "Buy groceries" }, { id: 2, title: "Walk dog" }]
// columns: ["id", "title"]
```

### `db:exec`

Execute an INSERT, UPDATE, DELETE, or DDL statement.

**Returns:** `{ affectedRows: int }`

```js
const { affectedRows } = await db.exec({
  handle,
  sql: 'UPDATE tasks SET done = 1 WHERE id = $1',
  params: [1]
});
// affectedRows: 1
```

### `db:batch`

Execute multiple statements in a single transaction. If any statement fails, the entire batch is rolled back.

| Argument | Type | Required | Description |
|----------|------|----------|-------------|
| `handle` | `string` | ✅ | Database handle |
| `statements` | `array` | ✅ | `[{ sql, params? }, ...]` |

**Returns:** `[{ affectedRows }]` — one result per statement.

```js
const results = await db.batch({
  handle,
  statements: [
    { sql: 'INSERT INTO tasks (title) VALUES ($1)', params: ['Task A'] },
    { sql: 'INSERT INTO tasks (title) VALUES ($1)', params: ['Task B'] },
    { sql: 'UPDATE tasks SET done = 1 WHERE id = $1', params: [1] },
  ]
});
// [{ affectedRows: 1 }, { affectedRows: 1 }, { affectedRows: 1 }]
```

## Connection Strings

### SQLite

| Format | Description |
|--------|-------------|
| `myapp.db` | File in current directory |
| `/path/to/data.db` | Absolute path |
| `:memory:` | In-memory database (lost on close) |

### PostgreSQL

Standard libpq format:

```
host=localhost port=5432 dbname=myapp user=admin password=secret
```

## Parameterized Queries

Always use parameterized queries to prevent SQL injection:

```js
// ✅ Safe — parameterized
await db.query({
  handle,
  sql: 'SELECT * FROM users WHERE name = $1',
  params: [userName]
});

// ❌ Unsafe — string concatenation
await db.query({
  handle,
  sql: `SELECT * FROM users WHERE name = '${userName}'`
});
```

Parameters use positional syntax: `$1`, `$2`, `$3`, etc.

## Transactions

The `db:batch` command wraps all statements in a transaction automatically:

```js
try {
  await db.batch({
    handle,
    statements: [
      { sql: 'INSERT INTO orders (user_id, total) VALUES ($1, $2)', params: [1, 99.50] },
      { sql: 'UPDATE inventory SET stock = stock - 1 WHERE item_id = $1', params: [42] },
      { sql: 'INSERT INTO audit_log (action) VALUES ($1)', params: ['order_placed'] },
    ]
  });
  // All three statements committed
} catch (e) {
  // If any statement fails, ALL are rolled back
  console.error('Transaction failed:', e.message);
}
```

For single-statement transactions, `db:exec` is sufficient (each call is implicitly transactional).

## Connection Pooling

Each `db:open` creates a connection pool. The `poolSize` parameter controls concurrency:

```js
// 8 concurrent connections — good for high-throughput apps
const handle = await db.openDatabase({
  backend: 'sqlite3',
  connStr: 'myapp.db',
  poolSize: 8
});
```

- Default pool size: **4**
- Minimum: 1, Maximum: 32
- The pool is fiber-safe — multiple concurrent IPC requests can use the same handle without blocking
- For SQLite, a pool size of 1–4 is usually sufficient (SQLite serializes writes anyway)
- For PostgreSQL, size the pool based on expected concurrent query load

## Patterns

### CRUD Pattern

```ts
import { db } from '@libanyar/api';

let handle: string;

export async function initDB() {
  handle = await db.openDatabase({ backend: 'sqlite3', connStr: 'app.db' });
  await db.exec({
    handle,
    sql: `CREATE TABLE IF NOT EXISTS notes (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      title TEXT NOT NULL,
      content TEXT DEFAULT '',
      created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    )`
  });
}

export async function listNotes() {
  const { rows } = await db.query({
    handle,
    sql: 'SELECT * FROM notes ORDER BY created_at DESC'
  });
  return rows;
}

export async function createNote(title: string, content: string) {
  const { affectedRows } = await db.exec({
    handle,
    sql: 'INSERT INTO notes (title, content) VALUES ($1, $2)',
    params: [title, content]
  });
  return affectedRows;
}

export async function updateNote(id: number, title: string, content: string) {
  await db.exec({
    handle,
    sql: 'UPDATE notes SET title = $1, content = $2 WHERE id = $3',
    params: [title, content, id]
  });
}

export async function deleteNote(id: number) {
  await db.exec({
    handle,
    sql: 'DELETE FROM notes WHERE id = $1',
    params: [id]
  });
}
```

### In-Memory with Persistence

Use an in-memory database for speed, then encrypt/save to disk (like the Key Storage example):

```cpp
// Open in-memory SQLite
sqlite3_open(":memory:", &db_);

// Work with data in memory (fast!)
// ...

// When saving: serialize the in-memory DB and encrypt to file
// See examples/key-storage/ for a complete implementation
```

## Build Requirements

Database support requires the SOCI library (included with LibAsyik):

```bash
# Enabled by default in CMake
cmake .. -DANYAR_ENABLE_SOCI=ON
```

The `DbPlugin` is automatically registered when SOCI support is enabled. If disabled, the `db:*` commands won't be available.

## Next Steps

- [Writing Plugins](writing-plugins.md) — Create custom plugins with database access
- [Key Storage Example](../examples/key-storage/) — Full encrypted database application
- [Architecture Overview](../ARCHITECTURE.md) — Threading model and fiber-safe design
