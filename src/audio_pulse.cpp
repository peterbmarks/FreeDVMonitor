#include "audio_backend.h"

#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdio>

/* ── PulseAudio capture ────────────────────────────────────────────── */

class PulseCapture : public AudioCapture {
public:
    ~PulseCapture() override { close(); }

    bool open(const std::string& device_id, int sample_rate, int channels) override {
        close();

        pa_sample_spec spec{};
        spec.format   = PA_SAMPLE_FLOAT32LE;
        spec.rate     = static_cast<uint32_t>(sample_rate);
        spec.channels = static_cast<uint8_t>(channels);

        int err = 0;
        const char* dev = device_id.empty() ? nullptr : device_id.c_str();
        pa_ = pa_simple_new(nullptr, "FreeDV Monitor", PA_STREAM_RECORD,
                            dev, "Capture", &spec, nullptr, nullptr, &err);
        if (!pa_) {
            fprintf(stderr, "PulseAudio capture open failed: %s\n", pa_strerror(err));
            return false;
        }
        fprintf(stderr, "PulseAudio capture: %s, %d Hz, float32\n",
                device_id.empty() ? "(default)" : device_id.c_str(), sample_rate);
        return true;
    }

    int read(float* buffer, int frames) override {
        if (!pa_) return -1;
        int err = 0;
        int ret = pa_simple_read(pa_, buffer,
                                 static_cast<size_t>(frames) * sizeof(float), &err);
        if (ret < 0) {
            fprintf(stderr, "PulseAudio read error: %s\n", pa_strerror(err));
            return -1;
        }
        return 0;
    }

    void close() override {
        if (pa_) { pa_simple_free(pa_); pa_ = nullptr; }
    }

private:
    pa_simple* pa_ = nullptr;
};

/* ── PulseAudio playback ───────────────────────────────────────────── */

class PulsePlayback : public AudioPlayback {
public:
    ~PulsePlayback() override { close(); }

    bool open(int sample_rate, int channels) override {
        close();

        pa_sample_spec spec{};
        spec.format   = PA_SAMPLE_FLOAT32LE;
        spec.rate     = static_cast<uint32_t>(sample_rate);
        spec.channels = static_cast<uint8_t>(channels);

        int err = 0;
        pa_ = pa_simple_new(nullptr, "FreeDV Monitor", PA_STREAM_PLAYBACK,
                            nullptr, "Playback", &spec, nullptr, nullptr, &err);
        if (!pa_) {
            fprintf(stderr, "PulseAudio playback open failed: %s\n", pa_strerror(err));
            return false;
        }
        return true;
    }

    int write(const float* buffer, int frames) override {
        if (!pa_) return -1;
        int err = 0;
        int ret = pa_simple_write(pa_, buffer,
                                  static_cast<size_t>(frames) * sizeof(float), &err);
        return (ret < 0) ? -1 : 0;
    }

    void flush() override {
        if (pa_) pa_simple_flush(pa_, nullptr);
    }

    void close() override {
        if (pa_) { pa_simple_free(pa_); pa_ = nullptr; }
    }

private:
    pa_simple* pa_ = nullptr;
};

/* ── Device enumeration ────────────────────────────────────────────── */

struct PulseEnumData {
    std::vector<AudioDevice> devices;
    pa_threaded_mainloop *ml;
};

static void source_info_cb(pa_context* /*ctx*/, const pa_source_info* info,
                           int eol, void* userdata)
{
    auto* data = static_cast<PulseEnumData*>(userdata);
    if (eol > 0) {
        pa_threaded_mainloop_signal(data->ml, 0);
        return;
    }
    if (!info) return;

    // Skip monitor sources (they capture playback output, not real inputs)
    if (info->monitor_of_sink != PA_INVALID_INDEX)
        return;

    AudioDevice dev;
    dev.id = info->name;
    dev.description = info->description ? info->description : info->name;
    data->devices.push_back(std::move(dev));
}

static void context_state_cb(pa_context* ctx, void* userdata)
{
    auto* ml = static_cast<pa_threaded_mainloop*>(userdata);
    pa_context_state_t state = pa_context_get_state(ctx);
    if (state == PA_CONTEXT_READY || state == PA_CONTEXT_FAILED ||
        state == PA_CONTEXT_TERMINATED)
        pa_threaded_mainloop_signal(ml, 0);
}

std::vector<AudioDevice> audio_enumerate_inputs()
{
    PulseEnumData enum_data{};

    pa_threaded_mainloop* ml = pa_threaded_mainloop_new();
    if (!ml) return {};

    enum_data.ml = ml;

    pa_mainloop_api* api = pa_threaded_mainloop_get_api(ml);
    pa_context* ctx = pa_context_new(api, "FreeDV Monitor");
    if (!ctx) {
        pa_threaded_mainloop_free(ml);
        return {};
    }

    pa_context_set_state_callback(ctx, context_state_cb, ml);

    pa_threaded_mainloop_lock(ml);
    pa_threaded_mainloop_start(ml);

    pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr);

    // Wait for context to become ready
    while (true) {
        pa_context_state_t state = pa_context_get_state(ctx);
        if (state == PA_CONTEXT_READY) break;
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            pa_context_unref(ctx);
            pa_threaded_mainloop_unlock(ml);
            pa_threaded_mainloop_stop(ml);
            pa_threaded_mainloop_free(ml);
            return {};
        }
        pa_threaded_mainloop_wait(ml);
    }

    // Enumerate sources
    pa_operation* op = pa_context_get_source_info_list(ctx, source_info_cb, &enum_data);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
        pa_threaded_mainloop_wait(ml);
    pa_operation_unref(op);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);

    pa_threaded_mainloop_unlock(ml);
    pa_threaded_mainloop_stop(ml);
    pa_threaded_mainloop_free(ml);

    return enum_data.devices;
}

/* ── Factory functions ─────────────────────────────────────────────── */

std::unique_ptr<AudioCapture> audio_create_capture() {
    return std::make_unique<PulseCapture>();
}

std::unique_ptr<AudioPlayback> audio_create_playback() {
    return std::make_unique<PulsePlayback>();
}
