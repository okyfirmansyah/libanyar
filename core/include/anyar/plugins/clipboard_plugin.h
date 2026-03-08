#pragma once

/// @file clipboard_plugin.h
/// @brief Clipboard access plugin for LibAnyar.
///
/// Provides IPC commands for reading from and writing to the system clipboard.
/// Uses GTK3 clipboard API on Linux.
///
/// ## Registered Commands
///
/// | Command            | Args                | Returns  |
/// |--------------------|---------------------|----------|
/// | `clipboard:read`   | *(none)*            | `string` — current clipboard text |
/// | `clipboard:write`  | `text`: `string`    | `null`   |
///
/// ## Frontend Usage
/// ```js
/// import { invoke } from '@libanyar/api';
///
/// // Read clipboard
/// const text = await invoke('clipboard:read');
///
/// // Write to clipboard
/// await invoke('clipboard:write', { text: 'Hello!' });
/// ```

#include <anyar/plugin.h>

namespace anyar {

/// @brief System clipboard plugin.
///
/// Registers `clipboard:read` and `clipboard:write` commands that interface
/// with the GTK3 clipboard. The read command returns the current clipboard
/// text content, while write replaces it.
///
/// @see IAnyarPlugin
class ClipboardPlugin : public IAnyarPlugin {
public:
    /// @brief Returns the plugin name: `"clipboard"`.
    std::string name() const override { return "clipboard"; }

    /// @brief Registers clipboard IPC commands.
    /// @param ctx Plugin context providing access to the command registry.
    void initialize(PluginContext& ctx) override;
};

} // namespace anyar
