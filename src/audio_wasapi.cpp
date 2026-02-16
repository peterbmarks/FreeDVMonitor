#include "audio_backend.h"

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstring>
#include <cmath>

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
            fprintf(stderr, "WASAPI: Failed to create device enumerator (0x%08lx)\n", hr);
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
            fprintf(stderr, "WASAPI: Failed to get capture device (0x%08lx)\n", hr);
            return false;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client_));
        device->Release();

        if (FAILED(hr) || !client_) {
            fprintf(stderr, "WASAPI: Failed to activate audio client (0x%08lx)\n", hr);
            return false;
        }

        /* Set up desired format: float32, mono/stereo, requested rate */
        WAVEFORMATEX fmt{};
        fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels       = static_cast<WORD>(channels);
        fmt.nSamplesPerSec  = static_cast<DWORD>(sample_rate);
        fmt.wBitsPerSample  = 32;
        fmt.nBlockAlign     = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize          = 0;

        /* 50ms buffer — enough for real-time processing */
        REFERENCE_TIME duration = 500000;  // 50ms in 100ns units

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 0, duration, 0, &fmt, nullptr);
        if (FAILED(hr)) {
            fprintf(stderr, "WASAPI: Failed to initialize capture client (0x%08lx)\n", hr);
            close();
            return false;
        }

        hr = client_->GetService(__uuidof(IAudioCaptureClient),
                                 reinterpret_cast<void**>(&capture_));
        if (FAILED(hr) || !capture_) {
            fprintf(stderr, "WASAPI: Failed to get capture service (0x%08lx)\n", hr);
            close();
            return false;
        }

        /* Create event for blocking reads */
        event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        hr = client_->SetEventHandle(event_);
        if (FAILED(hr)) {
            /* Fallback: polling mode if event-driven not supported */
            CloseHandle(event_);
            event_ = nullptr;
        }

        hr = client_->Start();
        if (FAILED(hr)) {
            fprintf(stderr, "WASAPI: Failed to start capture (0x%08lx)\n", hr);
            close();
            return false;
        }

        sample_rate_ = sample_rate;
        channels_    = channels;
        fprintf(stderr, "WASAPI capture: %s, %d Hz, float32\n",
                device_id.empty() ? "(default)" : device_id.c_str(), sample_rate);
        return true;
    }

    int read(float* buffer, int frames) override {
        if (!client_ || !capture_) return -1;

        int filled = 0;
        while (filled < frames) {
            UINT32 packet_len = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet_len);
            if (FAILED(hr)) return -1;

            if (packet_len == 0) {
                /* Wait for data */
                if (event_) {
                    WaitForSingleObject(event_, 100);
                } else {
                    Sleep(1);
                }
                continue;
            }

            BYTE* data = nullptr;
            UINT32 num_frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) return -1;

            int to_copy = static_cast<int>(num_frames);
            if (filled + to_copy > frames)
                to_copy = frames - filled;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::memset(buffer + filled, 0,
                            static_cast<size_t>(to_copy) * sizeof(float));
            } else {
                const float* src = reinterpret_cast<const float*>(data);
                if (channels_ == 1) {
                    std::memcpy(buffer + filled, src,
                                static_cast<size_t>(to_copy) * sizeof(float));
                } else {
                    /* Downmix to mono */
                    for (int i = 0; i < to_copy; i++) {
                        float sum = 0.0f;
                        for (int ch = 0; ch < channels_; ch++)
                            sum += src[i * channels_ + ch];
                        buffer[filled + i] = sum / channels_;
                    }
                }
            }

            capture_->ReleaseBuffer(num_frames);
            filled += to_copy;
        }
        return 0;
    }

    void close() override {
        if (client_) { client_->Stop(); }
        if (capture_) { capture_->Release(); capture_ = nullptr; }
        if (client_)  { client_->Release();  client_  = nullptr; }
        if (event_)   { CloseHandle(event_); event_   = nullptr; }
    }

private:
    IAudioClient*        client_  = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    HANDLE               event_   = nullptr;
    int sample_rate_ = 0;
    int channels_    = 0;
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
        if (FAILED(hr)) return false;

        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr) || !device) return false;

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client_));
        device->Release();
        if (FAILED(hr) || !client_) return false;

        WAVEFORMATEX fmt{};
        fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels       = static_cast<WORD>(channels);
        fmt.nSamplesPerSec  = static_cast<DWORD>(sample_rate);
        fmt.wBitsPerSample  = 32;
        fmt.nBlockAlign     = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize          = 0;

        /* 100ms buffer for smooth playback */
        REFERENCE_TIME duration = 1000000;  // 100ms in 100ns units

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 0, duration, 0, &fmt, nullptr);
        if (FAILED(hr)) {
            fprintf(stderr, "WASAPI: Failed to initialize playback client (0x%08lx)\n", hr);
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

        channels_ = channels;
        return true;
    }

    int write(const float* buffer, int frames) override {
        if (!client_ || !render_) return -1;

        int written = 0;
        while (written < frames) {
            UINT32 padding = 0;
            HRESULT hr = client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) return -1;

            UINT32 available = buffer_frames_ - padding;
            if (available == 0) {
                Sleep(1);
                continue;
            }

            UINT32 to_write = static_cast<UINT32>(frames - written);
            if (to_write > available)
                to_write = available;

            BYTE* data = nullptr;
            hr = render_->GetBuffer(to_write, &data);
            if (FAILED(hr)) return -1;

            std::memcpy(data, buffer + written * channels_,
                        static_cast<size_t>(to_write) * channels_ * sizeof(float));

            render_->ReleaseBuffer(to_write, 0);
            written += static_cast<int>(to_write);
        }
        return 0;
    }

    void flush() override {
        /* Let remaining buffer drain, then reset */
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
    }

private:
    IAudioClient*       client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    UINT32              buffer_frames_ = 0;
    int                 channels_ = 0;
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
