#pragma once

/// @file fs_plugin.h
/// @brief File system access plugin for LibAnyar.
///
/// Provides IPC commands for file I/O operations: read, write, list, check
/// existence, create directories, remove files, and query metadata.
///
/// ## Registered Commands
///
/// | Command        | Args | Returns |
/// |----------------|------|---------|
/// | `fs:readFile`  | **`path`**, `encoding?` (`"utf-8"` or `"base64"`) | `string` — file contents |
/// | `fs:writeFile` | **`path`**, **`content`** | `null` |
/// | `fs:readDir`   | **`path`** | `[{name, isDirectory, isFile}]` |
/// | `fs:exists`    | **`path`** | `bool` |
/// | `fs:mkdir`     | **`path`** | `null` (creates parent dirs) |
/// | `fs:remove`    | **`path`**, `recursive?` | `null` |
/// | `fs:metadata`  | **`path`** | `{size, isDirectory, isFile, modifiedTime}` |
///
/// ## Frontend Usage
/// ```js
/// import { fs } from '@libanyar/api';
///
/// const text = await fs.readFile({ path: '/tmp/notes.txt' });
/// await fs.writeFile({ path: '/tmp/out.txt', content: 'hello' });
/// const entries = await fs.readDir({ path: '/home/user' });
/// const exists = await fs.exists({ path: '/tmp/file.txt' });
/// const meta = await fs.stat({ path: '/tmp/file.txt' });
/// ```

#include <anyar/plugin.h>

namespace anyar {

/// @brief File system operations plugin.
///
/// Registers 7 filesystem commands for reading, writing, listing,
/// and managing files and directories. Supports UTF-8 and base64
/// encoding for binary file access.
///
/// @see IAnyarPlugin
class FsPlugin : public IAnyarPlugin {
public:
    /// @brief Returns the plugin name: `"fs"`.
    std::string name() const override { return "fs"; }

    /// @brief Registers filesystem IPC commands.
    /// @param ctx Plugin context providing access to the command registry.
    void initialize(PluginContext& ctx) override;
};

} // namespace anyar
