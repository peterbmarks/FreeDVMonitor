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

    // Input gain slider
    GtkWidget *gain_slider         = nullptr;

    // Waterfall spectrum display
    GtkWidget *waterfall_area      = nullptr;
    GtkWidget *freq_scale_area     = nullptr;
    guchar    *waterfall_pixels    = nullptr;
    int        waterfall_width     = 0;
    int        waterfall_height    = 0;
    guint      waterfall_timer_id  = 0;

    // Status bar update timer
    guint      status_timer_id    = 0;
};

AppWindow *app_window_new(GtkApplication *app);

#endif
