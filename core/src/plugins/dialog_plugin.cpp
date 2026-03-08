#include <anyar/plugins/dialog_plugin.h>
#include <anyar/gtk_dispatch.h>

#include <gtk/gtk.h>
#include <string>
#include <vector>

namespace anyar {

// ── Helpers ─────────────────────────────────────────────────────────────────

static void add_filters(GtkFileChooser* chooser, const json& filters) {
    if (!filters.is_array()) return;

    for (auto& f : filters) {
        GtkFileFilter* gf = gtk_file_filter_new();
        gtk_file_filter_set_name(gf, f.value("name", "").c_str());
        if (f.contains("extensions") && f["extensions"].is_array()) {
            for (auto& ext : f["extensions"]) {
                std::string pattern = "*." + ext.get<std::string>();
                gtk_file_filter_add_pattern(gf, pattern.c_str());
            }
        }
        gtk_file_chooser_add_filter(chooser, gf);
    }
}

/// Pump remaining GTK events after destroying a dialog
static void pump_gtk() {
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
}

// ── Plugin registration ─────────────────────────────────────────────────────

void DialogPlugin::initialize(PluginContext& ctx) {
    auto& cmds = ctx.commands;

    // ── dialog:open ─────────────────────────────────────────────────────────
    cmds.add("dialog:open", [](const json& args) -> json {
        // Copy args we need — they're used inside the main-thread closure
        std::string title   = args.value("title", "Open File");
        bool multiple       = args.value("multiple", false);
        bool directory      = args.value("directory", false);
        std::string defPath = args.value("defaultPath", "");
        json filters        = args.value("filters", json::array());

        return run_on_gtk_main([&]() -> json {
            GtkFileChooserAction action = directory
                ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER
                : GTK_FILE_CHOOSER_ACTION_OPEN;

            GtkWidget* dialog = gtk_file_chooser_dialog_new(
                title.c_str(), nullptr, action,
                "_Cancel", GTK_RESPONSE_CANCEL,
                "_Open", GTK_RESPONSE_ACCEPT,
                nullptr);

            auto chooser = GTK_FILE_CHOOSER(dialog);

            if (multiple) {
                gtk_file_chooser_set_select_multiple(chooser, TRUE);
            }

            if (!defPath.empty()) {
                gtk_file_chooser_set_current_folder(chooser, defPath.c_str());
            }

            add_filters(chooser, filters);

            gint result = gtk_dialog_run(GTK_DIALOG(dialog));

            if (result != GTK_RESPONSE_ACCEPT) {
                gtk_widget_destroy(dialog);
                pump_gtk();
                return nullptr;
            }

            json paths = json::array();

            if (multiple) {
                GSList* list = gtk_file_chooser_get_filenames(chooser);
                for (GSList* iter = list; iter; iter = iter->next) {
                    paths.push_back(std::string(static_cast<char*>(iter->data)));
                    g_free(iter->data);
                }
                g_slist_free(list);
            } else {
                char* filename = gtk_file_chooser_get_filename(chooser);
                if (filename) {
                    paths.push_back(std::string(filename));
                    g_free(filename);
                }
            }

            gtk_widget_destroy(dialog);
            pump_gtk();
            return paths;
        });
    });

    // ── dialog:save ─────────────────────────────────────────────────────────
    cmds.add("dialog:save", [](const json& args) -> json {
        std::string title   = args.value("title", "Save File");
        std::string defPath = args.value("defaultPath", "");
        json filters        = args.value("filters", json::array());

        return run_on_gtk_main([&]() -> json {
            GtkWidget* dialog = gtk_file_chooser_dialog_new(
                title.c_str(), nullptr, GTK_FILE_CHOOSER_ACTION_SAVE,
                "_Cancel", GTK_RESPONSE_CANCEL,
                "_Save", GTK_RESPONSE_ACCEPT,
                nullptr);

            auto chooser = GTK_FILE_CHOOSER(dialog);
            gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);

            if (!defPath.empty()) {
                auto pos = defPath.rfind('/');
                if (pos != std::string::npos) {
                    gtk_file_chooser_set_current_folder(chooser, defPath.substr(0, pos).c_str());
                    if (pos + 1 < defPath.size()) {
                        gtk_file_chooser_set_current_name(chooser, defPath.substr(pos + 1).c_str());
                    }
                } else {
                    gtk_file_chooser_set_current_name(chooser, defPath.c_str());
                }
            }

            add_filters(chooser, filters);

            gint result = gtk_dialog_run(GTK_DIALOG(dialog));

            if (result != GTK_RESPONSE_ACCEPT) {
                gtk_widget_destroy(dialog);
                pump_gtk();
                return nullptr;
            }

            char* filename = gtk_file_chooser_get_filename(chooser);
            std::string path;
            if (filename) {
                path = filename;
                g_free(filename);
            }

            gtk_widget_destroy(dialog);
            pump_gtk();
            return json(path);
        });
    });

    // ── dialog:message ──────────────────────────────────────────────────────
    cmds.add("dialog:message", [](const json& args) -> json {
        std::string title   = args.value("title", "Message");
        std::string message = args.at("message").get<std::string>();
        std::string kind    = args.value("kind", "info");

        return run_on_gtk_main([&]() -> json {
            GtkMessageType msg_type = GTK_MESSAGE_INFO;
            if (kind == "warning") msg_type = GTK_MESSAGE_WARNING;
            else if (kind == "error") msg_type = GTK_MESSAGE_ERROR;

            GtkWidget* dialog = gtk_message_dialog_new(
                nullptr, GTK_DIALOG_MODAL, msg_type, GTK_BUTTONS_OK,
                "%s", message.c_str());
            gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());

            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            pump_gtk();

            return nullptr;
        });
    });

    // ── dialog:confirm ──────────────────────────────────────────────────────
    cmds.add("dialog:confirm", [](const json& args) -> json {
        std::string title   = args.value("title", "Confirm");
        std::string message = args.at("message").get<std::string>();
        std::string kind    = args.value("kind", "info");

        return run_on_gtk_main([&]() -> json {
            GtkMessageType msg_type = GTK_MESSAGE_QUESTION;
            if (kind == "warning") msg_type = GTK_MESSAGE_WARNING;
            else if (kind == "error") msg_type = GTK_MESSAGE_ERROR;

            GtkWidget* dialog = gtk_message_dialog_new(
                nullptr, GTK_DIALOG_MODAL, msg_type, GTK_BUTTONS_YES_NO,
                "%s", message.c_str());
            gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());

            gint result = gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            pump_gtk();

            return result == GTK_RESPONSE_YES;
        });
    });
}

} // namespace anyar
