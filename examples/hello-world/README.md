# Hello World Example

A comprehensive demo of LibAnyar's core features using a React frontend.

## Features Demonstrated

- **IPC Commands** — `greet`, `get_info`, `increment` (with state)
- **Events** — `counter:updated` (C++ → frontend push)
- **Built-in Plugins:**
  - File system (`fs:readFile` via native dialog)
  - Clipboard (`clipboard:read`, `clipboard:write`)
  - Shell (`shell:execute` — run `uname -a`)
  - Database (`db:open`, `db:exec`, `db:query` — SQLite todo list)
  - Dialog (`dialog:open` — file picker)

## Project Structure

```
hello-world/
├── CMakeLists.txt
├── src-cpp/
│   └── main.cpp           # 3 custom commands + window config
└── frontend/
    ├── package.json        # React 18 + Vite
    ├── vite.config.js
    ├── index.html
    └── src/
        ├── main.jsx        # Entry point
        └── App.jsx         # Full demo UI with all plugin demos
```

## Building

```bash
# From the libanyar build directory:
make hello_world -j$(nproc)

# Or build frontend separately:
cd examples/hello-world/frontend
npm install && npm run build
```

## Running

```bash
cd build/examples/hello-world
./hello_world
```

The app opens with DevTools enabled (`debug = true`).

## Key Code

### Backend (main.cpp)

```cpp
// Simple command
app.command("greet", [](const json& args) -> json {
    std::string name = args.value("name", "World");
    return {{"message", "Hello, " + name + "! 🎉"}};
});

// Stateful command
app.command("increment", [](const json& args) -> json {
    static int counter = 0;
    return {{"count", ++counter}};
});
```

### Frontend (App.jsx)

```jsx
import { invoke, listen } from '@libanyar/api';

const result = await invoke('greet', { name: 'World' });
// → { message: "Hello, World! 🎉", from: "LibAnyar C++ Backend" }

const unlisten = await listen('counter:updated', (payload) => {
    setCount(payload.count);
});
```
