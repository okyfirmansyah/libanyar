#pragma once

/// @file db_plugin.h
/// @brief Database access plugin for LibAnyar.
///
/// Provides IPC commands for managing database connections and executing
/// SQL queries. Supports SQLite3 and PostgreSQL via LibAsyik's fiber-safe
/// SOCI connection pool.
///
/// ## Registered Commands
///
/// | Command     | Args | Returns |
/// |-------------|------|---------|
/// | `db:open`   | **`backend`** (`"sqlite3"` or `"postgresql"`), **`connStr`**, `poolSize?` (1–32, default 4) | `string` — opaque handle |
/// | `db:close`  | **`handle`** | `{closed: true}` |
/// | `db:query`  | **`handle`**, **`sql`**, `params?` (positional `$1,$2,...`) | `{rows: object[], columns: string[]}` |
/// | `db:exec`   | **`handle`**, **`sql`**, `params?` | `{affectedRows: int}` |
/// | `db:batch`  | **`handle`**, **`statements`**: `[{sql, params?}]` | `[{affectedRows}]` (transactional) |
///
/// ## Frontend Usage
/// ```js
/// import { db } from '@libanyar/api';
///
/// const handle = await db.openDatabase({
///   backend: 'sqlite3', connStr: 'myapp.db'
/// });
///
/// await db.exec({ handle, sql: 'CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, text TEXT)' });
/// await db.exec({ handle, sql: 'INSERT INTO notes (text) VALUES ($1)', params: ['Hello'] });
///
/// const { rows } = await db.query({ handle, sql: 'SELECT * FROM notes' });
/// ```

#include <anyar/plugin.h>

#include <libasyik/sql.hpp>

#include <memory>
#include <string>
#include <unordered_map>

#include <boost/fiber/mutex.hpp>

namespace anyar {

/// @brief Database access plugin with connection pooling.
///
/// Manages named database connections through opaque handle strings.
/// Each `db:open` call creates a SOCI connection pool; subsequent
/// `db:query`, `db:exec`, and `db:batch` calls borrow sessions from
/// the pool. The `db:batch` command executes all statements in a
/// single transaction (automatic rollback on failure).
///
/// Connections are fiber-safe — multiple concurrent IPC requests can
/// share the same database handle without blocking the event loop.
///
/// @see IAnyarPlugin
class DbPlugin : public IAnyarPlugin {
public:
    /// @brief Returns the plugin name: `"db"`.
    std::string name() const override { return "db"; }

    /// @brief Registers database IPC commands.
    /// @param ctx Plugin context providing access to the command registry and service.
    void initialize(PluginContext& ctx) override;

    /// @brief Closes all open database pools on shutdown.
    void shutdown() override;

private:
    /// @brief Map of handle → connection pool.
    std::unordered_map<std::string, asyik::sql_pool_ptr> pools_;

    /// @brief Mutex protecting pool map access across fibers.
    boost::fibers::mutex pools_mutex_;

    /// @brief Service pointer for creating connection pool sessions.
    asyik::service_ptr service_;

    /// @brief Generate a unique handle string (UUID-like).
    std::string generate_handle();

    /// @brief Get pool by handle.
    /// @param handle The opaque handle returned by `db:open`.
    /// @throws std::runtime_error if handle is not found.
    asyik::sql_pool_ptr get_pool(const std::string& handle);
};

} // namespace anyar
