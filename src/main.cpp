#include <gtk/gtk.h>
#include "app_window.h"

static void on_activate(GtkApplication *app, gpointer /*user_data*/) {
    AppWindow *win = app_window_new(app);
    gtk_widget_show_all(win->window);
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new(
        "org.freedv.monitor", G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}
