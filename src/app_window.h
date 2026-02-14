#ifndef APP_WINDOW_H
#define APP_WINDOW_H

#include <gtk/gtk.h>
#include "rade_decoder.h"

struct AppWindow {
    GtkWidget *window;
    GtkWidget *header_label;
    GtkWidget *audio_combo;
    GtkWidget *refresh_button;
    GtkWidget *start_button;
    GtkWidget *statusbar;
    guint statusbar_context;

    RadaeDecoder decoder;
};

AppWindow *app_window_new(GtkApplication *app);

#endif
