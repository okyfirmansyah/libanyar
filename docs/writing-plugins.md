# Writing Plugins

LibAnyar's plugin system lets you encapsulate C++ functionality into reusable modules with their own IPC commands and event handlers.

## Plugin Interface

Every plugin implements the `IAnyarPlugin` interface:

```cpp
#include <anyar/plugin.h>

namespace anyar {

class IAnyarPlugin {
public:
    virtual ~IAnyarPlugin() = default;

    /// Unique plugin name (used for logging)
    virtual std::string name() const = 0;

    /// Called once during app initialization
    virtual void initialize(PluginContext& ctx) = 0;

    /// Called during app shutdown (optional override)
    virtual void shutdown() {}
};

}
```

The `PluginContext` provides everything a plugin needs:

```cpp
struct PluginContext {
    asyik::service_ptr service;      // LibAsyik async service (fibers, timers)
    anyar_http_server_ptr server;    // HTTP server (for custom routes)
    CommandRegistry& commands;       // Register IPC commands
    EventBus& events;               // Emit/subscribe to events
    AppConfig& config;               // App configuration
};
```

## Creating a Plugin

### Step 1: Header File

```cpp
// src-cpp/weather_plugin.h
#pragma once

#include <anyar/plugin.h>

class WeatherPlugin : public anyar::IAnyarPlugin {
public:
    std::string name() const override { return "weather"; }
    void initialize(anyar::PluginContext& ctx) override;

private:
    std::string api_key_;
};
```

### Step 2: Implementation

```cpp
// src-cpp/weather_plugin.cpp
#include "weather_plugin.h"
#include <iostream>

void WeatherPlugin::initialize(anyar::PluginContext& ctx) {
    auto& cmds = ctx.commands;
    auto& events = ctx.events;

    // Register a synchronous command
    cmds.add("weather:get", [this](const json& args) -> json {
        std::string city = args.at("city").get<std::string>();

        // In a real plugin, you'd call a weather API here
        return {
            {"city", city},
            {"temp", 22},
            {"condition", "Sunny"}
        };
    });

    // Register an async command (for long-running operations)
    cmds.add_async("weather:subscribe", [this, &events](const json& args, auto reply) {
        std::string city = args.at("city").get<std::string>();

        // Simulate periodic updates
        for (int i = 0; i < 10; i++) {
            events.emit("weather:update", {
                {"city", city},
                {"temp", 20 + (i % 5)},
                {"iteration", i}
            });
            asyik::sleep_for(std::chrono::seconds(5));
        }

        reply({{"status", "done"}});
    });

    std::cout << "[weather] Plugin initialized" << std::endl;
}
```

### Step 3: Register in main.cpp

```cpp
#include <anyar/app.h>
#include "weather_plugin.h"

int main() {
    anyar::App app;

    // Register the plugin
    app.use(std::make_shared<WeatherPlugin>());

    app.create_window({.title = "Weather App"});
    return app.run();
}
```

### Step 4: Use from Frontend

```js
import { invoke, listen } from '@libanyar/api';

// Call a command
const weather = await invoke('weather:get', { city: 'Jakarta' });
console.log(weather.temp); // 22

// Subscribe to events
const unlisten = await listen('weather:update', (event) => {
    console.log('Update:', event.data);
});

// Start async subscription
await invoke('weather:subscribe', { city: 'Jakarta' });
```

## Command Types

### Synchronous Commands

Use `cmds.add()` for fast, non-blocking operations:

```cpp
cmds.add("plugin:sync_command", [](const json& args) -> json {
    // Runs on the fiber event loop — must return quickly
    return {{"result", "fast"}};
});
```

### Asynchronous Commands

Use `cmds.add_async()` for long-running operations. The reply callback can be called later:

```cpp
cmds.add_async("plugin:long_task", [](const json& args, auto reply) {
    // Runs in its own fiber — can sleep/await without blocking
    asyik::sleep_for(std::chrono::seconds(2));
    reply({{"result", "done after 2s"}});
});
```

**Important:** Async commands run in a Boost fiber. You can use `asyik::sleep_for()`, fiber mutexes, and other LibAsyik async primitives.

## Naming Convention

Use a namespace prefix for your commands and events:

```
<plugin>:<action>
```

Examples:
- `weather:get`, `weather:subscribe`
- `auth:login`, `auth:logout`
- `ks:open`, `ks:save`, `ks:entries`

Built-in plugins use: `fs:`, `dialog:`, `shell:`, `clipboard:`, `db:`.

## Plugin Lifecycle

```
app.use(plugin)           → plugin stored
app.run()                 → plugin.initialize(ctx) called
  ... app running ...
  window closed
app.~App()                → plugin.shutdown() called
```

The `shutdown()` method is optional — override it to clean up resources:

```cpp
void shutdown() override {
    // Close connections, free resources
    std::cout << "[weather] Plugin shutting down" << std::endl;
}
```

## Accessing the HTTP Server

Plugins can register custom HTTP routes for advanced use cases:

```cpp
void initialize(anyar::PluginContext& ctx) override {
    auto server = ctx.server;

    // Custom REST endpoint
    server->on_http("/api/weather/{city}", "GET",
        [](auto req, auto args) {
            std::string city = args["city"];
            req->response.body = json{{"city", city}, {"temp", 22}}.dump();
            req->response.result(200);
        });
}
```

## Real-World Example

The [Key Storage example](../examples/key-storage/) demonstrates a complete plugin with:

- **24 IPC commands** (`ks:open`, `ks:save`, `ks:entries`, `ks:create_entry`, etc.)
- **SQLite database** with in-memory decryption
- **AES-256-GCM encryption** for secure file storage
- **PBKDF2 key derivation** from master password
- **Resource cleanup** in `shutdown()`

See [keystore_plugin.h](../examples/key-storage/src-cpp/keystore_plugin.h) for the full implementation.

## Built-in Plugins

LibAnyar ships with these plugins (auto-registered):

| Plugin | Commands | Description |
|--------|----------|-------------|
| `FsPlugin` | `fs:readFile`, `fs:writeFile`, `fs:readDir`, `fs:exists`, `fs:mkdir`, `fs:remove`, `fs:metadata` | File system operations |
| `DialogPlugin` | `dialog:open`, `dialog:save`, `dialog:message`, `dialog:ask`, `dialog:confirm` | Native GTK3 dialogs ([Dialog Guide](dialogs.md)) |
| `ShellPlugin` | `shell:openUrl`, `shell:openPath`, `shell:execute` | External programs & URLs |
| `ClipboardPlugin` | `clipboard:read`, `clipboard:write` | System clipboard access |
| `DbPlugin` | `db:open`, `db:close`, `db:query`, `db:exec`, `db:batch` | SQLite/PostgreSQL databases |

## Next Steps

- [Database Integration](database-integration.md) — Complete guide to the DbPlugin
- [Architecture Overview](../ARCHITECTURE.md) — Threading model, IPC protocol details
