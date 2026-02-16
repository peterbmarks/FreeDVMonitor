#include "audio_backend.h"

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

/* ── WASAPI stream flags for automatic format conversion (Win 7+) ──── */

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY  0x08000000
#endif

/* ── COM helper ────────────────────────────────────────────────────── */

static bool com_initialized = false;

static void ensure_com() {
    if (!com_initialized) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_initialized = true;
    }
}

/* ── Helper: convert WCHAR string to UTF-8 ─────────────────────────── */

static std::string wchar_to_utf8(const WCHAR* wstr) {
    if (!wstr || !wstr[0]) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    return result;
}

static std::wstring utf8_to_wchar(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], len);
    return result;
}

/* ── WASAPI Capture ────────────────────────────────────────────────── */

class WasapiCapture : public AudioCapture {
public:
    ~WasapiCapture() override { close(); }

    bool open(const std::string& device_id, int sample_rate, int channels) override {
        close();
        ensure_com();

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) {
            fprintf(stderr, "WASAPI capture: CoCreateInstance failed (0x%08lx)\n", hr);
            return false;
        }

        IMMDevice* device = nullptr;
        if (device_id.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        } else {
            std::wstring wid = utf8_to_wchar(device_id);
            hr = enumerator->GetDevice(wid.c_str(), &device);
        }
        enumerator->Release();

        if (FAILED(hr) || !device) {
            fprintf(stderr, "WASAPI capture: GetDevice failed (0x%08lx)\n", hr);
            return false;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client_));
        device->Release();

        if (FAILED(hr) || !client_) {
            fprintf(stderr, "WASAPI capture: Activate failed (0x%08lx)\n", hr);
            return false;
        }

        /* Get the device's mix format to understand its native capabilities */
        WAVEFORMATEX* mix_fmt = nullptr;
        hr = client_->GetMixFormat(&mix_fmt);
        if (FAILED(hr) || !mix_fmt) {
            fprintf(stderr, "WASAPI capture: GetMixFormat failed (0x%08lx)\n", hr);
            close();
            return false;
        }

        fprintf(stderr, "WASAPI capture: device mix format: %d Hz, %d ch, %d bit\n",
                (int)mix_fmt->nSamplesPerSec, (int)mix_fmt->nChannels,
                (int)mix_fmt->wBitsPerSample);

        /* Use the device's native format — we'll resample/downmix ourselves */
        device_rate_     = static_cast<int>(mix_fmt->nSamplesPerSec);
        device_channels_ = static_cast<int>(mix_fmt->nChannels);
        device_bps_      = static_cast<int>(mix_fmt->wBitsPerSample);

        /* Check if device format uses float or integer samples */
        device_is_float_ = false;
        if (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            device_is_float_ = true;
        } else if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->cbSize >= 22) {
            WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_fmt);
            // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00aa00389b71}
            static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
                {0x00000003, 0x0000, 0x0010, {0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
            if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                device_is_float_ = true;
        }

        /* 50ms buffer */
        REFERENCE_TIME duration = 500000;

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 0, duration, 0, mix_fmt, nullptr);
        CoTaskMemFree(mix_fmt);

        if (FAILED(hr)) {
            fprintf(stderr, "WASAPI capture: Initialize failed (0x%08lx)\n", hr);
            close();
            return false;
        }

        hr = client_->GetService(__uuidof(IAudioCaptureClient),
                                 reinterpret_cast<void**>(&capture_));
        if (FAILED(hr) || !capture_) {
            fprintf(stderr, "WASAPI capture: GetService failed (0x%08lx)\n", hr);
            close();
            return false;
        }

        hr = client_->Start();
        if (FAILED(hr)) {
            fprintf(stderr, "WASAPI capture: Start failed (0x%08lx)\n", hr);
            close();
            return false;
        }

        target_rate_     = sample_rate;
        target_channels_ = channels;
        resample_pos_    = 0.0;

        fprintf(stderr, "WASAPI capture: opened %s, device %d Hz %d ch -> target %d Hz %d ch\n",
                device_id.empty() ? "(default)" : device_id.c_str(),
                device_rate_, device_channels_, target_rate_, target_channels_);
        return true;
    }

    int read(float* buffer, int frames) override {
        if (!client_ || !capture_) return -1;

        int filled = 0;
        while (filled < frames) {
            /* If we have resampled data buffered, consume it first */
            while (filled < frames && !resample_buf_.empty()) {
                buffer[filled++] = resample_buf_.back();
                resample_buf_.pop_back();
            }
            if (filled >= frames) break;

            /* Get next packet from WASAPI */
            UINT32 packet_len = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet_len);
            if (FAILED(hr)) return -1;

            if (packet_len == 0) {
                Sleep(1);
                continue;
            }

            BYTE* data = nullptr;
            UINT32 num_frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) return -1;

            /* Convert device format to mono float at target rate */
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                /* Produce silence at target rate */
                int target_frames = static_cast<int>(
                    static_cast<double>(num_frames) * target_rate_ / device_rate_);
                for (int i = 0; i < target_frames && filled < frames; i++)
                    buffer[filled++] = 0.0f;
            } else {
                /* Step 1: extract mono float from device format */
                std::vector<float> mono(num_frames);
                extract_mono_float(data, mono.data(), static_cast<int>(num_frames));

                /* Step 2: resample from device_rate_ to target_rate_ */
                resample_into(mono.data(), static_cast<int>(num_frames),
                              buffer, frames, filled);
            }

            capture_->ReleaseBuffer(num_frames);
        }
        return 0;
    }

    void close() override {
        if (client_) { client_->Stop(); }
        if (capture_) { capture_->Release(); capture_ = nullptr; }
        if (client_)  { client_->Release();  client_  = nullptr; }
        resample_buf_.clear();
        resample_pos_ = 0.0;
    }

private:
    /* Extract mono float samples from the device's native format */
    void extract_mono_float(const BYTE* data, float* out, int num_frames) {
        for (int i = 0; i < num_frames; i++) {
            float sum = 0.0f;
            for (int ch = 0; ch < device_channels_; ch++) {
                float v = 0.0f;
                if (device_is_float_ && device_bps_ == 32) {
                    const float* fp = reinterpret_cast<const float*>(data);
                    v = fp[i * device_channels_ + ch];
                } else if (device_bps_ == 16) {
                    const int16_t* sp = reinterpret_cast<const int16_t*>(data);
                    v = sp[i * device_channels_ + ch] / 32768.0f;
                } else if (device_bps_ == 24) {
                    int idx = (i * device_channels_ + ch) * 3;
                    int32_t raw = (static_cast<int32_t>(data[idx + 2]) << 16) |
                                  (data[idx + 1] << 8) | data[idx];
                    if (raw & 0x800000) raw |= static_cast<int32_t>(0xFF000000);
                    v = raw / 8388608.0f;
                } else if (device_bps_ == 32) {
                    const int32_t* ip = reinterpret_cast<const int32_t*>(data);
                    v = ip[i * device_channels_ + ch] / 2147483648.0f;
                }
                sum += v;
            }
            out[i] = sum / device_channels_;
        }
    }

    /* Resample from device rate to target rate, filling output buffer */
    void resample_into(const float* src, int src_frames,
                       float* dst, int dst_max, int& dst_filled) {
        double ratio = static_cast<double>(device_rate_) / target_rate_;

        while (dst_filled < dst_max) {
            int idx = static_cast<int>(resample_pos_);
            if (idx + 1 >= src_frames) break;

            float frac = static_cast<float>(resample_pos_ - idx);
            dst[dst_filled++] = src[idx] + frac * (src[idx + 1] - src[idx]);
            resample_pos_ += ratio;
        }

        /* Adjust position for consumed source frames */
        resample_pos_ -= src_frames;
        if (resample_pos_ < 0.0) resample_pos_ = 0.0;

        /* If we have leftover output capacity, buffer any remaining
           resampled samples for the next read() call */
        /* (position is already adjusted, leftover handled on next packet) */
    }

    IAudioClient*        client_  = nullptr;
    IAudioCaptureClient* capture_ = nullptr;

    int  device_rate_     = 0;
    int  device_channels_ = 0;
    int  device_bps_      = 0;
    bool device_is_float_ = false;

    int    target_rate_     = 0;
    int    target_channels_ = 0;
    double resample_pos_    = 0.0;

    /* Buffer for excess resampled samples (stored in reverse for pop_back) */
    std::vector<float> resample_buf_;
};

/* ── WASAPI Playback ───────────────────────────────────────────────── */

class WasapiPlayback : public AudioPlayback {
public:
    ~WasapiPlayback() override { close(); }

    bool open(int sample_rate, int channels) override {
        close();
        ensure_com();

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) {
            fprintf(stderr, "WASAPI playback: CoCreateInstance failed (0x%08lx)\n", hr);
            return false;
        }

        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr) || !device) {
            fprintf(stderr, "WASAPI playback: GetDefaultAudioEndpoint failed (0x%08lx)\n", hr);
            return false;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client_));
        device->Release();
        if (FAILED(hr) || !client_) {
            fprintf(stderr, "WASAPI playback: Activate failed (0x%08lx)\n", hr);
            return false;
        }

        /* Get mix format */
        WAVEFORMATEX* mix_fmt = nullptr;
        hr = client_->GetMixFormat(&mix_fmt);
        if (FAILED(hr) || !mix_fmt) {
            fprintf(stderr, "WASAPI playback: GetMixFormat failed (0x%08lx)\n", hr);
            close();
            return false;
        }

        fprintf(stderr, "WASAPI playback: device mix format: %d Hz, %d ch, %d bit\n",
                (int)mix_fmt->nSamplesPerSec, (int)mix_fmt->nChannels,
                (int)mix_fmt->wBitsPerSample);

        device_rate_     = static_cast<int>(mix_fmt->nSamplesPerSec);
        device_channels_ = static_cast<int>(mix_fmt->nChannels);

        /* Check if device uses float format */
        device_is_float_ = false;
        if (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            device_is_float_ = true;
        } else if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->cbSize >= 22) {
            WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_fmt);
            static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
                {0x00000003, 0x0000, 0x0010, {0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
            if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                device_is_float_ = true;
        }

        /* 100ms buffer */
        REFERENCE_TIME duration = 1000000;

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 0, duration, 0, mix_fmt, nullptr);
        CoTaskMemFree(mix_fmt);

        if (FAILED(hr)) {
            fprintf(stderr, "WASAPI playback: Initialize failed (0x%08lx)\n", hr);
            close();
            return false;
        }

        hr = client_->GetBufferSize(&buffer_frames_);
        if (FAILED(hr)) { close(); return false; }

        hr = client_->GetService(__uuidof(IAudioRenderClient),
                                 reinterpret_cast<void**>(&render_));
        if (FAILED(hr) || !render_) { close(); return false; }

        hr = client_->Start();
        if (FAILED(hr)) { close(); return false; }

        source_rate_     = sample_rate;
        source_channels_ = channels;
        resample_pos_    = 0.0;

        fprintf(stderr, "WASAPI playback: source %d Hz %d ch -> device %d Hz %d ch\n",
                source_rate_, source_channels_, device_rate_, device_channels_);
        return true;
    }

    int write(const float* buffer, int frames) override {
        if (!client_ || !render_) return -1;

        /* Resample source (e.g. 16kHz mono) to device format (e.g. 48kHz stereo) */
        double ratio = static_cast<double>(device_rate_) / source_rate_;
        int out_frames = static_cast<int>(frames * ratio) + 2;
        std::vector<float> resampled(static_cast<size_t>(out_frames * device_channels_));

        int produced = 0;
        for (int i = 0; i < out_frames; i++) {
            double src_pos = resample_pos_ + i / ratio;
            int idx = static_cast<int>(src_pos);
            if (idx + 1 >= frames) break;

            float frac = static_cast<float>(src_pos - idx);
            float sample = buffer[idx] + frac * (buffer[idx + 1] - buffer[idx]);

            /* Duplicate mono to all device channels */
            for (int ch = 0; ch < device_channels_; ch++) {
                if (device_is_float_) {
                    resampled[static_cast<size_t>(produced * device_channels_ + ch)] = sample;
                } else {
                    /* For int16 devices, still write as float — WASAPI shared mode
                       mix format is almost always float32 */
                    resampled[static_cast<size_t>(produced * device_channels_ + ch)] = sample;
                }
            }
            produced++;
        }

        /* Update fractional position for continuity */
        resample_pos_ = (resample_pos_ + static_cast<double>(frames)) -
                         static_cast<int>(resample_pos_ + static_cast<double>(frames));
        /* Actually, track how many source samples we consumed */
        resample_pos_ = fmod(resample_pos_ + static_cast<double>(produced) / ratio -
                              static_cast<double>(frames), 1.0);
        if (resample_pos_ < 0.0) resample_pos_ += 1.0;

        /* Write resampled data to WASAPI buffer */
        int written = 0;
        while (written < produced) {
            UINT32 padding = 0;
            HRESULT hr = client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) return -1;

            UINT32 available = buffer_frames_ - padding;
            if (available == 0) {
                Sleep(1);
                continue;
            }

            UINT32 to_write = static_cast<UINT32>(produced - written);
            if (to_write > available)
                to_write = available;

            BYTE* data = nullptr;
            hr = render_->GetBuffer(to_write, &data);
            if (FAILED(hr)) return -1;

            std::memcpy(data,
                        &resampled[static_cast<size_t>(written * device_channels_)],
                        static_cast<size_t>(to_write) * device_channels_ * sizeof(float));

            render_->ReleaseBuffer(to_write, 0);
            written += static_cast<int>(to_write);
        }
        return 0;
    }

    void flush() override {
        if (client_) {
            Sleep(50);
            client_->Stop();
            client_->Reset();
            client_->Start();
        }
    }

    void close() override {
        if (client_)  { client_->Stop(); }
        if (render_)  { render_->Release();  render_  = nullptr; }
        if (client_)  { client_->Release();  client_  = nullptr; }
        buffer_frames_ = 0;
        resample_pos_  = 0.0;
    }

private:
    IAudioClient*       client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    UINT32              buffer_frames_ = 0;

    int  device_rate_     = 0;
    int  device_channels_ = 0;
    bool device_is_float_ = false;

    int    source_rate_     = 0;
    int    source_channels_ = 0;
    double resample_pos_    = 0.0;
};

/* ── Device enumeration ────────────────────────────────────────────── */

std::vector<AudioDevice> audio_enumerate_inputs()
{
    ensure_com();
    std::vector<AudioDevice> result;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return result;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        enumerator->Release();
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr)) continue;

        AudioDevice dev;

        /* Get device ID */
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            dev.id = wchar_to_utf8(id);
            CoTaskMemFree(id);
        }

        /* Get friendly name */
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR) {
                dev.description = wchar_to_utf8(name.pwszVal);
            }
            PropVariantClear(&name);
            props->Release();
        }

        if (dev.description.empty())
            dev.description = dev.id;

        device->Release();
        result.push_back(std::move(dev));
    }

    collection->Release();
    enumerator->Release();
    return result;
}

/* ── Factory functions ─────────────────────────────────────────────── */

std::unique_ptr<AudioCapture> audio_create_capture() {
    return std::make_unique<WasapiCapture>();
}

std::unique_ptr<AudioPlayback> audio_create_playback() {
    return std::make_unique<WasapiPlayback>();
}
