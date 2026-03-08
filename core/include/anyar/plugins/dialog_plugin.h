#pragma once

/// @file dialog_plugin.h
/// @brief Native dialog plugin for LibAnyar.
///
/// Provides IPC commands for showing native file open/save dialogs and
/// message/confirm dialogs. Uses GTK3 native dialogs on Linux.
///
/// ## Registered Commands
///
/// | Command          | Args | Returns |
/// |------------------|------|---------|
/// | `dialog:open`    | `title?`, `multiple?`, `directory?`, `defaultPath?`, `filters?` | `string[]` or `null` (cancelled) |
/// | `dialog:save`    | `title?`, `defaultPath?`, `filters?` | `string` or `null` (cancelled) |
/// | `dialog:message` | **`message`**, `title?`, `kind?` (`"info"`, `"warning"`, `"error"`) | `null` |
/// | `dialog:confirm` | **`message`**, `title?`, `kind?` | `bool` (`true` = Yes) |
///
/// ## Frontend Usage
/// ```js
/// import { dialog } from '@libanyar/api';
///
/// // Open file dialog with filters
/// const files = await dialog.open({
///   title: 'Select Image',
///   filters: [{ name: 'Images', extensions: ['png', 'jpg'] }]
/// });
///
/// // Save dialog
/// const path = await dialog.save({ defaultPath: 'output.txt' });
///
/// // Message box
/// await dialog.message({ message: 'Done!', kind: 'info' });
///
/// // Confirm dialog
/// const yes = await dialog.confirm({ message: 'Delete this?' });
/// ```

#include <anyar/plugin.h>

namespace anyar {

/// @brief Native dialog plugin.
///
/// Registers `dialog:open`, `dialog:save`, `dialog:message`, and
/// `dialog:confirm` commands that display GTK3-native OS dialogs.
/// All dialogs run on the GTK main thread via `run_on_gtk_main()`.
///
/// The `filters` argument for open/save accepts an array of objects:
/// ```json
/// [{ "name": "Images", "extensions": ["png", "jpg", "gif"] }]
/// ```
///
/// @see IAnyarPlugin
class DialogPlugin : public IAnyarPlugin {
public:
    /// @brief Returns the plugin name: `"dialog"`.
    std::string name() const override { return "dialog"; }

    /// @brief Registers dialog IPC commands.
    /// @param ctx Plugin context providing access to the command registry.
    void initialize(PluginContext& ctx) override;
};

} // namespace anyar
