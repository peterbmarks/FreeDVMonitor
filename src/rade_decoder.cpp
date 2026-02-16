#include "rade_decoder.h"

#include <cmath>
#include <cstring>
#include <complex>
#include <vector>
#include <algorithm>
#include <mutex>

/* ── C headers from RADE / Opus (wrapped for C++ linkage) ────────────── */
extern "C" {
#include "rade_api.h"
#include "rade_dsp.h"
#include "fargan.h"
#include "lpcnet.h"
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── construction / destruction ──────────────────────────────────────── */

RadaeDecoder::RadaeDecoder()  = default;
RadaeDecoder::~RadaeDecoder() { stop(); close(); }

/* ── Hilbert coefficient initialisation (matches rade_demod.c) ───────── */

static void init_hilbert_coeffs(float coeffs[], int ntaps) {
    int center = (ntaps - 1) / 2;
    for (int i = 0; i < ntaps; i++) {
        int n = i - center;
        if (n == 0 || (n & 1) == 0) {
            coeffs[i] = 0.0f;
        } else {
            float h = 2.0f / (static_cast<float>(M_PI) * n);
            float w = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) * i / (ntaps - 1));
            coeffs[i] = h * w;
        }
    }
}

/* ── WAV file I/O (adapted from rade_demod.c) ────────────────────────── */

#define WAV_FMT_PCM   1
#define WAV_FMT_FLOAT 3

struct wav_info {
    int      sample_rate;
    int      num_channels;
    int      bits_per_sample;
    bool     is_float;
    long     data_offset;
    uint32_t data_size;
};

static bool wav_read_header(FILE* f, wav_info& info)
{
    char     tag[4];
    uint32_t riff_size;

    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "RIFF", 4)) return false;
    if (std::fread(&riff_size, 4, 1, f) != 1) return false;
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "WAVE", 4)) return false;

    info.data_offset = -1;

    while (true) {
        char     chunk_id[4];
        uint32_t chunk_size;
        if (std::fread(chunk_id, 1, 4, f) != 4) break;
        if (std::fread(&chunk_size, 4, 1, f) != 1) break;

        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16) return false;
            uint8_t buf[16];
            if (std::fread(buf, 1, 16, f) != 16) return false;

            uint16_t audio_fmt, nch, bps;
            uint32_t sr;
            std::memcpy(&audio_fmt, buf + 0,  2);
            std::memcpy(&nch,       buf + 2,  2);
            std::memcpy(&sr,        buf + 4,  4);
            std::memcpy(&bps,       buf + 14, 2);

            info.sample_rate     = static_cast<int>(sr);
            info.num_channels    = static_cast<int>(nch);
            info.bits_per_sample = static_cast<int>(bps);
            info.is_float        = (audio_fmt == WAV_FMT_FLOAT);

            if (chunk_size > 16)
                std::fseek(f, static_cast<long>(chunk_size - 16), SEEK_CUR);

        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            info.data_offset = std::ftell(f);
            info.data_size   = chunk_size;
            break;
        } else {
            std::fseek(f, static_cast<long>((chunk_size + 1) & ~1u), SEEK_CUR);
        }
    }
    return (info.data_offset >= 0);
}

static std::vector<float> wav_read_mono_float(FILE* f, const wav_info& info)
{
    int  bps  = info.bits_per_sample;
    int  nch  = info.num_channels;
    long total = static_cast<long>(info.data_size) / (bps / 8);
    long mono  = total / nch;

    std::vector<float> buf(static_cast<size_t>(mono));

    for (long i = 0; i < mono; i++) {
        float sum = 0.0f;
        for (int ch = 0; ch < nch; ch++) {
            float v = 0.0f;
            if (info.is_float && bps == 32) {
                float tmp; if (std::fread(&tmp, 4, 1, f) == 1) v = tmp;
            } else if (info.is_float && bps == 64) {
                double tmp; if (std::fread(&tmp, 8, 1, f) == 1) v = static_cast<float>(tmp);
            } else if (bps == 16) {
                int16_t tmp; if (std::fread(&tmp, 2, 1, f) == 1) v = tmp / 32768.0f;
            } else if (bps == 24) {
                uint8_t b[3]; if (std::fread(b, 1, 3, f) == 3) {
                    int32_t raw = (static_cast<int32_t>(b[2]) << 16) | (b[1] << 8) | b[0];
                    if (raw & 0x800000) raw |= static_cast<int32_t>(0xFF000000);
                    v = raw / 8388608.0f;
                }
            } else if (bps == 32) {
                int32_t tmp; if (std::fread(&tmp, 4, 1, f) == 1) v = tmp / 2147483648.0f;
            } else {
                return {};
            }
            sum += v;
        }
        buf[static_cast<size_t>(i)] = sum / nch;
    }
    return buf;
}

static std::vector<float> resample_batch(const std::vector<float>& in,
                                         int in_rate, int out_rate)
{
    if (in_rate == out_rate) return in;

    auto n_in = static_cast<long>(in.size());
    if (n_in < 2) return {};

    long n_out = static_cast<long>(static_cast<double>(n_in) * out_rate / in_rate);
    std::vector<float> out(static_cast<size_t>(n_out));

    double step = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    for (long i = 0; i < n_out; i++) {
        double pos  = i * step;
        long   idx  = static_cast<long>(pos);
        float  frac = static_cast<float>(pos - idx);
        if (idx + 1 >= n_in) { idx = n_in - 2; frac = 1.0f; }
        out[static_cast<size_t>(i)] = in[static_cast<size_t>(idx)]
            + frac * (in[static_cast<size_t>(idx + 1)] - in[static_cast<size_t>(idx)]);
    }
    return out;
}

/* ── radix-2 Cooley-Tukey FFT (in-place, N must be power of 2) ────────── */

static void fft_radix2(std::complex<float>* x, int N)
{
    /* bit-reversal permutation */
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    /* butterfly passes */
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2.0f * static_cast<float>(M_PI) / len;
        std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < N; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; j++) {
                auto u = x[i + j];
                auto v = x[i + j + len / 2] * w;
                x[i + j]             = u + v;
                x[i + j + len / 2]   = u - v;
                w *= wlen;
            }
        }
    }
}

/* ── get_spectrum (thread-safe read) ─────────────────────────────────── */

void RadaeDecoder::get_spectrum(float* out, int n) const
{
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    int count = std::min(n, SPECTRUM_BINS);
    std::memcpy(out, spectrum_mag_, static_cast<size_t>(count) * sizeof(float));
}

/* ── raw recording ───────────────────────────────────────────────────── */

void RadaeDecoder::start_recording(const std::string& path)
{
    std::lock_guard<std::mutex> lock(rec_mutex_);
    if (rec_file_) return;
    rec_file_ = std::fopen(path.c_str(), "wb");
    if (rec_file_) {
        recording_.store(true, std::memory_order_relaxed);
        fprintf(stderr, "Recording: %s, signed 16-bit PCM, 8000 Hz, mono\n",
                path.c_str());
    }
}

void RadaeDecoder::stop_recording()
{
    recording_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(rec_mutex_);
    if (rec_file_) { std::fclose(rec_file_); rec_file_ = nullptr; }
}

/* ── open / close ────────────────────────────────────────────────────── */

bool RadaeDecoder::open(const std::string& device_name)
{
    close();

    /* ── Open PulseAudio capture at 8 kHz mono float32 ────────────── */
    pa_sample_spec cap_spec{};
    cap_spec.format   = PA_SAMPLE_FLOAT32LE;
    cap_spec.rate     = RADE_FS;        /* 8000 Hz — PulseAudio resamples from hardware */
    cap_spec.channels = 1;

    int pa_err = 0;
    const char* dev = device_name.empty() ? nullptr : device_name.c_str();
    pa_in_ = pa_simple_new(nullptr, "FreeDV Monitor", PA_STREAM_RECORD,
                           dev, "Capture", &cap_spec, nullptr, nullptr, &pa_err);
    if (!pa_in_) {
        fprintf(stderr, "PulseAudio capture open failed: %s\n", pa_strerror(pa_err));
        return false;
    }

    fprintf(stderr, "PulseAudio capture: %s, %u Hz, float32\n",
            device_name.empty() ? "(default)" : device_name.c_str(),
            cap_spec.rate);

    /* ── Open PulseAudio playback at 16 kHz mono float32 ─────────── */
    pa_sample_spec play_spec{};
    play_spec.format   = PA_SAMPLE_FLOAT32LE;
    play_spec.rate     = RADE_FS_SPEECH;  /* 16000 Hz — PulseAudio resamples to hardware */
    play_spec.channels = 1;

    pa_out_ = pa_simple_new(nullptr, "FreeDV Monitor", PA_STREAM_PLAYBACK,
                            nullptr, "Playback", &play_spec, nullptr, nullptr, &pa_err);
    if (!pa_out_) {
        fprintf(stderr, "PulseAudio playback open failed: %s\n", pa_strerror(pa_err));
        pa_simple_free(pa_in_); pa_in_ = nullptr;
        return false;
    }

    /* ── RADE receiver ──────────────────────────────────────────────── */
    rade_initialize();
    rade_ = rade_open(nullptr, 0);
    if (!rade_) {
        pa_simple_free(pa_in_);  pa_in_  = nullptr;
        pa_simple_free(pa_out_); pa_out_ = nullptr;
        return false;
    }

    /* ── FARGAN vocoder ─────────────────────────────────────────────── */
    fargan_ = new FARGANState;
    fargan_init(static_cast<FARGANState*>(fargan_));
    fargan_ready_ = false;
    warmup_count_ = 0;

    /* ── Hilbert coefficients ───────────────────────────────────────── */
    init_hilbert_coeffs(hilbert_coeffs_, HILBERT_NTAPS);
    std::memset(hilbert_hist_, 0, sizeof(hilbert_hist_));
    hilbert_pos_ = 0;
    std::memset(delay_buf_, 0, sizeof(delay_buf_));
    delay_pos_ = 0;

    /* ── Hanning window for FFT ─────────────────────────────────────── */
    for (int i = 0; i < FFT_SIZE; i++)
        fft_window_[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (FFT_SIZE - 1)));
    std::memset(spectrum_mag_, 0, sizeof(spectrum_mag_));

    return true;
}

bool RadaeDecoder::open_file(const std::string& wav_path)
{
    close();

    /* ── Read and parse WAV file ────────────────────────────────── */
    FILE* f = std::fopen(wav_path.c_str(), "rb");
    if (!f) return false;

    wav_info wav{};
    if (!wav_read_header(f, wav)) {
        std::fclose(f);
        return false;
    }

    auto mono = wav_read_mono_float(f, wav);
    std::fclose(f);
    if (mono.empty()) return false;

    /* ── Resample to 8 kHz ──────────────────────────────────────── */
    if (wav.sample_rate != RADE_FS) {
        file_audio_8k_ = resample_batch(mono, wav.sample_rate, RADE_FS);
    } else {
        file_audio_8k_ = std::move(mono);
    }
    if (file_audio_8k_.empty()) return false;
    file_pos_ = 0;

    /* ── Open PulseAudio playback at 16 kHz mono float32 ─────────── */
    pa_sample_spec play_spec{};
    play_spec.format   = PA_SAMPLE_FLOAT32LE;
    play_spec.rate     = RADE_FS_SPEECH;
    play_spec.channels = 1;

    int pa_err = 0;
    pa_out_ = pa_simple_new(nullptr, "FreeDV Monitor", PA_STREAM_PLAYBACK,
                            nullptr, "Playback", &play_spec, nullptr, nullptr, &pa_err);
    if (!pa_out_) {
        fprintf(stderr, "PulseAudio playback open failed: %s\n", pa_strerror(pa_err));
        return false;
    }

    /* ── RADE receiver ──────────────────────────────────────────── */
    rade_initialize();
    rade_ = rade_open(nullptr, 0);
    if (!rade_) {
        pa_simple_free(pa_out_); pa_out_ = nullptr;
        return false;
    }

    /* ── FARGAN vocoder ─────────────────────────────────────────── */
    fargan_ = new FARGANState;
    fargan_init(static_cast<FARGANState*>(fargan_));
    fargan_ready_ = false;
    warmup_count_ = 0;

    /* ── Hilbert coefficients ───────────────────────────────────── */
    init_hilbert_coeffs(hilbert_coeffs_, HILBERT_NTAPS);
    std::memset(hilbert_hist_, 0, sizeof(hilbert_hist_));
    hilbert_pos_ = 0;
    std::memset(delay_buf_, 0, sizeof(delay_buf_));
    delay_pos_ = 0;

    /* ── Hanning window for FFT ─────────────────────────────────── */
    for (int i = 0; i < FFT_SIZE; i++)
        fft_window_[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (FFT_SIZE - 1)));
    std::memset(spectrum_mag_, 0, sizeof(spectrum_mag_));

    file_mode_ = true;

    return true;
}

void RadaeDecoder::close()
{
    stop();
    stop_recording();

    if (rade_) { rade_close(rade_); rade_ = nullptr; }
    if (fargan_) { delete static_cast<FARGANState*>(fargan_); fargan_ = nullptr; }

    if (pa_in_)  { pa_simple_free(pa_in_);  pa_in_  = nullptr; }
    if (pa_out_) { pa_simple_free(pa_out_); pa_out_ = nullptr; }

    file_audio_8k_.clear();
    file_audio_8k_.shrink_to_fit();
    file_pos_  = 0;
    file_mode_ = false;

    synced_       = false;
    snr_dB_       = 0.0f;
    freq_offset_  = 0.0f;
    input_level_  = 0.0f;
    output_level_ = 0.0f;
}

/* ── start / stop ────────────────────────────────────────────────────── */

void RadaeDecoder::start()
{
    if ((!pa_in_ && !file_mode_) || !pa_out_ || !rade_ || running_) return;

    running_ = true;
    thread_  = std::thread(&RadaeDecoder::processing_loop, this);
}

void RadaeDecoder::stop()
{
    if (!running_) return;
    running_ = false;

    if (thread_.joinable()) thread_.join();

    /* Flush any remaining playback data */
    if (pa_out_) pa_simple_flush(pa_out_, nullptr);

    input_level_  = 0.0f;
    output_level_ = 0.0f;
    synced_       = false;
}

/* ── streaming Hilbert transform ─────────────────────────────────────
 *
 *  For each input sample at 8 kHz, produce one RADE_COMP:
 *    .real = sample delayed by HILBERT_DELAY (63 samples)
 *    .imag = Hilbert-filtered sample
 * ──────────────────────────────────────────────────────────────────── */

static void hilbert_process(const float* in, RADE_COMP* out, int n,
                            const float coeffs[], float hist[], int& pos,
                            float delay[], int& dpos, int ntaps, int delay_n)
{
    for (int i = 0; i < n; i++) {
        float sample = in[i];

        /* store in history ring buffer */
        hist[pos] = sample;

        /* FIR convolution for imaginary part */
        float imag = 0.0f;
        for (int k = 0; k < ntaps; k++) {
            int idx = pos - k;
            if (idx < 0) idx += ntaps;
            imag += coeffs[k] * hist[idx];
        }

        /* delayed real part */
        delay[dpos] = sample;
        int read_pos = dpos - delay_n;
        if (read_pos < 0) read_pos += ntaps;
        out[i].real = delay[read_pos];
        out[i].imag = imag;

        pos = (pos + 1) % ntaps;
        dpos = (dpos + 1) % ntaps;
    }
}

/* ── processing loop (dedicated thread) ──────────────────────────────── */

void RadaeDecoder::processing_loop()
{
    int nin_max        = rade_nin_max(rade_);
    int n_features_out = rade_n_features_in_out(rade_);
    int n_eoo_bits     = rade_n_eoo_bits(rade_);

    /* allocate working buffers */
    std::vector<RADE_COMP> rx_buf(static_cast<size_t>(nin_max));
    std::vector<float>     feat_buf(static_cast<size_t>(n_features_out));
    std::vector<float>     eoo_buf(static_cast<size_t>(n_eoo_bits));

    /* accumulation buffer for 8 kHz mono float samples */
    std::vector<float> acc_8k;
    acc_8k.reserve(static_cast<size_t>(nin_max * 2));

    /* capture read buffer (already at 8 kHz from PulseAudio) */
    constexpr int READ_FRAMES = 512;
    std::vector<float> capture_buf(READ_FRAMES);

    bool was_synced = false;
    bool output_primed = false;

    while (running_.load(std::memory_order_relaxed)) {

        int nin = rade_nin(rade_);

        /* ── accumulate enough 8 kHz samples ─────────────────────────── */
        while (static_cast<int>(acc_8k.size()) < nin &&
               running_.load(std::memory_order_relaxed))
        {
            if (file_mode_) {
                /* ── file mode: copy from pre-loaded buffer ───────── */
                size_t remaining = file_audio_8k_.size() - file_pos_;
                if (remaining == 0) {
                    running_ = false;
                    break;
                }
                size_t need  = static_cast<size_t>(nin) - acc_8k.size();
                size_t chunk = std::min(need, remaining);
                acc_8k.insert(acc_8k.end(),
                              file_audio_8k_.begin() + static_cast<ptrdiff_t>(file_pos_),
                              file_audio_8k_.begin() + static_cast<ptrdiff_t>(file_pos_ + chunk));
                file_pos_ += chunk;
            } else {
                /* ── live mode: read from PulseAudio capture ────────── */
                int pa_err = 0;
                int ret = pa_simple_read(pa_in_, capture_buf.data(),
                                         READ_FRAMES * sizeof(float), &pa_err);
                if (ret < 0) {
                    if (!running_.load(std::memory_order_relaxed)) break;
                    fprintf(stderr, "PulseAudio read error: %s\n", pa_strerror(pa_err));
                    running_ = false;
                    break;
                }

                /* Already at 8 kHz float32 — append directly */
                acc_8k.insert(acc_8k.end(), capture_buf.begin(),
                              capture_buf.begin() + READ_FRAMES);
            }
        }

        if (!running_.load(std::memory_order_relaxed)) break;

        /* ── record 8 kHz samples before gain ─────────────────────────── */
        if (recording_.load(std::memory_order_relaxed)) {
            std::vector<int16_t> rec_i16(static_cast<size_t>(nin));
            for (int i = 0; i < nin; i++) {
                float s = acc_8k[static_cast<size_t>(i)] * 32768.0f;
                if (s > 32767.0f) s = 32767.0f;
                if (s < -32768.0f) s = -32768.0f;
                rec_i16[static_cast<size_t>(i)] = static_cast<int16_t>(s);
            }
            std::lock_guard<std::mutex> lock(rec_mutex_);
            if (rec_file_)
                std::fwrite(rec_i16.data(), sizeof(int16_t),
                            static_cast<size_t>(nin), rec_file_);
        }

        /* ── apply input gain ─────────────────────────────────────────── */
        {
            float gain = input_gain_.load(std::memory_order_relaxed);
            if (gain != 1.0f) {
                for (int i = 0; i < nin; i++)
                    acc_8k[static_cast<size_t>(i)] *= gain;
            }
        }

        /* ── FFT spectrum of input 8 kHz audio ───────────────────────── */
        if (static_cast<int>(acc_8k.size()) >= FFT_SIZE) {
            std::complex<float> fft_buf[FFT_SIZE];
            int offset = static_cast<int>(acc_8k.size()) - FFT_SIZE;
            for (int i = 0; i < FFT_SIZE; i++)
                fft_buf[i] = acc_8k[static_cast<size_t>(offset + i)] * fft_window_[i];

            fft_radix2(fft_buf, FFT_SIZE);

            float tmp[SPECTRUM_BINS];
            for (int i = 0; i < SPECTRUM_BINS; i++) {
                float mag = std::abs(fft_buf[i]) / (FFT_SIZE * 0.5f);
                tmp[i] = (mag > 1e-10f)
                       ? 20.0f * std::log10(mag)
                       : -200.0f;
            }
            {
                std::lock_guard<std::mutex> lock(spectrum_mutex_);
                std::memcpy(spectrum_mag_, tmp, sizeof(spectrum_mag_));
            }
        }

        /* ── input RMS level ──────────────────────────────────────────── */
        {
            double sum2 = 0.0;
            for (int i = 0; i < nin; i++)
                sum2 += static_cast<double>(acc_8k[static_cast<size_t>(i)])
                      * static_cast<double>(acc_8k[static_cast<size_t>(i)]);
            input_level_.store(std::sqrt(static_cast<float>(sum2 / nin)),
                               std::memory_order_relaxed);
        }

        /* ── Hilbert transform: real 8 kHz → complex IQ ──────────────── */
        hilbert_process(acc_8k.data(), rx_buf.data(), nin,
                        hilbert_coeffs_, hilbert_hist_, hilbert_pos_,
                        delay_buf_, delay_pos_, HILBERT_NTAPS, HILBERT_DELAY);

        /* consume nin samples from accumulator */
        acc_8k.erase(acc_8k.begin(), acc_8k.begin() + nin);

        /* ── RADE Rx ─────────────────────────────────────────────────── */
        int has_eoo = 0;
        int n_out = rade_rx(rade_, feat_buf.data(), &has_eoo,
                            eoo_buf.data(), rx_buf.data());

        /* update sync status */
        bool now_synced = (rade_sync(rade_) != 0);
        synced_.store(now_synced, std::memory_order_relaxed);

        if (now_synced) {
            snr_dB_.store(static_cast<float>(rade_snrdB_3k_est(rade_)),
                          std::memory_order_relaxed);
            freq_offset_.store(rade_freq_offset(rade_),
                               std::memory_order_relaxed);
        }

        /* handle sync transitions */
        if (was_synced && !now_synced) {
            /* lost sync — reset FARGAN for next sync */
            fargan_init(static_cast<FARGANState*>(fargan_));
            fargan_ready_ = false;
            warmup_count_ = 0;
            output_primed = false;
        }
        was_synced = now_synced;

        /* ── synthesise decoded speech ───────────────────────────────── */
        if (n_out > 0) {
            int n_frames = n_out / RADE_NB_TOTAL_FEATURES;
            double rms_sum = 0.0;
            int    rms_n   = 0;

            for (int fi = 0; fi < n_frames; fi++) {
                float* feat = &feat_buf[static_cast<size_t>(fi * RADE_NB_TOTAL_FEATURES)];

                /* ── FARGAN warmup: buffer first 5 frames ─────────────── */
                if (!fargan_ready_) {
                    std::memcpy(&warmup_buf_[warmup_count_ * NB_TOTAL_FEAT],
                                feat,
                                static_cast<size_t>(NB_TOTAL_FEAT) * sizeof(float));

                    if (++warmup_count_ >= 5) {
                        /* pack to NB_FEATURES stride for fargan_cont */
                        float packed[5 * NB_FEATURES];
                        for (int i = 0; i < 5; i++)
                            std::memcpy(&packed[i * NB_FEATURES],
                                        &warmup_buf_[i * NB_TOTAL_FEAT],
                                        static_cast<size_t>(NB_FEATURES) * sizeof(float));

                        float zeros[FARGAN_CONT_SAMPLES] = {};
                        fargan_cont(static_cast<FARGANState*>(fargan_),
                                    zeros, packed);
                        fargan_ready_ = true;

                        /* pre-fill output buffer with silence so it has
                           enough headroom for the bursty write pattern */
                        if (!output_primed) {
                            int prefill = 2 * 12 * LPCNET_FRAME_SIZE;
                            std::vector<float> silence(static_cast<size_t>(prefill), 0.0f);
                            pa_simple_write(pa_out_, silence.data(),
                                            static_cast<size_t>(prefill) * sizeof(float),
                                            nullptr);
                            output_primed = true;
                        }
                    }
                    continue;   /* warmup frames not synthesised */
                }

                /* ── synthesise one 10-ms speech frame ────────────────── */
                float fpcm[LPCNET_FRAME_SIZE];
                fargan_synthesize(static_cast<FARGANState*>(fargan_),
                                  fpcm, feat);

                /* accumulate RMS of output */
                for (int s = 0; s < LPCNET_FRAME_SIZE; s++)
                    rms_sum += static_cast<double>(fpcm[s]) * fpcm[s];
                rms_n += LPCNET_FRAME_SIZE;

                /* ── write 16 kHz speech to PulseAudio output ──────────── */
                if (running_.load(std::memory_order_relaxed)) {
                    pa_simple_write(pa_out_, fpcm,
                                    LPCNET_FRAME_SIZE * sizeof(float),
                                    nullptr);
                }
            }

            /* update output level */
            if (rms_n > 0)
                output_level_.store(
                    static_cast<float>(std::sqrt(rms_sum / rms_n)),
                    std::memory_order_relaxed);
        } else {
            /* no decoded output this frame — decay level toward zero */
            float lvl = output_level_.load(std::memory_order_relaxed);
            output_level_.store(lvl * 0.9f, std::memory_order_relaxed);
        }
    }
}
