/*---------------------------------------------------------------------------*\
  test_loopback.c

  Loopback test: generate OFDM frames -> take real part -> Hilbert -> rade_rx
  This verifies the C DSP stack can achieve sync on a known signal.
\*---------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rade_api.h"
#include "rade_dsp.h"
#include "rade_ofdm.h"
#include "rade_rx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Hilbert transform FIR (same as rade_decoder.cpp) */
#define HILBERT_NTAPS 127
#define HILBERT_DELAY ((HILBERT_NTAPS - 1) / 2)  /* 63 */

static float hilbert_coeffs[HILBERT_NTAPS];
static float hilbert_hist[HILBERT_NTAPS];
static int   hilbert_pos = 0;
static float delay_buf[HILBERT_NTAPS];
static int   delay_pos = 0;

static void init_hilbert(void) {
    int center = (HILBERT_NTAPS - 1) / 2;
    for (int i = 0; i < HILBERT_NTAPS; i++) {
        int n = i - center;
        if (n == 0 || (n & 1) == 0) {
            hilbert_coeffs[i] = 0.0f;
        } else {
            float h = 2.0f / (M_PI * n);
            float w = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (HILBERT_NTAPS - 1));
            hilbert_coeffs[i] = h * w;
        }
    }
    memset(hilbert_hist, 0, sizeof(hilbert_hist));
    memset(delay_buf, 0, sizeof(delay_buf));
    hilbert_pos = 0;
    delay_pos = 0;
}

static void hilbert_process(const float *in, RADE_COMP *out, int n) {
    for (int i = 0; i < n; i++) {
        float sample = in[i];

        hilbert_hist[hilbert_pos] = sample;

        float imag = 0.0f;
        for (int k = 0; k < HILBERT_NTAPS; k++) {
            int idx = hilbert_pos - k;
            if (idx < 0) idx += HILBERT_NTAPS;
            imag += hilbert_coeffs[k] * hilbert_hist[idx];
        }

        delay_buf[delay_pos] = sample;
        int read_pos = delay_pos - HILBERT_DELAY;
        if (read_pos < 0) read_pos += HILBERT_NTAPS;
        out[i].real = delay_buf[read_pos];
        out[i].imag = imag;

        hilbert_pos = (hilbert_pos + 1) % HILBERT_NTAPS;
        delay_pos = (delay_pos + 1) % HILBERT_NTAPS;
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    fprintf(stderr, "=== RADE Loopback Test ===\n\n");

    /* ── Test 1: Feed complex OFDM directly (no Hilbert) ─────────────── */
    fprintf(stderr, "--- Test 1: Direct complex loopback (bypass Hilbert) ---\n");
    {
        struct rade *r = rade_open(NULL, 0);  /* verbose enabled */
        if (!r) { fprintf(stderr, "FAIL: rade_open\n"); return 1; }

        int nin_max = rade_nin_max(r);
        int n_feat = rade_n_features_in_out(r);
        int n_eoo = rade_n_eoo_bits(r);

        float *features = (float *)calloc(n_feat, sizeof(float));
        float *eoo = (float *)calloc(n_eoo, sizeof(float));
        RADE_COMP *rx_buf = (RADE_COMP *)calloc(nin_max, sizeof(RADE_COMP));

        /* Generate 20 modem frames of OFDM signal */
        int n_frames = 20;
        int Nmf = RADE_NMF;
        RADE_COMP *tx_signal = (RADE_COMP *)calloc(n_frames * Nmf, sizeof(RADE_COMP));

        /* Init OFDM for modulation */
        rade_ofdm ofdm;
        rade_ofdm_init(&ofdm, 3);

        /* Generate random latent data and modulate */
        float z[RADE_NZMF * RADE_LATENT_DIM];
        for (int f = 0; f < n_frames; f++) {
            /* Fill with small random values */
            for (int i = 0; i < RADE_NZMF * RADE_LATENT_DIM; i++) {
                z[i] = 0.1f * ((float)rand() / RAND_MAX - 0.5f);
            }
            rade_ofdm_mod_frame(&ofdm, &tx_signal[f * Nmf], z);
        }

        fprintf(stderr, "Generated %d modem frames (%d samples)\n", n_frames, n_frames * Nmf);

        /* Feed to receiver nin samples at a time */
        int tx_pos = 0;
        int synced = 0;

        for (int iter = 0; iter < n_frames * 2 && tx_pos < n_frames * Nmf; iter++) {
            int nin = rade_nin(r);
            int avail = n_frames * Nmf - tx_pos;
            if (avail < nin) break;

            memcpy(rx_buf, &tx_signal[tx_pos], nin * sizeof(RADE_COMP));
            tx_pos += nin;

            int has_eoo = 0;
            int n_out = rade_rx(r, features, &has_eoo, eoo, rx_buf);

            if (rade_sync(r)) {
                if (!synced) {
                    fprintf(stderr, ">>> SYNC achieved at frame %d (tx_pos=%d)!\n", iter, tx_pos);
                    synced = 1;
                }
            }
        }

        if (!synced) {
            fprintf(stderr, ">>> FAIL: Never achieved sync with direct complex signal\n");
        }

        free(features); free(eoo); free(rx_buf); free(tx_signal);
        rade_close(r);
    }

    fprintf(stderr, "\n");

    /* ── Test 2: Real part -> Hilbert -> rade_rx ──────────────────────── */
    fprintf(stderr, "--- Test 2: Real part -> Hilbert -> rade_rx ---\n");
    {
        struct rade *r = rade_open(NULL, 0);  /* verbose enabled */
        if (!r) { fprintf(stderr, "FAIL: rade_open\n"); return 1; }

        int nin_max = rade_nin_max(r);
        int n_feat = rade_n_features_in_out(r);
        int n_eoo = rade_n_eoo_bits(r);

        float *features = (float *)calloc(n_feat, sizeof(float));
        float *eoo = (float *)calloc(n_eoo, sizeof(float));
        RADE_COMP *rx_buf = (RADE_COMP *)calloc(nin_max, sizeof(RADE_COMP));

        /* Generate 20 modem frames */
        int n_frames = 20;
        int Nmf = RADE_NMF;
        RADE_COMP *tx_signal = (RADE_COMP *)calloc(n_frames * Nmf, sizeof(RADE_COMP));

        rade_ofdm ofdm;
        rade_ofdm_init(&ofdm, 3);

        float z[RADE_NZMF * RADE_LATENT_DIM];
        for (int f = 0; f < n_frames; f++) {
            for (int i = 0; i < RADE_NZMF * RADE_LATENT_DIM; i++) {
                z[i] = 0.1f * ((float)rand() / RAND_MAX - 0.5f);
            }
            rade_ofdm_mod_frame(&ofdm, &tx_signal[f * Nmf], z);
        }

        /* Extract real part */
        int total_samples = n_frames * Nmf;
        float *real_signal = (float *)calloc(total_samples, sizeof(float));
        for (int i = 0; i < total_samples; i++) {
            real_signal[i] = tx_signal[i].real;
        }

        fprintf(stderr, "Generated %d samples of real signal\n", total_samples);

        /* Apply Hilbert transform and feed to receiver */
        init_hilbert();

        int tx_pos = 0;
        int synced = 0;

        for (int iter = 0; iter < n_frames * 2 && tx_pos < total_samples; iter++) {
            int nin = rade_nin(r);
            int avail = total_samples - tx_pos;
            if (avail < nin) break;

            /* Hilbert transform: real -> complex IQ */
            hilbert_process(&real_signal[tx_pos], rx_buf, nin);
            tx_pos += nin;

            int has_eoo = 0;
            int n_out = rade_rx(r, features, &has_eoo, eoo, rx_buf);

            if (rade_sync(r)) {
                if (!synced) {
                    fprintf(stderr, ">>> SYNC achieved at frame %d (tx_pos=%d)!\n", iter, tx_pos);
                    synced = 1;
                }
            }
        }

        if (!synced) {
            fprintf(stderr, ">>> FAIL: Never achieved sync with Hilbert signal\n");
        }

        free(features); free(eoo); free(rx_buf); free(tx_signal); free(real_signal);
        rade_close(r);
    }

    /* ── Generate test WAV file for use with the app ────────────────────── */
    fprintf(stderr, "--- Generating test_rade.wav (8kHz mono, 10s) ---\n");
    {
        rade_ofdm ofdm;
        rade_ofdm_init(&ofdm, 3);

        int Nmf = RADE_NMF;
        int n_frames = 10 * RADE_FS / Nmf;  /* ~10 seconds */
        int total_samples = n_frames * Nmf;

        RADE_COMP *tx_signal = (RADE_COMP *)calloc(total_samples, sizeof(RADE_COMP));

        float z[RADE_NZMF * RADE_LATENT_DIM];
        for (int f = 0; f < n_frames; f++) {
            for (int i = 0; i < RADE_NZMF * RADE_LATENT_DIM; i++) {
                z[i] = 0.1f * ((float)rand() / RAND_MAX - 0.5f);
            }
            rade_ofdm_mod_frame(&ofdm, &tx_signal[f * Nmf], z);
        }

        /* Extract real part as 16-bit PCM */
        int16_t *pcm = (int16_t *)calloc(total_samples, sizeof(int16_t));
        for (int i = 0; i < total_samples; i++) {
            float s = tx_signal[i].real * 16384.0f;  /* ~-6dBFS */
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            pcm[i] = (int16_t)s;
        }

        /* Write WAV file */
        FILE *wf = fopen("test_rade.wav", "wb");
        if (wf) {
            uint32_t data_size = total_samples * 2;
            uint32_t file_size = 36 + data_size;
            uint16_t audio_fmt = 1;  /* PCM */
            uint16_t nch = 1;
            uint32_t sr = RADE_FS;
            uint32_t byte_rate = sr * 2;
            uint16_t block_align = 2;
            uint16_t bps = 16;

            fwrite("RIFF", 1, 4, wf);
            fwrite(&file_size, 4, 1, wf);
            fwrite("WAVE", 1, 4, wf);
            fwrite("fmt ", 1, 4, wf);
            uint32_t fmt_size = 16;
            fwrite(&fmt_size, 4, 1, wf);
            fwrite(&audio_fmt, 2, 1, wf);
            fwrite(&nch, 2, 1, wf);
            fwrite(&sr, 4, 1, wf);
            fwrite(&byte_rate, 4, 1, wf);
            fwrite(&block_align, 2, 1, wf);
            fwrite(&bps, 2, 1, wf);
            fwrite("data", 1, 4, wf);
            fwrite(&data_size, 4, 1, wf);
            fwrite(pcm, 2, total_samples, wf);
            fclose(wf);
            fprintf(stderr, ">>> Written test_rade.wav (%d samples, %.1f seconds)\n",
                    total_samples, (float)total_samples / RADE_FS);
            fprintf(stderr, "    Open this file in the app to test sync.\n");
        } else {
            fprintf(stderr, ">>> Failed to write test_rade.wav\n");
        }

        free(pcm);
        free(tx_signal);
    }

    fprintf(stderr, "\n=== Tests complete ===\n");
    return 0;
}
