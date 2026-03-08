#pragma once

/// @file dialog_plugin.h
/// @brief Native dialog plugin for LibAnyar.
///
/// Provides IPC commands for showing native file open/save dialogs and
/// message/confirm/ask dialogs.  Uses GTK3 native dialogs on Linux.
///
/// ## Registered Commands
///
/// | Command          | Args | Returns |
/// |------------------|------|---------|
/// | `dialog:open`    | `title?`, `multiple?`, `directory?`, `defaultPath?`, `filters?` | `string[]` or `null` (cancelled) |
/// | `dialog:save`    | `title?`, `defaultPath?`, `filters?` | `string` or `null` (cancelled) |
/// | `dialog:message` | **`message`**, `title?`, `kind?`, `buttons?` | `string` ("Ok", "Cancel", "Yes", "No") |
/// | `dialog:ask`     | **`message`**, `title?`, `kind?` | `bool` (`true` = Yes) |
/// | `dialog:confirm` | **`message`**, `title?`, `kind?`, `okLabel?`, `cancelLabel?` | `bool` (`true` = Ok) |
///
/// ### `buttons` parameter for `dialog:message`
///
/// Preset strings: `"Ok"`, `"OkCancel"`, `"YesNo"`, `"YesNoCancel"`
///
/// Custom label objects:
/// ```json
/// { "ok": "Save" }
/// { "ok": "Accept", "cancel": "Decline" }
/// { "yes": "Allow", "no": "Deny", "cancel": "Skip" }
/// ```
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
/// // Message box (Ok button, returns "Ok")
/// await dialog.message('Done!');
/// await dialog.message('File not found', { title: 'Error', kind: 'error' });
///
/// // Yes/No question (returns boolean)
/// const yes = await dialog.ask('Delete this entry?', { kind: 'warning' });
///
/// // Ok/Cancel confirmation (returns boolean)
/// const ok = await dialog.confirm('Discard unsaved changes?');
///
/// // Custom buttons
/// const result = await dialog.message('Save changes?', {
///   buttons: { yes: 'Save', no: 'Discard', cancel: 'Cancel' },
/// });
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
