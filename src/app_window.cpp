#include "app_window.h"
#include <portaudio.h>
#include <string>
#ifdef __linux__
#include <unistd.h>
#include <fcntl.h>
#endif

// Suppress noisy ALSA warnings on Linux (e.g. "Unable to find definition")
static int saved_stderr_fd = -1;

static void suppress_stderr(bool suppress) {
#ifdef __linux__
    if (suppress) {
        fflush(stderr);
        saved_stderr_fd = dup(STDERR_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    } else if (saved_stderr_fd >= 0) {
        fflush(stderr);
        dup2(saved_stderr_fd, STDERR_FILENO);
        close(saved_stderr_fd);
        saved_stderr_fd = -1;
    }
#else
    (void)suppress;
#endif
}

static void populate_audio_inputs(AppWindow *win) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(win->audio_combo));

    suppress_stderr(true);
    PaError err = Pa_Initialize();
    suppress_stderr(false);
    if (err != paNoError) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(win->audio_combo),
                                       "(PortAudio init failed)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(win->audio_combo), 0);
        return;
    }

    int count = Pa_GetDeviceCount();
    int added = 0;
    for (int i = 0; i < count; i++) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (info && info->maxInputChannels > 0) {
            gtk_combo_box_text_append_text(
                GTK_COMBO_BOX_TEXT(win->audio_combo), info->name);
            added++;
        }
    }

    Pa_Terminate();

    if (added == 0) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(win->audio_combo),
                                       "(no input devices found)");
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(win->audio_combo), 0);
}

static void on_audio_combo_changed(GtkComboBox *combo, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    gchar *text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (text) {
        std::string msg = "Audio input: ";
        msg += text;
        gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                           msg.c_str());
        g_free(text);
    }
}

static void on_refresh_clicked(GtkWidget * /*widget*/, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    populate_audio_inputs(win);
    gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                       "Audio devices refreshed");
}

static void on_start_clicked(GtkWidget * /*widget*/, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);

    if (win->decoder.is_running()) {
        win->decoder.stop();
        win->decoder.close();
        gtk_button_set_label(GTK_BUTTON(win->start_button), "Start");
        gtk_widget_set_sensitive(win->audio_combo, TRUE);
        gtk_widget_set_sensitive(win->refresh_button, TRUE);
        gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                           "Decoder stopped");
        return;
    }

    gchar *text = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(win->audio_combo));
    if (!text) {
        gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                           "No audio input selected");
        return;
    }

    std::string dev_name(text);
    g_free(text);

    /* Look up PortAudio device index by name */
    suppress_stderr(true);
    Pa_Initialize();
    suppress_stderr(false);
    int dev_index = RadaeDecoder::find_device_by_name(dev_name);
    Pa_Terminate();

    if (dev_index == paNoDevice) {
        gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                           "Audio device not found");
        return;
    }

    if (!win->decoder.open(dev_index)) {
        gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                           "Failed to open audio streams");
        return;
    }

    win->decoder.start();
    gtk_button_set_label(GTK_BUTTON(win->start_button), "Stop");
    gtk_widget_set_sensitive(win->audio_combo, FALSE);
    gtk_widget_set_sensitive(win->refresh_button, FALSE);
    gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                       "Decoder running...");
}

static void on_window_destroy(GtkWidget * /*widget*/, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    win->decoder.stop();
    win->decoder.close();
    delete win;
}

AppWindow *app_window_new(GtkApplication *app) {
    auto *win = new AppWindow{};

    // Main window
    win->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win->window), "FreeDV Monitor");
    gtk_window_set_default_size(GTK_WINDOW(win->window), 480, 360);
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

    // Audio input row: label + combo + refresh button + start button
    GtkWidget *audio_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), audio_box, FALSE, FALSE, 0);

    GtkWidget *audio_label = gtk_label_new("Audio Input:");
    gtk_box_pack_start(GTK_BOX(audio_box), audio_label, FALSE, FALSE, 0);

    win->audio_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(win->audio_combo, 50, -1);  // allow shrinking
    gtk_box_pack_start(GTK_BOX(audio_box), win->audio_combo, TRUE, TRUE, 0);
    g_signal_connect(win->audio_combo, "changed", G_CALLBACK(on_audio_combo_changed), win);

    win->refresh_button = gtk_button_new_with_label("Refresh");
    gtk_box_pack_start(GTK_BOX(audio_box), win->refresh_button, FALSE, FALSE, 0);
    g_signal_connect(win->refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), win);

    win->start_button = gtk_button_new_with_label("Start");
    gtk_box_pack_start(GTK_BOX(audio_box), win->start_button, FALSE, FALSE, 0);
    g_signal_connect(win->start_button, "clicked", G_CALLBACK(on_start_clicked), win);

    populate_audio_inputs(win);

    // Status bar
    win->statusbar = gtk_statusbar_new();
    win->statusbar_context = gtk_statusbar_get_context_id(
        GTK_STATUSBAR(win->statusbar), "main");
    gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                       "Ready");
    gtk_box_pack_end(GTK_BOX(vbox), win->statusbar, FALSE, FALSE, 0);

    return win;
}
