# Building Your First App

This tutorial walks through creating a complete LibAnyar application from scratch — a simple Greeter app with a Svelte frontend and C++ backend.

## 1. Scaffold the Project

```bash
# From your libanyar build directory:
./cli/anyar init greeter --template svelte-ts

cd greeter
```

This creates:

```
greeter/
├── CMakeLists.txt
├── src-cpp/
│   └── main.cpp         ← C++ backend
├── frontend/
│   ├── package.json
│   ├── vite.config.js
│   ├── index.html
│   └── src/
│       ├── main.js
│       ├── app.css
│       └── App.svelte   ← Frontend UI
└── README.md
```

## 2. Understand the Backend

Open `src-cpp/main.cpp`:

```cpp
#include <anyar/app.h>

int main() {
    anyar::AppConfig config;
    config.dist_path = "./dist";

    anyar::App app(config);

    // This is a "command" — callable from the frontend via IPC
    app.command("greet", [](const json& args) -> json {
        std::string name = args.value("name", "World");
        return {{"message", "Hello, " + name + "!"}};
    });

    anyar::WindowConfig win;
    win.title = "Greeter";
    win.width = 800;
    win.height = 600;
    app.create_window(win);

    return app.run();
}
```

Key concepts:
- **`app.command(name, handler)`** registers a synchronous IPC command
- The handler receives a `json` object (arguments from the frontend) and returns a `json` result
- **`app.create_window(config)`** opens a webview window pointing to the frontend
- **`app.run()`** starts the HTTP server + event loop (blocks until the window closes)

## 3. Understand the Frontend

Open `frontend/src/App.svelte`:

```svelte
<script>
  import { invoke } from '@libanyar/api';

  let message = $state('');
  let name = $state('');

  async function greet() {
    try {
      const res = await invoke('greet', { name: name || 'World' });
      message = res.message;
    } catch (e) {
      message = 'Error: ' + e.message;
    }
  }
</script>

<main>
  <h1>Greeter</h1>
  <input type="text" bind:value={name} placeholder="Enter a name..." />
  <button onclick={greet}>Greet</button>
  {#if message}
    <p>{message}</p>
  {/if}
</main>
```

Key concepts:
- **`invoke(command, args)`** calls the C++ command registered with `app.command()`
- It returns a Promise that resolves with the JSON result
- Uses **native IPC** (< 0.01ms) in the desktop app, falls back to HTTP in browser dev mode

## 4. Add a New Command

Let's add a `get_time` command to the backend. Edit `src-cpp/main.cpp`:

```cpp
#include <anyar/app.h>
#include <ctime>
#include <iomanip>
#include <sstream>

int main() {
    anyar::AppConfig config;
    config.dist_path = "./dist";

    anyar::App app(config);

    app.command("greet", [](const json& args) -> json {
        std::string name = args.value("name", "World");
        return {{"message", "Hello, " + name + "!"}};
    });

    // NEW: Return the current time
    app.command("get_time", [](const json&) -> json {
        auto now = std::time(nullptr);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M:%S");
        return {{"time", oss.str()}};
    });

    anyar::WindowConfig win;
    win.title = "Greeter";
    win.width = 800;
    win.height = 600;
    app.create_window(win);

    return app.run();
}
```

Then use it in the frontend (`frontend/src/App.svelte`):

```svelte
<script>
  import { invoke } from '@libanyar/api';

  let message = $state('');
  let name = $state('');
  let time = $state('');

  async function greet() {
    const res = await invoke('greet', { name: name || 'World' });
    message = res.message;
  }

  async function getTime() {
    const res = await invoke('get_time');
    time = res.time;
  }
</script>

<main>
  <h1>Greeter</h1>
  <input type="text" bind:value={name} placeholder="Enter a name..." />
  <button onclick={greet}>Greet</button>
  <button onclick={getTime}>Get Time</button>

  {#if message}<p>{message}</p>{/if}
  {#if time}<p>Server time: {time}</p>{/if}
</main>
```

## 5. Run in Development Mode

```bash
# From the project root:
anyar dev
```

This starts:
1. **Vite dev server** (port 5173) with Hot Module Replacement — change Svelte code, see updates instantly
2. **C++ backend** build and run — the native app window opens pointing to the Vite server

> **Tip:** Frontend changes apply instantly via HMR. Backend (C++) changes require restarting `anyar dev`.

## 6. Build for Production

```bash
anyar build
```

This:
1. Builds the frontend → `frontend/dist/`
2. Compiles C++ in Release mode
3. Copies `dist/` next to the binary
4. Reports the binary path and size

Run the production binary:

```bash
cd build && ./greeter
```

## 6b. Package for Distribution

Once your production build succeeds, create distributable Linux packages:

```bash
# DEB package (Ubuntu/Debian)
anyar build --package deb --version 1.0.0

# AppImage (portable Linux)
anyar build --package appimage --version 1.0.0

# Both at once
anyar build --package all --version 1.0.0
```

Output files appear in `build/`:
- `greeter_1.0.0_amd64.deb`
- `greeter-1.0.0-x86_64.AppImage`

See [Packaging & Distribution](packaging.md) for details on custom icons, dependency detection, and advanced options.

## 6c. Single-Binary Deployment

Embed the frontend directly into the binary so no external `dist/` directory is needed:

```bash
anyar build --embed
```

The resulting binary contains all frontend assets compiled in via [CMakeRC](https://github.com/vector-of-bool/cmrc). You can copy it anywhere and it runs standalone — no `dist/` folder required.

Combine with packaging for the smallest distributable:

```bash
anyar build --embed --package deb --version 1.0.0
```

## 7. Using Events

LibAnyar supports bidirectional events between C++ and the frontend.

### Emit from C++ → Frontend

```cpp
// In main.cpp — emit an event every second
app.command_async("start_clock", [&app](const json&, auto reply) {
    auto service = app.service();
    asyik::sleep_for(std::chrono::seconds(0));  // yield to fiber scheduler

    for (int i = 0; i < 60; i++) {
        auto now = std::time(nullptr);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&now), "%H:%M:%S");
        app.emit("clock:tick", {{"time", oss.str()}});
        asyik::sleep_for(std::chrono::seconds(1));
    }
    reply({{"status", "done"}});
});
```

### Listen in Frontend

```js
import { invoke, listen } from '@libanyar/api';

// Start the clock
await invoke('start_clock');

// Listen for tick events
const unlisten = await listen('clock:tick', (event) => {
  console.log('Time:', event.data.time);
});

// Later: stop listening
unlisten();
```

## 8. Using Built-in Plugins

LibAnyar comes with built-in plugins for common tasks:

```js
import { fs, dialog, shell, db } from '@libanyar/api';

// File system
const content = await fs.readFile({ path: '/tmp/notes.txt' });

// Native dialogs
const files = await dialog.open({
  title: 'Select a file',
  filters: [{ name: 'Text', extensions: ['txt', 'md'] }]
});

// Shell
await shell.openUrl({ url: 'https://example.com' });

// Database
const handle = await db.openDatabase({ backend: 'sqlite3', connStr: ':memory:' });
await db.exec({ handle, sql: 'CREATE TABLE tasks (id INTEGER PRIMARY KEY, name TEXT)' });
```

## Next Steps

- [Packaging & Distribution](packaging.md) — Create DEB packages and AppImage bundles
- [Writing Plugins](writing-plugins.md) — Create custom C++ plugins for your app
- [Database Integration](database-integration.md) — Full SQLite/PostgreSQL guide
- [Architecture Overview](../ARCHITECTURE.md) — How the IPC, events, and threading model work
