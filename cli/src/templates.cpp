// anyar CLI — project template generation
// Embeds template content for svelte-ts, react-ts, and vanilla projects

#include "cli.h"
#include <fstream>
#include <iostream>

namespace anyar_cli {

// ── Template registry ───────────────────────────────────────────────────────

static const std::vector<TemplateSpec> TEMPLATES = {
    {"svelte-ts", "Svelte 5 + TypeScript + Tailwind CSS", "svelte"},
    {"react-ts",  "React 18 + TypeScript",                "react"},
    {"vanilla",   "Vanilla HTML/JS (no framework)",       "vanilla"},
};

const std::vector<TemplateSpec>& available_templates() {
    return TEMPLATES;
}

// ── Helper: write file ──────────────────────────────────────────────────────

static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path);
    if (!ofs) {
        print_error("Failed to write: " + path.string());
        return;
    }
    ofs << content;
    print_success("Created " + fs::relative(path).string());
}

// ── Shared files (all templates) ────────────────────────────────────────────

static void gen_cmake(const std::string& name, const fs::path& dest,
                      const fs::path& libanyar_root) {
    // Root CMakeLists.txt
    write_file(dest / "CMakeLists.txt",
R"(cmake_minimum_required(VERSION 3.16)
project()" + name + R"( LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point to LibAnyar source tree
set(LIBANYAR_DIR ")" + libanyar_root.string() + R"(" CACHE PATH "Path to libanyar source tree")

# Include LibAnyar core library
add_subdirectory(${LIBANYAR_DIR}/core ${CMAKE_BINARY_DIR}/anyar_core)

add_executable()" + name + R"(
    src-cpp/main.cpp
)

target_link_libraries()" + name + R"( PRIVATE anyar_core)

# Copy frontend dist to build output
set(FRONTEND_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist)
if(EXISTS ${FRONTEND_DIST_DIR})
    add_custom_command(TARGET )" + name + R"( POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${FRONTEND_DIST_DIR}
            $<TARGET_FILE_DIR:)" + name + R"(>/dist
        COMMENT "Copying frontend dist → build/dist"
    )
endif()
)");
}

static void gen_main_cpp(const std::string& name, const fs::path& dest) {
    write_file(dest / "src-cpp" / "main.cpp",
R"(// )" + name + R"( — LibAnyar Application

#include <anyar/app.h>
#include <iostream>

int main() {
    anyar::AppConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.debug = true;
    config.dist_path = "./dist";

    anyar::App app(config);

    // ── Register commands ───────────────────────────────────────────────

    app.command("greet", [](const anyar::json& args) -> anyar::json {
        std::string name = args.value("name", "World");
        return {{"message", "Hello, " + name + "!"}};
    });

    // ── Create window ───────────────────────────────────────────────────

    anyar::WindowConfig win;
    win.title = ")" + name + R"(";
    win.width = 1024;
    win.height = 768;
    win.resizable = true;
    win.debug = true;

    app.create_window(win);

    std::cout << "[)" + name + R"(] Starting..." << std::endl;

    return app.run();
}
)");
}

static void gen_gitignore(const fs::path& dest) {
    write_file(dest / ".gitignore",
R"(build/
frontend/node_modules/
frontend/dist/
*.o
*.a
)");
}

static void gen_readme(const std::string& name, const std::string& tmpl, const fs::path& dest) {
    write_file(dest / "README.md",
"# " + name + R"(

A desktop application built with [LibAnyar](https://github.com/user/libanyar).

## Development

```bash
# Start dev server (frontend HMR + C++ backend)
anyar dev

# Or manually:
cd frontend && npm run dev &
cd build && cmake .. && make -j$(nproc) && ./)" + name + R"(
```

## Build for Production

```bash
anyar build

# Or manually:
cd frontend && npm run build
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

## Project Structure

```
)" + name + R"(/
├── CMakeLists.txt          # C++ build configuration
├── src-cpp/
│   └── main.cpp            # C++ backend (commands, plugins)
├── frontend/
│   ├── package.json        # Frontend dependencies
│   ├── vite.config.js      # Vite configuration
│   └── src/                # Frontend source ()" + tmpl + R"()
└── build/                  # Build output (git-ignored)
```
)");
}

// ── Svelte 5 + TypeScript template ──────────────────────────────────────────

static void gen_svelte_ts(const std::string& name, const fs::path& dest,
                          const fs::path& libanyar_root) {
    fs::path fe = dest / "frontend";

    write_file(fe / "package.json",
R"({
  "name": ")" + name + R"(-frontend",
  "private": true,
  "version": "0.1.0",
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "vite build",
    "preview": "vite preview"
  },
  "dependencies": {},
  "devDependencies": {
    "@sveltejs/vite-plugin-svelte": "^4.0.0",
    "svelte": "^5.0.0",
    "vite": "^5.4.0",
    "tailwindcss": "^4.0.0",
    "@tailwindcss/vite": "^4.0.0"
  }
}
)");

    write_file(fe / "vite.config.js",
R"(import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import tailwindcss from '@tailwindcss/vite';
import path from 'path';

export default defineConfig({
  plugins: [svelte(), tailwindcss()],
  resolve: {
    alias: {
      '@libanyar/api': path.resolve(__dirname, ')" + libanyar_root.string() + R"(/js-bridge/src'),
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  server: {
    port: 5173,
    strictPort: false,
  },
});
)");

    write_file(fe / "index.html",
R"(<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
    <title>)" + name + R"(</title>
  </head>
  <body>
    <div id="app"></div>
    <script type="module" src="/src/main.js"></script>
  </body>
</html>
)");

    write_file(fe / "src" / "main.js",
R"(import './app.css';
import App from './App.svelte';
import { mount } from 'svelte';

const app = mount(App, {
  target: document.getElementById('app'),
});

export default app;
)");

    write_file(fe / "src" / "app.css",
R"(@import "tailwindcss";

:root {
  --bg: #0e0e11;
  --surface: #151518;
  --border: #2e2e36;
  --accent: #7c6bf6;
  --text: #ededf0;
  --text-dim: #a8a8b4;
}

* { box-sizing: border-box; margin: 0; padding: 0; }

html {
  overflow: hidden;
  height: 100%;
  touch-action: none;
}

body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  font-size: 14px;
  line-height: 1.5;
  background: var(--bg);
  color: var(--text);
  min-height: 100vh;
  -webkit-font-smoothing: antialiased;
  overflow: hidden;
  touch-action: none;
  overscroll-behavior: none;
}
)");

    write_file(fe / "src" / "App.svelte",
R"(<script>
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

<main class="h-screen flex items-center justify-center">
  <div class="text-center space-y-6 max-w-md">
    <h1 class="text-3xl font-bold" style="color: var(--accent);">
      )" + name + R"(
    </h1>
    <p style="color: var(--text-dim);">
      Built with LibAnyar — C++ backend + Svelte frontend
    </p>

    <div class="flex gap-2 justify-center">
      <input
        type="text"
        placeholder="Enter a name..."
        bind:value={name}
        onkeydown={(e) => e.key === 'Enter' && greet()}
        class="px-4 py-2 rounded-lg text-sm"
        style="background: var(--surface); border: 1px solid var(--border); color: var(--text);"
      />
      <button
        onclick={greet}
        class="px-4 py-2 rounded-lg text-sm font-medium text-white cursor-pointer"
        style="background: var(--accent);"
      >
        Greet
      </button>
    </div>

    {#if message}
      <div class="px-4 py-3 rounded-lg text-sm"
           style="background: var(--surface); border: 1px solid var(--border);">
        {message}
      </div>
    {/if}
  </div>
</main>
)");
}

// ── React 18 + TypeScript template ──────────────────────────────────────────

static void gen_react_ts(const std::string& name, const fs::path& dest,
                         const fs::path& libanyar_root) {
    fs::path fe = dest / "frontend";

    write_file(fe / "package.json",
R"({
  "name": ")" + name + R"(-frontend",
  "private": true,
  "version": "0.1.0",
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "vite build",
    "preview": "vite preview"
  },
  "dependencies": {
    "react": "^18.3.0",
    "react-dom": "^18.3.0"
  },
  "devDependencies": {
    "@types/react": "^18.3.0",
    "@types/react-dom": "^18.3.0",
    "@vitejs/plugin-react": "^4.3.0",
    "vite": "^5.4.0"
  }
}
)");

    write_file(fe / "vite.config.js",
R"(import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import path from 'path';

export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: {
      '@libanyar/api': path.resolve(__dirname, ')" + libanyar_root.string() + R"(/js-bridge/src'),
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  server: {
    port: 5173,
    strictPort: false,
  },
});
)");

    write_file(fe / "index.html",
R"(<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
    <title>)" + name + R"(</title>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.jsx"></script>
  </body>
</html>
)");

    write_file(fe / "src" / "main.jsx",
R"(import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import './index.css';

ReactDOM.createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
)");

    write_file(fe / "src" / "index.css",
R"(:root {
  --bg: #0e0e11;
  --surface: #151518;
  --border: #2e2e36;
  --accent: #7c6bf6;
  --text: #ededf0;
  --text-dim: #a8a8b4;
}

* { box-sizing: border-box; margin: 0; padding: 0; }

html {
  overflow: hidden;
  height: 100%;
  touch-action: none;
}

body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  font-size: 14px;
  line-height: 1.5;
  background: var(--bg);
  color: var(--text);
  min-height: 100vh;
  -webkit-font-smoothing: antialiased;
  overflow: hidden;
  touch-action: none;
  overscroll-behavior: none;
}
)");

    write_file(fe / "src" / "App.jsx",
R"(import { useState } from 'react';
import { invoke } from '@libanyar/api';

export default function App() {
  const [name, setName] = useState('');
  const [message, setMessage] = useState('');

  async function greet() {
    try {
      const res = await invoke('greet', { name: name || 'World' });
      setMessage(res.message);
    } catch (e) {
      setMessage('Error: ' + e.message);
    }
  }

  return (
    <div style={{
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      height: '100vh',
    }}>
      <div style={{ textAlign: 'center', maxWidth: 400 }}>
        <h1 style={{ fontSize: 28, fontWeight: 700, color: 'var(--accent)', marginBottom: 8 }}>
          )" + name + R"(
        </h1>
        <p style={{ color: 'var(--text-dim)', marginBottom: 24 }}>
          Built with LibAnyar — C++ backend + React frontend
        </p>

        <div style={{ display: 'flex', gap: 8, justifyContent: 'center', marginBottom: 16 }}>
          <input
            type="text"
            placeholder="Enter a name..."
            value={name}
            onChange={e => setName(e.target.value)}
            onKeyDown={e => e.key === 'Enter' && greet()}
            style={{
              padding: '8px 16px', borderRadius: 8, fontSize: 14,
              background: 'var(--surface)', border: '1px solid var(--border)',
              color: 'var(--text)', outline: 'none',
            }}
          />
          <button onClick={greet} style={{
            padding: '8px 16px', borderRadius: 8, fontSize: 14,
            fontWeight: 500, cursor: 'pointer', border: 'none',
            background: 'var(--accent)', color: 'white',
          }}>
            Greet
          </button>
        </div>

        {message && (
          <div style={{
            padding: '12px 16px', borderRadius: 8, fontSize: 14,
            background: 'var(--surface)', border: '1px solid var(--border)',
          }}>
            {message}
          </div>
        )}
      </div>
    </div>
  );
}
)");
}

// ── Vanilla template ────────────────────────────────────────────────────────

static void gen_vanilla(const std::string& name, const fs::path& dest,
                        const fs::path& libanyar_root) {
    fs::path fe = dest / "frontend";

    write_file(fe / "package.json",
R"({
  "name": ")" + name + R"(-frontend",
  "private": true,
  "version": "0.1.0",
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "vite build",
    "preview": "vite preview"
  },
  "devDependencies": {
    "vite": "^5.4.0"
  }
}
)");

    write_file(fe / "vite.config.js",
R"(import { defineConfig } from 'vite';
import path from 'path';

export default defineConfig({
  resolve: {
    alias: {
      '@libanyar/api': path.resolve(__dirname, ')" + libanyar_root.string() + R"(/js-bridge/src'),
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  server: {
    port: 5173,
    strictPort: false,
  },
});
)");

    write_file(fe / "index.html",
R"HTML(<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
    <title>)HTML" + name + R"HTML(</title>
    <style>
      :root {
        --bg: #0e0e11; --surface: #151518; --border: #2e2e36;
        --accent: #7c6bf6; --text: #ededf0; --text-dim: #a8a8b4;
      }
      * { box-sizing: border-box; margin: 0; padding: 0; }
      html { overflow: hidden; height: 100%; touch-action: none; }
      body {
        font-family: 'Inter', -apple-system, sans-serif;
        font-size: 14px; background: var(--bg); color: var(--text);
        display: flex; align-items: center; justify-content: center;
        min-height: 100vh; overflow: hidden; touch-action: none;
      }
      .container { text-align: center; max-width: 400px; }
      h1 { font-size: 28px; color: var(--accent); margin-bottom: 8px; }
      p { color: var(--text-dim); margin-bottom: 24px; }
      .row { display: flex; gap: 8px; justify-content: center; margin-bottom: 16px; }
      input {
        padding: 8px 16px; border-radius: 8px; font-size: 14px;
        background: var(--surface); border: 1px solid var(--border);
        color: var(--text); outline: none;
      }
      button {
        padding: 8px 16px; border-radius: 8px; font-size: 14px;
        font-weight: 500; cursor: pointer; border: none;
        background: var(--accent); color: white;
      }
      #result {
        padding: 12px 16px; border-radius: 8px;
        background: var(--surface); border: 1px solid var(--border);
        display: none;
      }
    </style>
  </head>
  <body>
    <div class="container">
      <h1>)HTML" + name + R"HTML(</h1>
      <p>Built with LibAnyar — C++ backend + Vanilla JS</p>
      <div class="row">
        <input id="name" type="text" placeholder="Enter a name..." />
        <button onclick="greet()">Greet</button>
      </div>
      <div id="result"></div>
    </div>
    <script type="module" src="/src/main.js"></script>
  </body>
</html>
)HTML");

    write_file(fe / "src" / "main.js",
R"(import { invoke } from '@libanyar/api';

window.greet = async function() {
  const name = document.getElementById('name').value || 'World';
  try {
    const res = await invoke('greet', { name });
    const el = document.getElementById('result');
    el.textContent = res.message;
    el.style.display = 'block';
  } catch (e) {
    const el = document.getElementById('result');
    el.textContent = 'Error: ' + e.message;
    el.style.display = 'block';
  }
};

document.getElementById('name').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') window.greet();
});
)");
}

// ── Main dispatch ───────────────────────────────────────────────────────────

void generate_template(const std::string& template_name,
                       const std::string& project_name,
                       const fs::path& dest_dir,
                       const fs::path& libanyar_root) {
    // Shared files
    gen_cmake(project_name, dest_dir, libanyar_root);
    gen_main_cpp(project_name, dest_dir);
    gen_gitignore(dest_dir);
    gen_readme(project_name, template_name, dest_dir);

    // Template-specific frontend
    if (template_name == "svelte-ts") {
        gen_svelte_ts(project_name, dest_dir, libanyar_root);
    } else if (template_name == "react-ts") {
        gen_react_ts(project_name, dest_dir, libanyar_root);
    } else if (template_name == "vanilla") {
        gen_vanilla(project_name, dest_dir, libanyar_root);
    }
}

} // namespace anyar_cli
