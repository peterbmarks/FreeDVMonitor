#ifndef APP_WINDOW_H
#define APP_WINDOW_H

#include <gtk/gtk.h>

struct AppWindow {
    GtkWidget *window;
    GtkWidget *header_label;
    GtkWidget *audio_combo;
    GtkWidget *refresh_button;
    GtkWidget *text_entry;
    GtkWidget *send_button;
    GtkWidget *clear_button;
    GtkWidget *output_label;
    GtkWidget *statusbar;
    guint statusbar_context;
};

AppWindow *app_window_new(GtkApplication *app);

#endif
