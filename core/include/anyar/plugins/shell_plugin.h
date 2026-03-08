#pragma once

/// @file shell_plugin.h
/// @brief Shell/process integration plugin for LibAnyar.
///
/// Provides IPC commands for opening URLs/paths with the default handler
/// and executing external programs with captured output.
///
/// ## Registered Commands
///
/// | Command          | Args | Returns |
/// |------------------|------|---------|
/// | `shell:openUrl`  | **`url`**: `string` | `null` |
/// | `shell:openPath` | **`path`**: `string` | `null` |
/// | `shell:execute`  | **`program`**, `args?`: `string[]`, `cwd?` | `{code, stdout, stderr}` |
///
/// ## Frontend Usage
/// ```js
/// import { shell } from '@libanyar/api';
///
/// await shell.openUrl({ url: 'https://example.com' });
/// await shell.openPath({ path: '/home/user/Documents' });
///
/// const result = await shell.execute({
///   program: 'ls', args: ['-la'], cwd: '/tmp'
/// });
/// console.log(result.stdout);
/// ```

#include <anyar/plugin.h>

namespace anyar {

/// @brief Shell and external process plugin.
///
/// Registers `shell:openUrl` (opens in default browser), `shell:openPath`
/// (opens in default file manager), and `shell:execute` (runs a program
/// with args and returns its exit code, stdout, and stderr).
///
/// Uses `xdg-open` on Linux for URL/path opening.
///
/// @see IAnyarPlugin
class ShellPlugin : public IAnyarPlugin {
public:
    /// @brief Returns the plugin name: `"shell"`.
    std::string name() const override { return "shell"; }

    /// @brief Registers shell IPC commands.
    /// @param ctx Plugin context providing access to the command registry.
    void initialize(PluginContext& ctx) override;
};

} // namespace anyar
