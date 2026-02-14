/*---------------------------------------------------------------------------*\

  rade_tx.c

  RADAE transmitter implementation.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  - Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  - Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "rade_tx.h"
#include <string.h>
#include <assert.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rade_constants.h"

/*---------------------------------------------------------------------------*\
                        INITIALIZATION
\*---------------------------------------------------------------------------*/

int rade_tx_init(rade_tx_state *tx, const RADEEnc *enc_model, int bottleneck, int auxdata, int bpf_en) {
    assert(tx != NULL);

    memset(tx, 0, sizeof(rade_tx_state));

    tx->bottleneck = bottleneck;
    tx->auxdata = auxdata;
    tx->bpf_en = bpf_en;

    /* Number of input features per encoder call */
    tx->num_features = RADE_NB_TOTAL_FEATURES;

    /* Initialize OFDM */
    rade_ofdm_init(&tx->ofdm, bottleneck);

    /* Initialize BPF if enabled */
    if (bpf_en) {
        rade_bpf_init(&tx->bpf, RADE_BPF_NTAP, (float)RADE_FS, 1200.0f, 1500.0f, RADE_NMF);
    }

    /* Initialize encoder */
    if (enc_model != NULL) {
        memcpy(&tx->enc_model, enc_model, sizeof(RADEEnc));
    } else {
        /* Use built-in weights */
        if (init_radeenc(&tx->enc_model, radeenc_arrays, RADE_NB_TOTAL_FEATURES) != 0) {
            return -1;
        }
    }
    rade_init_encoder(&tx->enc_state);

    /* Initialize EOO bits to +1 */
    for (int i = 0; i < RADE_TX_N_EOO_BITS; i++) {
        tx->eoo_bits[i] = 1.0f;
    }

    return 0;
}

/*---------------------------------------------------------------------------*\
                         GETTERS
\*---------------------------------------------------------------------------*/

int rade_tx_n_features_in(const rade_tx_state *tx) {
    assert(tx != NULL);
    return RADE_FRAMES_PER_STEP * tx->num_features;
}

int rade_tx_n_samples_out(const rade_tx_state *tx) {
    assert(tx != NULL);
    return RADE_NMF;
}

int rade_tx_n_eoo_out(const rade_tx_state *tx) {
    assert(tx != NULL);
    return RADE_NEOO;
}

int rade_tx_n_eoo_bits(const rade_tx_state *tx) {
    assert(tx != NULL);
    return RADE_TX_N_EOO_BITS;
}

void rade_tx_state_set_eoo_bits(rade_tx_state *tx, float eoo_bits[]) {
    assert(tx != NULL);
    assert(eoo_bits != NULL);
    memcpy(tx->eoo_bits, eoo_bits, sizeof(float) * RADE_TX_N_EOO_BITS);
}

/*---------------------------------------------------------------------------*\
                         TRANSMISSION
\*---------------------------------------------------------------------------*/

int rade_tx_process(rade_tx_state *tx, RADE_COMP *tx_out, float *features_in) {
    assert(tx != NULL);
    assert(tx_out != NULL);
    assert(features_in != NULL);

    int arch = 0;
#ifdef OPUS_HAVE_RTCD
    arch = opus_select_arch();
#endif

    /* Encode features to latent vectors */
    float z[RADE_NZMF * RADE_LATENT_DIM];
    for (int i = 0; i < RADE_FRAMES_PER_STEP; i++) {
        rade_core_encoder(&tx->enc_state, &tx->enc_model,
                          &z[(i / (RADE_FRAMES_PER_STEP / RADE_NZMF)) * RADE_LATENT_DIM],
                          &features_in[i * tx->num_features],
                          arch, tx->bottleneck);
    }

    /* OFDM modulate to IQ samples */
    int n_out = rade_ofdm_mod_frame(&tx->ofdm, tx_out, z);

    return n_out;
}

int rade_tx_state_eoo(rade_tx_state *tx, RADE_COMP *tx_eoo_out) {
    assert(tx != NULL);
    assert(tx_eoo_out != NULL);

    int n_out;
    const RADE_COMP *eoo = rade_ofdm_get_eoo(&tx->ofdm, &n_out);
    memcpy(tx_eoo_out, eoo, sizeof(RADE_COMP) * n_out);

    return n_out;
}
