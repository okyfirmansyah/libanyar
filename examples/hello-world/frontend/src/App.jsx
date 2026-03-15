import React, { useState, useEffect } from 'react';
import { invoke, listen } from '@libanyar/api';

export default function App() {
  const [greeting, setGreeting] = useState('');
  const [name, setName] = useState('');
  const [count, setCount] = useState(0);
  const [info, setInfo] = useState(null);
  const [status, setStatus] = useState('Connecting...');
  const [clipboardText, setClipboardText] = useState('');
  const [fileContent, setFileContent] = useState('');
  const [dbHandle, setDbHandle] = useState(null);
  const [dbRows, setDbRows] = useState([]);
  const [dbStatus, setDbStatus] = useState('');
  const [todoText, setTodoText] = useState('');

  useEffect(() => {
    // Fetch framework info on mount
    invoke('anyar:ping')
      .then(() => {
        setStatus('Connected ✓');
        return invoke('get_info');
      })
      .then((data) => setInfo(data))
      .catch((err) => setStatus(`Error: ${err.message}`));

    // Listen for events from backend
    const unsub = listen('counter:updated', (payload) => {
      setCount(payload.count);
    });

    return () => unsub();
  }, []);

  async function handleGreet() {
    try {
      const result = await invoke('greet', { name: name || 'World' });
      setGreeting(result.message);
    } catch (err) {
      setGreeting(`Error: ${err.message}`);
    }
  }

  async function handleIncrement() {
    try {
      const result = await invoke('increment');
      setCount(result.count);
    } catch (err) {
      console.error('Increment failed:', err);
    }
  }

  // ── Native API demos (Phase 3) ───────────────────────────────────────────

  async function handleOpenFile() {
    try {
      const paths = await invoke('dialog:open', {
        title: 'Open a text file',
        filters: [{ name: 'Text Files', extensions: ['txt', 'md', 'json'] }],
      });
      if (paths && paths.length > 0) {
        const content = await invoke('fs:readFile', { path: paths[0] });
        setFileContent(content);
      }
    } catch (err) {
      console.error('Open file failed:', err);
    }
  }

  async function handleClipboardRead() {
    try {
      const text = await invoke('clipboard:read');
      setClipboardText(text || '(empty)');
    } catch (err) {
      setClipboardText(`Error: ${err.message}`);
    }
  }

  async function handleClipboardWrite() {
    try {
      await invoke('clipboard:write', { text: 'Hello from LibAnyar!' });
      setClipboardText('Wrote: "Hello from LibAnyar!"');
    } catch (err) {
      setClipboardText(`Error: ${err.message}`);
    }
  }

  async function handleRunCommand() {
    try {
      const result = await invoke('shell:execute', {
        program: 'uname',
        args: ['-a'],
      });
      setFileContent(`Exit: ${result.code}\n${result.stdout}`);
    } catch (err) {
      setFileContent(`Error: ${err.message}`);
    }
  }

  // ── Database demos (Phase 4) ──────────────────────────────────────────────

  async function handleDbInit() {
    try {
      const handle = await invoke('db:open', {
        backend: 'sqlite3',
        connStr: 'hello_world.db',
        poolSize: 2,
      });
      setDbHandle(handle);
      await invoke('db:exec', {
        handle,
        sql: 'CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL, done INTEGER DEFAULT 0)',
      });
      setDbStatus('SQLite ready ✓');
      await refreshTodos(handle);
    } catch (err) {
      setDbStatus(`Error: ${err.message}`);
    }
  }

  async function refreshTodos(handle) {
    const h = handle || dbHandle;
    if (!h) return;
    try {
      const result = await invoke('db:query', {
        handle: h,
        sql: 'SELECT id, text, done FROM todos ORDER BY id DESC',
      });
      setDbRows(result.rows || []);
    } catch (err) {
      console.error('Query failed:', err);
    }
  }

  async function handleAddTodo() {
    if (!dbHandle || !todoText.trim()) return;
    try {
      await invoke('db:exec', {
        handle: dbHandle,
        sql: 'INSERT INTO todos (text) VALUES ($1)',
        params: [todoText.trim()],
      });
      setTodoText('');
      await refreshTodos();
    } catch (err) {
      setDbStatus(`Insert error: ${err.message}`);
    }
  }

  async function handleToggleTodo(id, currentDone) {
    if (!dbHandle) return;
    try {
      await invoke('db:exec', {
        handle: dbHandle,
        sql: 'UPDATE todos SET done = $1 WHERE id = $2',
        params: [currentDone ? 0 : 1, id],
      });
      await refreshTodos();
    } catch (err) {
      console.error('Toggle failed:', err);
    }
  }

  async function handleDeleteTodo(id) {
    if (!dbHandle) return;
    try {
      await invoke('db:exec', {
        handle: dbHandle,
        sql: 'DELETE FROM todos WHERE id = $1',
        params: [id],
      });
      await refreshTodos();
    } catch (err) {
      console.error('Delete failed:', err);
    }
  }

  return (
    <div className="container">
      <header>
        <img src="/assets/libanyar.png" alt="LibAnyar" style={{ height: '96px', marginBottom: '4px' }} />
        <p className="subtitle">C++ Desktop Framework with Web Frontend</p>
        <span className={`status ${status.includes('✓') ? 'connected' : 'pending'}`}>
          {status}
        </span>
      </header>

      <section className="card">
        <h2>Greet Command</h2>
        <div className="input-row">
          <input
            type="text"
            placeholder="Enter your name..."
            value={name}
            onChange={(e) => setName(e.target.value)}
            onKeyDown={(e) => e.key === 'Enter' && handleGreet()}
          />
          <button onClick={handleGreet}>Say Hello</button>
        </div>
        {greeting && <p className="result">{greeting}</p>}
      </section>

      <section className="card">
        <h2>Counter (IPC Roundtrip)</h2>
        <div className="counter-row">
          <span className="count">{count}</span>
          <button onClick={handleIncrement}>+ Increment</button>
        </div>
      </section>

      <section className="card">
        <h2>Native APIs</h2>
        <div className="button-row">
          <button onClick={handleOpenFile}>📂 Open File</button>
          <button onClick={handleClipboardRead}>📋 Read Clipboard</button>
          <button onClick={handleClipboardWrite}>✏️ Write Clipboard</button>
          <button onClick={handleRunCommand}>⚡ Run uname -a</button>
        </div>
        {clipboardText && <p className="result">Clipboard: {clipboardText}</p>}
        {fileContent && (
          <pre className="result" style={{ maxHeight: '200px', overflow: 'auto', whiteSpace: 'pre-wrap' }}>
            {fileContent}
          </pre>
        )}
      </section>

      <section className="card">
        <h2>Database (SQLite)</h2>
        {!dbHandle ? (
          <button onClick={handleDbInit}>🗄️ Open SQLite Database</button>
        ) : (
          <>
            <span className="status connected">{dbStatus}</span>
            <div className="input-row" style={{ marginTop: '12px' }}>
              <input
                type="text"
                placeholder="Add a todo..."
                value={todoText}
                onChange={(e) => setTodoText(e.target.value)}
                onKeyDown={(e) => e.key === 'Enter' && handleAddTodo()}
              />
              <button onClick={handleAddTodo}>Add</button>
            </div>
            {dbRows.length > 0 && (
              <ul className="todo-list">
                {dbRows.map((row) => (
                  <li key={row.id} className={row.done ? 'done' : ''}>
                    <label>
                      <input
                        type="checkbox"
                        checked={!!row.done}
                        onChange={() => handleToggleTodo(row.id, row.done)}
                      />
                      <span>{row.text}</span>
                    </label>
                    <button className="btn-delete" onClick={() => handleDeleteTodo(row.id)}>×</button>
                  </li>
                ))}
              </ul>
            )}
            {dbRows.length === 0 && <p className="result">No todos yet. Add one above!</p>}
          </>
        )}
      </section>

      {info && (
        <section className="card info">
          <h2>Backend Info</h2>
          <table>
            <tbody>
              {Object.entries(info).map(([key, value]) => (
                <tr key={key}>
                  <td className="key">{key}</td>
                  <td>{String(value)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
      )}

      <footer>
        <p>
          Built with <strong>LibAsyik</strong> + <strong>webview/webview</strong> + <strong>React</strong>
        </p>
      </footer>
    </div>
  );
}
