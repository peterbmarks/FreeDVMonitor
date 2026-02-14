#include "app_window.h"
#include <portaudio.h>
#include <string>
#include <cstring>
#include <cmath>
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

/* ── Configuration persistence ─────────────────────────────────────── */

static std::string config_path() {
    const gchar *dir = g_get_user_config_dir();  // ~/.config
    std::string path = std::string(dir) + "/FreeDVMonitor";
    g_mkdir_with_parents(path.c_str(), 0755);
    return path + "/settings.ini";
}

static void config_save_audio_device(const char *device_name) {
    GKeyFile *kf = g_key_file_new();
    std::string path = config_path();
    g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr);
    g_key_file_set_string(kf, "audio", "input_device", device_name);
    g_key_file_save_to_file(kf, path.c_str(), nullptr);
    g_key_file_free(kf);
}

static std::string config_load_audio_device() {
    GKeyFile *kf = g_key_file_new();
    std::string path = config_path();
    std::string result;
    if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        gchar *val = g_key_file_get_string(kf, "audio", "input_device", nullptr);
        if (val) {
            result = val;
            g_free(val);
        }
    }
    g_key_file_free(kf);
    return result;
}

static void config_save_input_gain(double gain_dB) {
    GKeyFile *kf = g_key_file_new();
    std::string path = config_path();
    g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr);
    g_key_file_set_double(kf, "audio", "input_gain_dB", gain_dB);
    g_key_file_save_to_file(kf, path.c_str(), nullptr);
    g_key_file_free(kf);
}

static double config_load_input_gain() {
    GKeyFile *kf = g_key_file_new();
    std::string path = config_path();
    double result = 0.0;  // 0 dB = unity gain
    if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        GError *err = nullptr;
        double val = g_key_file_get_double(kf, "audio", "input_gain_dB", &err);
        if (!err)
            result = val;
        else
            g_error_free(err);
    }
    g_key_file_free(kf);
    return result;
}

/* ── Waterfall spectrum display ─────────────────────────────────────── */

static void db_to_rgb(float dB, guchar *r, guchar *g, guchar *b) {
    float t = (dB + 100.0f) / 60.0f;  // -100 dB → 0, -40 dB → 1
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    if (t < 0.2f) {
        float s = t / 0.2f;
        *r = 0; *g = 0; *b = (guchar)(s * 255);
    } else if (t < 0.4f) {
        float s = (t - 0.2f) / 0.2f;
        *r = 0; *g = (guchar)(s * 255); *b = 255;
    } else if (t < 0.6f) {
        float s = (t - 0.4f) / 0.2f;
        *r = (guchar)(s * 255); *g = 255; *b = (guchar)((1.0f - s) * 255);
    } else if (t < 0.8f) {
        float s = (t - 0.6f) / 0.2f;
        *r = 255; *g = (guchar)((1.0f - s) * 255); *b = 0;
    } else {
        float s = (t - 0.8f) / 0.2f;
        *r = 255; *g = (guchar)(s * 255); *b = (guchar)(s * 255);
    }
}

static void on_waterfall_size_allocate(GtkWidget * /*widget*/,
                                       GdkRectangle *allocation,
                                       gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    int new_w = allocation->width;
    int new_h = allocation->height;
    if (new_w == win->waterfall_width && new_h == win->waterfall_height)
        return;
    g_free(win->waterfall_pixels);
    win->waterfall_width  = new_w;
    win->waterfall_height = new_h;
    win->waterfall_pixels = (guchar *)g_malloc0(new_w * new_h * 3);
}

static gboolean on_waterfall_draw(GtkWidget * /*widget*/, cairo_t *cr,
                                  gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    int w = win->waterfall_width;
    int h = win->waterfall_height;
    if (!win->waterfall_pixels || w <= 0 || h <= 0)
        return FALSE;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    unsigned char *surf_data = cairo_image_surface_get_data(surf);
    int surf_stride = cairo_image_surface_get_stride(surf);
    cairo_surface_flush(surf);

    for (int y = 0; y < h; y++) {
        guchar *src = win->waterfall_pixels + y * w * 3;
        guchar *dst = surf_data + y * surf_stride;
        for (int x = 0; x < w; x++) {
            guint32 *pixel = (guint32 *)(dst + x * 4);
            *pixel = ((guint32)src[0] << 16) |
                     ((guint32)src[1] << 8)  |
                      (guint32)src[2];
            src += 3;
        }
    }

    cairo_surface_mark_dirty(surf);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(surf);
    return TRUE;
}

static gboolean on_waterfall_timer(gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    int w = win->waterfall_width;
    int h = win->waterfall_height;
    if (!win->waterfall_pixels || w <= 0 || h <= 0)
        return G_SOURCE_CONTINUE;

    int row_bytes = w * 3;

    // Shift rows down by one
    if (h > 1)
        std::memmove(win->waterfall_pixels + row_bytes,
                     win->waterfall_pixels,
                     row_bytes * (h - 1));

    // Get current spectrum
    float spectrum[RadaeDecoder::SPECTRUM_BINS];
    win->decoder.get_spectrum(spectrum, RadaeDecoder::SPECTRUM_BINS);

    // Paint new row at top
    guchar *row = win->waterfall_pixels;
    for (int x = 0; x < w; x++) {
        int bin = x * RadaeDecoder::SPECTRUM_BINS / w;
        if (bin >= RadaeDecoder::SPECTRUM_BINS) bin = RadaeDecoder::SPECTRUM_BINS - 1;
        db_to_rgb(spectrum[bin], &row[x * 3], &row[x * 3 + 1], &row[x * 3 + 2]);
    }

    gtk_widget_queue_draw(win->waterfall_area);
    return G_SOURCE_CONTINUE;
}

/* ── Status bar update timer ────────────────────────────────────────── */

static gboolean on_status_timer(gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    if (!win->decoder.is_running())
        return G_SOURCE_CONTINUE;

    char buf[128];
    if (win->decoder.is_synced()) {
        snprintf(buf, sizeof(buf), "SYNC | SNR: %.1f dB | Freq Offset: %.1f Hz",
                 win->decoder.snr_dB(), win->decoder.freq_offset());
    } else {
        snprintf(buf, sizeof(buf), "Searching...");
    }

    gtk_statusbar_pop(GTK_STATUSBAR(win->statusbar), win->statusbar_context);
    gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context, buf);
    return G_SOURCE_CONTINUE;
}

static void status_timer_start(AppWindow *win) {
    if (win->status_timer_id == 0)
        win->status_timer_id = g_timeout_add(250, on_status_timer, win);
}

static void status_timer_stop(AppWindow *win) {
    if (win->status_timer_id != 0) {
        g_source_remove(win->status_timer_id);
        win->status_timer_id = 0;
    }
}

static void waterfall_timer_start(AppWindow *win) {
    if (win->waterfall_timer_id == 0)
        win->waterfall_timer_id = g_timeout_add(50, on_waterfall_timer, win);
}

static void waterfall_timer_stop(AppWindow *win) {
    if (win->waterfall_timer_id != 0) {
        g_source_remove(win->waterfall_timer_id);
        win->waterfall_timer_id = 0;
    }
}

/* ── Input gain slider ─────────────────────────────────────────────── */

static void on_gain_slider_changed(GtkRange *range, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    double dB = gtk_range_get_value(range);
    float linear = static_cast<float>(std::pow(10.0, dB / 20.0));
    win->decoder.set_input_gain(linear);
    config_save_input_gain(dB);
}

/* ── Audio device helpers ──────────────────────────────────────────── */

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

    std::string saved = config_load_audio_device();
    int saved_index = -1;

    int count = Pa_GetDeviceCount();
    int added = 0;
    for (int i = 0; i < count; i++) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (info && info->maxInputChannels > 0) {
            gtk_combo_box_text_append_text(
                GTK_COMBO_BOX_TEXT(win->audio_combo), info->name);
            if (!saved.empty() && saved == info->name)
                saved_index = added;
            added++;
        }
    }

    Pa_Terminate();

    if (added == 0) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(win->audio_combo),
                                       "(no input devices found)");
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(win->audio_combo),
                             saved_index >= 0 ? saved_index : 0);
}

static void on_audio_combo_changed(GtkComboBox *combo, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    gchar *text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (text) {
        std::string msg = "Audio input: ";
        msg += text;
        gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                           msg.c_str());
        config_save_audio_device(text);
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
        status_timer_stop(win);
        waterfall_timer_stop(win);
        win->decoder.stop();
        win->decoder.close();
        gtk_button_set_label(GTK_BUTTON(win->start_button), "Start");
        gtk_widget_set_sensitive(win->audio_combo, TRUE);
        gtk_widget_set_sensitive(win->refresh_button, TRUE);
        gtk_statusbar_pop(GTK_STATUSBAR(win->statusbar), win->statusbar_context);
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
    waterfall_timer_start(win);
    status_timer_start(win);
    gtk_button_set_label(GTK_BUTTON(win->start_button), "Stop");
    gtk_widget_set_sensitive(win->audio_combo, FALSE);
    gtk_widget_set_sensitive(win->refresh_button, FALSE);
}

static void on_window_destroy(GtkWidget * /*widget*/, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    status_timer_stop(win);
    waterfall_timer_stop(win);
    win->decoder.stop();
    win->decoder.close();
    g_free(win->waterfall_pixels);
    win->waterfall_pixels = nullptr;
    delete win;
}

/* ── Menu callbacks ─────────────────────────────────────────────────── */

static void stop_decoder(AppWindow *win) {
    if (win->decoder.is_running()) {
        status_timer_stop(win);
        waterfall_timer_stop(win);
        win->decoder.stop();
        win->decoder.close();
        gtk_button_set_label(GTK_BUTTON(win->start_button), "Start");
        gtk_widget_set_sensitive(win->audio_combo, TRUE);
        gtk_widget_set_sensitive(win->refresh_button, TRUE);
    }
}

static void on_open_wav(GtkMenuItem * /*item*/, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Open WAV File",
        GTK_WINDOW(win->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        nullptr);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "WAV files");
    gtk_file_filter_add_pattern(filter, "*.wav");
    gtk_file_filter_add_pattern(filter, "*.WAV");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_widget_destroy(dialog);

        stop_decoder(win);

        if (!win->decoder.open_file(filename)) {
            gtk_statusbar_pop(GTK_STATUSBAR(win->statusbar), win->statusbar_context);
            gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                               "Failed to open WAV file");
            g_free(filename);
            return;
        }

        win->decoder.start();
        waterfall_timer_start(win);
        status_timer_start(win);
        gtk_button_set_label(GTK_BUTTON(win->start_button), "Stop");
        gtk_widget_set_sensitive(win->audio_combo, FALSE);
        gtk_widget_set_sensitive(win->refresh_button, FALSE);

        gchar *basename = g_path_get_basename(filename);
        char msg[256];
        snprintf(msg, sizeof(msg), "Playing: %s", basename);
        gtk_statusbar_pop(GTK_STATUSBAR(win->statusbar), win->statusbar_context);
        gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context, msg);
        g_free(basename);
        g_free(filename);
    } else {
        gtk_widget_destroy(dialog);
    }
}

static void on_menu_exit(GtkMenuItem * /*item*/, gpointer data) {
    auto *win = static_cast<AppWindow *>(data);
    gtk_widget_destroy(win->window);
}

AppWindow *app_window_new(GtkApplication *app) {
    auto *win = new AppWindow{};

    // Main window
    win->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win->window), "FreeDV Monitor");
    gtk_window_set_default_size(GTK_WINDOW(win->window), 480, 500);
    g_signal_connect(win->window, "destroy", G_CALLBACK(on_window_destroy), win);

    // Outer vertical box (no padding — menubar sits flush against window edges)
    GtkWidget *outer_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win->window), outer_vbox);

    // Menu bar
    GtkWidget *menubar = gtk_menu_bar_new();

    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *file_item = gtk_menu_item_new_with_label("File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);

    GtkWidget *open_wav_item = gtk_menu_item_new_with_label("Open WAV...");
    g_signal_connect(open_wav_item, "activate", G_CALLBACK(on_open_wav), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_wav_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), gtk_separator_menu_item_new());

    GtkWidget *exit_item = gtk_menu_item_new_with_label("Exit");
    g_signal_connect(exit_item, "activate", G_CALLBACK(on_menu_exit), win);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), exit_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);
    gtk_box_pack_start(GTK_BOX(outer_vbox), menubar, FALSE, FALSE, 0);

    // Content area with padding below the menubar
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_box_pack_start(GTK_BOX(outer_vbox), vbox, TRUE, TRUE, 0);

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

    // Waterfall + gain slider row
    GtkWidget *waterfall_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), waterfall_box, TRUE, TRUE, 0);

    // Vertical gain slider (left edge) — range -40 to +20 dB, default from config
    win->gain_slider = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -40.0, 20.0, 1.0);
    gtk_scale_set_value_pos(GTK_SCALE(win->gain_slider), GTK_POS_BOTTOM);
    gtk_widget_set_size_request(win->gain_slider, -1, -1);
    gtk_range_set_inverted(GTK_RANGE(win->gain_slider), TRUE);
    double saved_gain = config_load_input_gain();
    gtk_range_set_value(GTK_RANGE(win->gain_slider), saved_gain);
    win->decoder.set_input_gain(static_cast<float>(std::pow(10.0, saved_gain / 20.0)));
    g_signal_connect(win->gain_slider, "value-changed",
                     G_CALLBACK(on_gain_slider_changed), win);
    gtk_box_pack_start(GTK_BOX(waterfall_box), win->gain_slider, FALSE, FALSE, 0);

    // Waterfall spectrum display
    win->waterfall_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(win->waterfall_area, -1, 200);
    gtk_box_pack_start(GTK_BOX(waterfall_box), win->waterfall_area, TRUE, TRUE, 0);
    g_signal_connect(win->waterfall_area, "draw",
                     G_CALLBACK(on_waterfall_draw), win);
    g_signal_connect(win->waterfall_area, "size-allocate",
                     G_CALLBACK(on_waterfall_size_allocate), win);

    // Status bar
    win->statusbar = gtk_statusbar_new();
    win->statusbar_context = gtk_statusbar_get_context_id(
        GTK_STATUSBAR(win->statusbar), "main");
    gtk_statusbar_push(GTK_STATUSBAR(win->statusbar), win->statusbar_context,
                       "Ready");
    gtk_box_pack_end(GTK_BOX(vbox), win->statusbar, FALSE, FALSE, 0);

    return win;
}
