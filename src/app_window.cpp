#include "app_window.h"
#include <string>

static void on_send_clicked(GtkWidget * /*widget*/, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);

    const char *text = gtk_entry_get_text(GTK_ENTRY(win->text_entry));
    std::string message = "You entered: ";
    message += text;

    gtk_label_set_text(GTK_LABEL(win->output_label), message.c_str());
    gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                       "Message sent");
}

static void on_clear_clicked(GtkWidget * /*widget*/, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);

    gtk_entry_set_text(GTK_ENTRY(win->text_entry), "");
    gtk_label_set_text(GTK_LABEL(win->output_label), "(no output yet)");
    gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                       "Cleared");
}

static void on_window_destroy(GtkWidget * /*widget*/, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    delete win;
}

AppWindow *app_window_new(GtkApplication *app) {
    auto *win = new AppWindow{};

    // Main window
    win->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win->window), "FreeDV Monitor");
    gtk_window_set_default_size(GTK_WINDOW(win->window), 480, 320);
    g_signal_connect(win->window, "destroy", G_CALLBACK(on_window_destroy), win);

    // Vertical box layout
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(win->window), vbox);

    // Header label
    win->header_label = gtk_label_new("FreeDV Monitor");
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.4));
    gtk_label_set_attributes(GTK_LABEL(win->header_label), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(vbox), win->header_label, FALSE, FALSE, 0);

    // Separator
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    // Input row: label + entry
    GtkWidget *input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), input_box, FALSE, FALSE, 0);

    GtkWidget *entry_label = gtk_label_new("Message:");
    gtk_box_pack_start(GTK_BOX(input_box), entry_label, FALSE, FALSE, 0);

    win->text_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->text_entry), "Type something here...");
    gtk_box_pack_start(GTK_BOX(input_box), win->text_entry, TRUE, TRUE, 0);

    // Button row
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 0);

    win->send_button = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(button_box), win->send_button, TRUE, TRUE, 0);
    g_signal_connect(win->send_button, "clicked", G_CALLBACK(on_send_clicked), win);

    win->clear_button = gtk_button_new_with_label("Clear");
    gtk_box_pack_start(GTK_BOX(button_box), win->clear_button, TRUE, TRUE, 0);
    g_signal_connect(win->clear_button, "clicked", G_CALLBACK(on_clear_clicked), win);

    // Output label (expands to fill space)
    GtkWidget *output_frame = gtk_frame_new("Output");
    gtk_box_pack_start(GTK_BOX(vbox), output_frame, TRUE, TRUE, 0);

    win->output_label = gtk_label_new("(no output yet)");
    gtk_label_set_line_wrap(GTK_LABEL(win->output_label), TRUE);
    gtk_widget_set_halign(win->output_label, GTK_ALIGN_START);
    gtk_widget_set_valign(win->output_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(win->output_label, 8);
    gtk_widget_set_margin_top(win->output_label, 8);
    gtk_container_add(GTK_CONTAINER(output_frame), win->output_label);

    // Status bar
    win->statusbar = gtk_statusbar_new();
    win->statusbar_context = gtk_statusbar_get_context_id(
        GTK_STATUSBAR(win->statusbar), "main");
    gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                       "Ready");
    gtk_box_pack_end(GTK_BOX(vbox), win->statusbar, FALSE, FALSE, 0);

    return win;
}
