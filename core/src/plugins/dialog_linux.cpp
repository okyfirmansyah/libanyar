#include <anyar/plugins/dialog_plugin.h>
#include <anyar/main_thread.h>

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

        return run_on_main_thread([&]() -> json {
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

        return run_on_main_thread([&]() -> json {
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
    // Flexible message dialog modeled after Tauri v2.4+.
    //
    // Args:
    //   message   (string, required)
    //   title     (string, default "Message")
    //   kind      ("info"|"warning"|"error", default "info")
    //   buttons   (string preset OR object with custom labels)
    //
    // Preset strings: "Ok", "OkCancel", "YesNo", "YesNoCancel"
    // Custom objects:
    //   { "ok": "Save" }                                    → Ok
    //   { "ok": "Accept", "cancel": "Decline" }             → OkCancel
    //   { "yes": "Allow", "no": "Deny" }                    → YesNo
    //   { "yes": "Allow", "no": "Deny", "cancel": "Skip" } → YesNoCancel
    //
    // Returns the button string: "Ok", "Cancel", "Yes", "No"
    cmds.add("dialog:message", [](const json& args) -> json {
        std::string title   = args.value("title", "Message");
        std::string message = args.at("message").get<std::string>();
        std::string kind    = args.value("kind", "info");
        json buttons_arg    = args.value("buttons", json("Ok"));

        // Determine button preset and optional custom labels
        enum ButtonPreset { BP_OK, BP_OK_CANCEL, BP_YES_NO, BP_YES_NO_CANCEL };
        ButtonPreset preset = BP_OK;

        // Custom label storage (empty = use GTK defaults)
        std::string lbl_ok, lbl_cancel, lbl_yes, lbl_no;

        if (buttons_arg.is_string()) {
            std::string s = buttons_arg.get<std::string>();
            if (s == "OkCancel")       preset = BP_OK_CANCEL;
            else if (s == "YesNo")     preset = BP_YES_NO;
            else if (s == "YesNoCancel") preset = BP_YES_NO_CANCEL;
            // else default "Ok"
        } else if (buttons_arg.is_object()) {
            if (buttons_arg.contains("yes")) {
                lbl_yes = buttons_arg["yes"].get<std::string>();
                lbl_no  = buttons_arg.value("no", std::string("No"));
                if (buttons_arg.contains("cancel")) {
                    lbl_cancel = buttons_arg["cancel"].get<std::string>();
                    preset = BP_YES_NO_CANCEL;
                } else {
                    preset = BP_YES_NO;
                }
            } else {
                lbl_ok = buttons_arg.value("ok", std::string("OK"));
                if (buttons_arg.contains("cancel")) {
                    lbl_cancel = buttons_arg["cancel"].get<std::string>();
                    preset = BP_OK_CANCEL;
                } else {
                    preset = BP_OK;
                }
            }
        }

        return run_on_main_thread([&]() -> json {
            GtkMessageType msg_type = GTK_MESSAGE_INFO;
            if (kind == "warning") msg_type = GTK_MESSAGE_WARNING;
            else if (kind == "error") msg_type = GTK_MESSAGE_ERROR;
            else if (preset == BP_YES_NO || preset == BP_YES_NO_CANCEL)
                msg_type = GTK_MESSAGE_QUESTION;

            // We always use GTK_BUTTONS_NONE and add buttons manually so we
            // can apply custom labels.
            GtkWidget* dialog = gtk_message_dialog_new(
                nullptr, GTK_DIALOG_MODAL, msg_type, GTK_BUTTONS_NONE,
                "%s", message.c_str());
            gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());

            auto* dlg = GTK_DIALOG(dialog);

            switch (preset) {
            case BP_OK:
                gtk_dialog_add_button(dlg,
                    lbl_ok.empty() ? "OK" : lbl_ok.c_str(),
                    GTK_RESPONSE_OK);
                break;

            case BP_OK_CANCEL:
                gtk_dialog_add_button(dlg,
                    lbl_cancel.empty() ? "Cancel" : lbl_cancel.c_str(),
                    GTK_RESPONSE_CANCEL);
                gtk_dialog_add_button(dlg,
                    lbl_ok.empty() ? "OK" : lbl_ok.c_str(),
                    GTK_RESPONSE_OK);
                break;

            case BP_YES_NO:
                gtk_dialog_add_button(dlg,
                    lbl_no.empty() ? "No" : lbl_no.c_str(),
                    GTK_RESPONSE_NO);
                gtk_dialog_add_button(dlg,
                    lbl_yes.empty() ? "Yes" : lbl_yes.c_str(),
                    GTK_RESPONSE_YES);
                break;

            case BP_YES_NO_CANCEL:
                gtk_dialog_add_button(dlg,
                    lbl_cancel.empty() ? "Cancel" : lbl_cancel.c_str(),
                    GTK_RESPONSE_CANCEL);
                gtk_dialog_add_button(dlg,
                    lbl_no.empty() ? "No" : lbl_no.c_str(),
                    GTK_RESPONSE_NO);
                gtk_dialog_add_button(dlg,
                    lbl_yes.empty() ? "Yes" : lbl_yes.c_str(),
                    GTK_RESPONSE_YES);
                break;
            }

            gint result = gtk_dialog_run(dlg);
            gtk_widget_destroy(dialog);
            pump_gtk();

            switch (result) {
            case GTK_RESPONSE_YES:    return json("Yes");
            case GTK_RESPONSE_NO:     return json("No");
            case GTK_RESPONSE_OK:     return json("Ok");
            case GTK_RESPONSE_CANCEL: return json("Cancel");
            default:                  return json("Cancel"); // window closed
            }
        });
    });

    // ── dialog:ask ──────────────────────────────────────────────────────────
    // Convenience: Yes/No question → boolean (true = Yes).
    // Equivalent to Tauri's ask().
    cmds.add("dialog:ask", [](const json& args) -> json {
        std::string title   = args.value("title", "Question");
        std::string message = args.at("message").get<std::string>();
        std::string kind    = args.value("kind", "info");

        return run_on_main_thread([&]() -> json {
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

    // ── dialog:confirm ──────────────────────────────────────────────────────
    // Convenience: Ok/Cancel confirmation → boolean (true = Ok).
    // Equivalent to Tauri's confirm().
    // Supports optional okLabel / cancelLabel for custom button text.
    cmds.add("dialog:confirm", [](const json& args) -> json {
        std::string title      = args.value("title", "Confirm");
        std::string message    = args.at("message").get<std::string>();
        std::string kind       = args.value("kind", "info");
        std::string okLabel    = args.value("okLabel", std::string("OK"));
        std::string cancelLabel= args.value("cancelLabel", std::string("Cancel"));

        return run_on_main_thread([&]() -> json {
            GtkMessageType msg_type = GTK_MESSAGE_QUESTION;
            if (kind == "warning") msg_type = GTK_MESSAGE_WARNING;
            else if (kind == "error") msg_type = GTK_MESSAGE_ERROR;

            GtkWidget* dialog = gtk_message_dialog_new(
                nullptr, GTK_DIALOG_MODAL, msg_type, GTK_BUTTONS_NONE,
                "%s", message.c_str());
            gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());

            gtk_dialog_add_button(GTK_DIALOG(dialog),
                cancelLabel.c_str(), GTK_RESPONSE_CANCEL);
            gtk_dialog_add_button(GTK_DIALOG(dialog),
                okLabel.c_str(), GTK_RESPONSE_OK);

            gint result = gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            pump_gtk();

            return result == GTK_RESPONSE_OK;
        });
    });
}

} // namespace anyar
