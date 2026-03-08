#include <anyar/plugins/clipboard_plugin.h>
#include <anyar/main_thread.h>

#include <gtk/gtk.h>
#include <stdexcept>
#include <string>

namespace anyar {

void ClipboardPlugin::initialize(PluginContext& ctx) {
    auto& cmds = ctx.commands;

    // ── clipboard:read ──────────────────────────────────────────────────────
    cmds.add("clipboard:read", [](const json& /*args*/) -> json {
        return run_on_main_thread([]() -> json {
            GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gchar* text = gtk_clipboard_wait_for_text(cb);

            if (!text) {
                return "";
            }

            std::string result(text);
            g_free(text);
            return result;
        });
    });

    // ── clipboard:write ─────────────────────────────────────────────────────
    cmds.add("clipboard:write", [](const json& args) -> json {
        std::string text = args.at("text").get<std::string>();

        return run_on_main_thread([&text]() -> json {
            GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(cb, text.c_str(), static_cast<gint>(text.size()));
            // Store so it persists after the app closes
            gtk_clipboard_store(cb);
            return nullptr;
        });
    });
}

} // namespace anyar
