/*---------------------------------------------------------------------------*\

  FILE........: ofdm.c
  AUTHORS.....: David Rowe & Steve Sampson
  DATE CREATED: June 2017

  A Library of functions that implement a QPSK OFDM modem, C port of
  the Octave functions in ofdm_lib.m

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2017/2018 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <complex.h>

#include "comp.h"
#include "ofdm_internal.h"
#include "codec2_ofdm.h"
#include "filter.h"

/* Static Prototypes */

static void dft(struct OFDM *, complex float *, complex float *);
static void idft(struct OFDM *, complex float *, complex float *);
static complex float vector_sum(complex float *, int);

/* Defines */

#define max( a, b ) ( ((a) > (b)) ? (a) : (b) )
#define min( a, b ) ( ((a) < (b)) ? (a) : (b) )

/* Constants */

/*
 * QPSK Quadrant bit-pair values - Gray Coded
 */
static const complex float constellation[] = {
    1.0f + 0.0f * I,
    0.0f + 1.0f * I,
    0.0f - 1.0f * I,
    -1.0f + 0.0f * I
};

/*
 * These pilots are compatible with Octave version
 */
static const float pilotvalues[] = {
  -1.0f,-1.0f, 1.0f, 1.0f,-1.0f,-1.0f,-1.0f, 1.0f,
  -1.0f, 1.0f,-1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
   1.0f, 1.0f, 1.0f,-1.0f,-1.0f, 1.0f,-1.0f, 1.0f,
  -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
   1.0f, 1.0f, 1.0f,-1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
   1.0f,-1.0f,-1.0f,-1.0f,-1.0f,-1.0f,-1.0f, 1.0f,
  -1.0f, 1.0f,-1.0f, 1.0f,-1.0f,-1.0f, 1.0f,-1.0f,
   1.0f, 1.0f, 1.0f, 1.0f,-1.0f, 1.0f,-1.0f, 1.0f
};

/*
 * A Unique Word (UW) used to verify we have modem frame sync when we
 * try a candidate coarse timing and freq offset.  The UW bits/symbols
 * are distributed through the modem frame using the index (ind)
 * tables below.  The indexes in uw_ind_sym [] and uw_ind are Octave
 * start from 1 format, subtract 1 to index C arrays.
 */

static const int tx_uw[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; /* UW bits    */
static complex float tx_uw_syms[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; /* UW QPSK symbols */
static const int uw_ind[] = {19, 20, 37, 38, 55, 56, 73, 74, 91, 92}; /* index in modem frame of UW bits */
static const int uw_ind_sym[] = {10, 19, 28, 37, 46}; /* index into modem frame of UW symbols */

/* static variables */

static struct OFDM_CONFIG ofdm_config;

static float ofdm_centre; /* Center frequency */
static float ofdm_fs; /* Sample rate */
static float ofdm_ts; /* Symbol cycle time */
static float ofdm_rs; /* Symbol rate */
static float ofdm_tcp; /* Cyclic prefix duration */
static float ofdm_timing_mx_thresh; /* See 700D Part 4 Acquisition blog post and ofdm_dev.m routines for how this was set */

static int ofdm_nc; /* NS-1 data symbols between pilots  */
static int ofdm_ns;
static int ofdm_bps; /* Bits per symbol */
static int ofdm_m; /* duration of each symbol in samples */
static int ofdm_ncp; /* duration of CP in samples */

static int ofdm_ftwindowwidth;
static int ofdm_bitsperframe;
static int ofdm_rowsperframe;
static int ofdm_samplesperframe;
static int ofdm_max_samplesperframe;
static int ofdm_rxbuf;
static int ofdm_ntxtbits; /* reserve bits/frame for auxillary text information */
static int ofdm_nuwbits; /* Unique word, used for positive indication of lock */
static int ofdm_state_str;

/* Functions -------------------------------------------------------------------*/

static float cnormf(complex float val) {
    float realf = crealf(val);
    float imagf = cimagf(val);

    return realf * realf + imagf * imagf;
}

/*
 * ------------
 * ofdm_create
 * ------------
 *
 * Returns OFDM data structure on success
 * Return NULL on fail
 *
 * If you want the defaults, call this with config structure
 * and the NC setting to 0. This will fill the structure with
 * default values of the original OFDM modem.
 */

struct OFDM *ofdm_create(const struct OFDM_CONFIG *config) {
    struct OFDM *ofdm;
    int i, j, n;

    /* Check if called correctly */

    if (config == NULL) {
        return NULL;
    }

    if (config->nc == 0) {
        /* Fill in default values */

        ofdm_nc = 17; /* Number of carriers */
        ofdm_ns = 8; /* Number of Symbol frames */
        ofdm_bps = 2; /* Bits per Symbol */
        ofdm_ts = 0.018f;
        ofdm_rs = 1.0f / ofdm_ts; /* Symbol Rate */
        ofdm_tcp = .002f; /* Cyclic Prefix duration */
        ofdm_centre = 1500.0f; /* Centre Audio Frequency */
        ofdm_fs = 8000.0f; /* Sample Frequency */
        ofdm_m = (int) (ofdm_fs / ofdm_rs); /* 144 */
        ofdm_ncp = (int) (ofdm_tcp * ofdm_fs); /* 16 */
        ofdm_ntxtbits = 4;
        ofdm_state_str = 16;
        ofdm_ftwindowwidth = 11;
        ofdm_timing_mx_thresh = 0.30f;
     } else {
        /* Use the users values */

        ofdm_nc = config->nc; /* Number of carriers */
        ofdm_ns = config->ns; /* Number of Symbol frames */
        ofdm_bps = config->bps; /* Bits per Symbol */
        ofdm_ts = config->ts;
        ofdm_rs = (1.0f / ofdm_ts); /* Symbol Rate */
        ofdm_tcp = config->tcp; /* Cyclic Prefix duration */
        ofdm_centre = config->centre; /* Centre Audio Frequency */
        ofdm_fs = config->fs; /* Sample Frequency */
        ofdm_m = (int) (ofdm_fs / ofdm_rs); /* 144 */
        ofdm_ncp = (int) (ofdm_tcp * ofdm_fs); /* 16 */
        ofdm_ntxtbits = config->txtbits;
        ofdm_state_str = config->state_str;
        ofdm_ftwindowwidth = config->ftwindowwidth;
        ofdm_timing_mx_thresh = config->ofdm_timing_mx_thresh;
    }

    /* Copy structure into global */

    ofdm_config.centre = ofdm_centre;
    ofdm_config.fs = ofdm_fs;
    ofdm_config.rs = ofdm_rs;
    ofdm_config.ts = ofdm_ts;
    ofdm_config.tcp = ofdm_tcp;
    ofdm_config.ofdm_timing_mx_thresh = ofdm_timing_mx_thresh;
    ofdm_config.nc = ofdm_nc;
    ofdm_config.ns = ofdm_ns;
    ofdm_config.bps = ofdm_bps;
    ofdm_config.txtbits = ofdm_ntxtbits;
    ofdm_config.state_str = ofdm_state_str;
    ofdm_config.ftwindowwidth = ofdm_ftwindowwidth;

    /* Calculate sizes from config param */

    ofdm_bitsperframe = (ofdm_ns - 1) * (ofdm_nc * ofdm_bps);
    ofdm_rowsperframe = ofdm_bitsperframe / (ofdm_nc * ofdm_bps);
    ofdm_samplesperframe = ofdm_ns * (ofdm_m + ofdm_ncp);
    ofdm_max_samplesperframe = ofdm_samplesperframe + (ofdm_m + ofdm_ncp) / 4;
    ofdm_rxbuf = 3 * ofdm_samplesperframe + 3 * (ofdm_m + ofdm_ncp);
    ofdm_nuwbits = (ofdm_ns - 1) * ofdm_bps - ofdm_ntxtbits;

    /* Were ready to start filling in the OFDM structure now */

    if ((ofdm = (struct OFDM *) malloc(sizeof (struct OFDM))) == NULL) {
        return NULL;
    }

    ofdm->pilot_samples = malloc(sizeof (complex float) * (ofdm_m + ofdm_ncp));
    ofdm->rxbuf = malloc(sizeof (complex float) * ofdm_rxbuf);
    ofdm->pilots = malloc(sizeof (complex float) * (ofdm_nc + 2));

    /*
     * rx_sym is a 2D array of variable size
     *
     * allocate rx_sym row storage. It is a pointer to a pointer
     */

    ofdm->rx_sym = malloc(sizeof (complex float) * (ofdm_ns + 3));

    /* allocate rx_sym column storage */ 

    for (i = 0; i <  (ofdm_ns + 3); i++) {
        ofdm->rx_sym[i] = (complex float *) malloc(sizeof(complex float) * (ofdm_nc + 2));
    }

    /* The rest of these are 1D arrays of variable size */

    ofdm->rx_np = malloc(sizeof (complex float) * (ofdm_rowsperframe * ofdm_nc));
    ofdm->w = malloc(sizeof (float) * (ofdm_nc + 2));
    ofdm->rx_amp = malloc(sizeof (float) * (ofdm_rowsperframe * ofdm_nc));
    ofdm->aphase_est_pilot_log = malloc(sizeof (float) * (ofdm_rowsperframe * ofdm_nc));
    ofdm->tx_uw = malloc(sizeof (int) * ofdm_nuwbits);
    ofdm->sync_state = malloc(sizeof (char) * ofdm_state_str);
    ofdm->last_sync_state = malloc(sizeof (char) * ofdm_state_str);
    ofdm->sync_state_interleaver = malloc(sizeof (char) * ofdm_state_str);
    ofdm->last_sync_state_interleaver = malloc(sizeof (char) * ofdm_state_str);

    /* store complex BPSK pilot symbols */

    assert(sizeof (pilotvalues) >= (ofdm_nc + 2) * sizeof (float));

    /* There are only 64 pilot values to use from */

    for (i = 0; i < (ofdm_nc + 2); i++) {
        ofdm->pilots[i] = pilotvalues[i] + 0.0f * I;
    }

    /* carrier tables for up and down conversion */

    float alower = ofdm_centre - ofdm_rs * ((float) ofdm_nc / 2);
    int Nlower = floorf(alower / ofdm_rs);

    for (i = 0, n = Nlower; i < (ofdm_nc + 2); i++, n++) {
        ofdm->w[i] = (TAU * (float) n) / (ofdm_fs / ofdm_rs);
    }

    for (i = 0; i < ofdm_rxbuf; i++) {
        ofdm->rxbuf[i] = 0.0f + 0.0f * I;
    }

    for (i = 0; i < (ofdm_ns + 3); i++) {
        for (j = 0; j < (ofdm_nc + 2); j++) {
            ofdm->rx_sym[i][j] = 0.0f + 0.0f * I;
        }
    }

    for (i = 0; i < ofdm_rowsperframe * ofdm_nc; i++) {
        ofdm->rx_np[i] = 0.0f + 0.0f * I;
    }

    for (i = 0; i < ofdm_rowsperframe; i++) {
        for (j = 0; j < ofdm_nc; j++) {
            ofdm->aphase_est_pilot_log[ofdm_nc * i + j] = 0.0f + 0.0f * I;
            ofdm->rx_amp[ofdm_nc * i + j] = 0.0f + 0.0f * I;
        }
    }

    /* default settings of options and states */

    ofdm->verbose = 0;
    ofdm->timing_en = true;
    ofdm->foff_est_en = true;
    ofdm->phase_est_en = true;

    ofdm->foff_est_gain = 0.05f;
    ofdm->foff_est_hz = 0.0f;
    ofdm->sample_point = 0;
    ofdm->timing_est = 0;
    ofdm->timing_valid = 0;
    ofdm->timing_mx = 0.0f;
    ofdm->nin = ofdm_samplesperframe;
    ofdm->mean_amp = 0.0f;
    ofdm->foff_metric = 0.0f + 0.0f * I;

    /* sync state machine */

    for (i = 0; i < ofdm_nuwbits; i++) {
        ofdm->tx_uw[i] = tx_uw[i];
    }

    strncpy(ofdm->sync_state, "search", ofdm_state_str);
    strncpy(ofdm->last_sync_state, "search", ofdm_state_str);
    ofdm->uw_errors = 0;
    ofdm->sync_counter = 0;
    ofdm->frame_count = 0;
    ofdm->sync_start = 0;
    ofdm->sync_end = 0;
    ofdm->sync_mode = OFDM_SYNC_AUTO;

    strncpy(ofdm->sync_state_interleaver, "search", ofdm_state_str);
    strncpy(ofdm->last_sync_state_interleaver, "search", ofdm_state_str);
    ofdm->frame_count_interleaver = 0;

    /* create the OFDM waveform */

    complex float temp[ofdm_m];

    idft(ofdm, temp, ofdm->pilots);

    /*
     * pilot_samples is 160 samples, but timing and freq offset est
     * were found by experiment to work better without a cyclic
     * prefix, so we uses zeroes instead.
     */

    /* zero out Cyclic Prefix (CP) values */

    for (i = 0; i < ofdm_ncp; i++) {
        ofdm->pilot_samples[i] = 0.0f + 0.0f * I;
    }

    // From Octave: states.timing_norm = Npsam*(rate_fs_pilot_samples*rate_fs_pilot_samples');

    /* Now copy the whole thing after the above */

    for (i = ofdm_ncp, j = 0; j < ofdm_m; i++, j++) {
        ofdm->pilot_samples[i] = temp[j];
    }

    /* calculate constant used to normalise timing correlation maximum */

    float acc = 0.0f;

    for (i = 0; i < (ofdm_m + ofdm_ncp); i++) {
        acc += cnormf(ofdm->pilot_samples[i]);
    }

    ofdm->timing_norm = (ofdm_m + ofdm_ncp) * acc;
    ofdm->clock_offset_counter = 0;
    ofdm->sig_var = ofdm->noise_var = 1.0f;
    ofdm->tx_bpf_en = false;

    // Transmit bandpass filter; complex coefficients, center frequency 1500 hz

    //quisk_filt_cfInit(&ofdm->ofdm_tx_bpf, filtP750S1040, sizeof(filtP750S1040) / sizeof(float));

    quisk_filt_cfInit(&ofdm->ofdm_tx_bpf, filtP550S750, sizeof (filtP550S750) / sizeof (float));
    quisk_cfTune(&ofdm->ofdm_tx_bpf, 1500.0f / ofdm_fs); // fixed value

    return ofdm; /* Success */
}

void ofdm_destroy(struct OFDM *ofdm) {
    int i;

    quisk_filt_destroy(&ofdm->ofdm_tx_bpf);
    free(ofdm->pilot_samples);
    free(ofdm->rxbuf);
    free(ofdm->pilots);

    for (i = 0; i < (ofdm_ns + 3); i++) { /* 2D array */
        free(ofdm->rx_sym[i]);
    }

    free(ofdm->rx_sym);

    free(ofdm->rx_np);
    free(ofdm->w);
    free(ofdm->rx_amp);
    free(ofdm->aphase_est_pilot_log);
    free(ofdm->tx_uw);
    free(ofdm->sync_state);
    free(ofdm->last_sync_state);
    free(ofdm->sync_state_interleaver);
    free(ofdm->last_sync_state_interleaver);
    free(ofdm);
}

/* Gray coded QPSK modulation function */

complex float qpsk_mod(int *bits) {
    return constellation[(bits[1] << 1) | bits[0]];
}

/* Gray coded QPSK demodulation function */

void qpsk_demod(complex float symbol, int *bits) {
    complex float rotate = symbol * cexpf(I * (M_PI / 4.0f));

    bits[0] = crealf(rotate) < 0.0f;
    bits[1] = cimagf(rotate) < 0.0f;
}

/* convert frequency domain into time domain */

static void idft(struct OFDM *ofdm, complex float *result, complex float *vector) {
    float inv_m = (1.0f / (float) ofdm_m);
    int row, col;

    for (row = 0; row < ofdm_m; row++) {
        result[row] = 0.0f + 0.0f * I;

        for (col = 0; col < (ofdm_nc + 2); col++) {
            result[row] = result[row] + (vector[col] * cexpf(I * ofdm->w[col] * row));
        }

        result[row] = result[row] * inv_m;
    }
}

/* convert time domain into frequency domain */

static void dft(struct OFDM *ofdm, complex float *result, complex float *vector) {
    int row, col;

    for (col = 0; col < (ofdm_nc + 2); col++) {
        result[col] = 0.0f + 0.0f * I;

        for (row = 0; row < ofdm_m; row++) {
            result[col] = result[col] + (vector[row] * conjf(cexpf(I * ofdm->w[col] * row)));
        }
    }
}

static complex float vector_sum(complex float *a, int num_elements) {
    int i;

    complex float sum = 0.0f + 0.0f * I;

    for (i = 0; i < num_elements; i++) {
        sum = sum + a[i];
    }

    return sum;
}

/*
 * Correlates the OFDM pilot symbol samples with a window of received
 * samples to determine the most likely timing offset.  Combines two
 * frames pilots so we need at least Nsamperframe+M+Ncp samples in rx.
 *
 * Can be used for acquisition (coarse timing), and fine timing.
 *
 * Unlike Octave version use states to return a few values.
 */

static int est_timing(struct OFDM *ofdm, complex float *rx, int length) {
    complex float csam;
    int Ncorr = length - (ofdm_samplesperframe + (ofdm_m + ofdm_ncp));
    int SFrame = ofdm_samplesperframe;
    float corr[Ncorr];
    int i, j;

    float acc = 0.0f;

    for (i = 0; i < length; i++) {
        acc += cnormf(rx[i]);
    }

    float av_level = 2.0f * sqrtf(ofdm->timing_norm * acc / length) + 1E-12f;

    for (i = 0; i < Ncorr; i++) {
        complex float corr_st = 0.0f + 0.0f * I;
        complex float corr_en = 0.0f + 0.0f * I;

        for (j = 0; j < (ofdm_m + ofdm_ncp); j++) {
            csam = conjf(ofdm->pilot_samples[j]);

            corr_st = corr_st + (rx[i + j         ] * csam);
            corr_en = corr_en + (rx[i + j + SFrame] * csam);
        }

        corr[i] = (cabsf(corr_st) + cabsf(corr_en)) / av_level;
    }

    /* find the max magnitude and its index */

    float timing_mx = 0.0f;
    int timing_est = 0;

    for (i = 0; i < Ncorr; i++) {
        if (corr[i] > timing_mx) {
            timing_mx = corr[i];
            timing_est = i;
        }
    }

    ofdm->timing_mx = timing_mx;
    ofdm->timing_valid = timing_mx > ofdm_timing_mx_thresh;

    if (ofdm->verbose > 1) {
        fprintf(stderr, "  av_level: %f  max: %f timing_est: %d timing_valid: %d\n", (double) av_level, (double) ofdm->timing_mx, timing_est, ofdm->timing_valid);
    }

    return timing_est;
}

/*
 * Determines frequency offset at current timing estimate, used for
 * coarse freq offset estimation during acquisition.
 *
 * Freq offset is based on an averaged statistic that was found to be
 * necessary to generate good quality estimates.
 *
 * Keep calling it when in trial or actual sync to keep statistic
 * updated, in case we lose sync.
 */

static float est_freq_offset(struct OFDM *ofdm, complex float *rx, int timing_est) {
    complex float csam1, csam2;
    int j, k;
    float foff_est;

    /*
      Freq offset can be considered as change in phase over two halves
      of pilot symbols.  We average this statistic over this and next
      frames pilots.
     */

    complex float p1, p2, p3, p4;
    p1 = p2 = p3 = p4 = 0.0f + 0.0f * I;

    /* calculate phase of pilots at half symbol intervals */

    for (j = 0, k = (ofdm_m + ofdm_ncp) / 2; j < (ofdm_m + ofdm_ncp) / 2; j++, k++) {
        csam1 = conjf(ofdm->pilot_samples[j]);
        csam2 = conjf(ofdm->pilot_samples[k]);

        /* pilot at start of frame */

        p1 = p1 + (rx[timing_est + j] * csam1);
        p2 = p2 + (rx[timing_est + k] * csam2);

        /* pilot at end of frame */

        p3 = p3 + (rx[timing_est + j + ofdm_samplesperframe] * csam1);
        p4 = p4 + (rx[timing_est + k + ofdm_samplesperframe] * csam2);
    }

    /* Calculate sample rate of phase samples, we are sampling phase
       of pilot at half a symbol intervals */

    float Fs1 = ofdm_fs / ((ofdm_m + ofdm_ncp) / 2);

    /* subtract phase of adjacent samples, rate of change of phase is
       frequency est.  We combine samples from either end of frame to
       improve estimate.  Small real 1E-12 term to prevent instability
       with 0 inputs. */

    ofdm->foff_metric = 0.9f * ofdm->foff_metric + 0.1f * (conjf(p1) * p2 + conjf(p3) * p4);
    foff_est = Fs1 * cargf(ofdm->foff_metric + 1E-12f) / TAU;

    if (ofdm->verbose > 1) {
        fprintf(stderr, "  foff_metric: %f %f foff_est: %f\n", creal(ofdm->foff_metric), cimag(ofdm->foff_metric), (double) foff_est);
    }

    return foff_est;
}

/*
 * ----------------------------------------------
 * ofdm_txframe - modulates one frame of symbols
 * ----------------------------------------------
 */

void ofdm_txframe(struct OFDM *ofdm, complex float *tx, complex float *tx_sym_lin) {
    complex float aframe[ofdm_ns][ofdm_nc + 2];
    complex float asymbol[ofdm_m];
    complex float asymbol_cp[ofdm_m + ofdm_ncp];
    int i, j, k, m;

    /* initialize aframe to complex zero */

    for (i = 0; i < ofdm_ns; i++) {
        for (j = 0; j < (ofdm_nc + 2); j++) {
            aframe[i][j] = 0.0f + 0.0f * I;
        }
    }

    /* copy in a row of complex pilots to first row */

    for (i = 0; i < (ofdm_nc + 2); i++) {
        aframe[0][i] = ofdm->pilots[i];
    }

    /* Place symbols in multi-carrier frame with pilots */
    /* This will place boundary values of complex zero around data */

    for (i = 1; i <= ofdm_rowsperframe; i++) {

        /* copy in the Nc complex values with [0 Nc 0] or (Nc + 2) total */

        for (j = 1; j < (ofdm_nc + 1); j++) {
            aframe[i][j] = tx_sym_lin[((i - 1) * ofdm_nc) + (j - 1)];
        }
    }

    /* OFDM up-convert symbol by symbol so we can add CP */

    for (i = 0, m = 0; i < ofdm_ns; i++, m += (ofdm_m + ofdm_ncp)) {
        idft(ofdm, asymbol, aframe[i]);

        /* Copy the last Ncp samples to the front */

        for (j = (ofdm_m - ofdm_ncp), k = 0; j < ofdm_m; j++, k++) {
            asymbol_cp[k] = asymbol[j];
        }

        /* Now copy the all samples for this row after it */

        for (j = ofdm_ncp, k = 0; k < ofdm_m; j++, k++) {
            asymbol_cp[j] = asymbol[k];
        }

        /* Now move row to the tx output */

        for (j = 0; j < (ofdm_m + ofdm_ncp); j++) {
            tx[m + j] = asymbol_cp[j];
        }
    }

    /* optional Tx Band Pass Filter */

    if (ofdm->tx_bpf_en == true) {
        complex float tx_filt[ofdm_samplesperframe];

        quisk_ccfFilter(tx, tx_filt, ofdm_samplesperframe, &ofdm->ofdm_tx_bpf);
        memcpy(tx, tx_filt, ofdm_samplesperframe * sizeof (complex float));
    }
}

struct OFDM_CONFIG *ofdm_get_config_param() {
    return &ofdm_config;
}

int ofdm_get_nin(struct OFDM *ofdm) {
    return ofdm->nin;
}

int ofdm_get_samples_per_frame() {
    return ofdm_samplesperframe;
}

int ofdm_get_max_samples_per_frame() {
    return 2 * ofdm_max_samplesperframe;
}

int ofdm_get_bits_per_frame() {
    return ofdm_bitsperframe;
}

void ofdm_set_verbose(struct OFDM *ofdm, int level) {
    ofdm->verbose = level;
}

void ofdm_set_timing_enable(struct OFDM *ofdm, bool val) {
    ofdm->timing_en = val;

    if (ofdm->timing_en == false) {
        /* manually set ideal timing instant */
        ofdm->sample_point = (ofdm_ncp - 1);
    }
}

void ofdm_set_foff_est_enable(struct OFDM *ofdm, bool val) {
    ofdm->foff_est_en = val;
}

void ofdm_set_phase_est_enable(struct OFDM *ofdm, bool val) {
    ofdm->phase_est_en = val;
}

void ofdm_set_off_est_hz(struct OFDM *ofdm, float val) {
    ofdm->foff_est_hz = val;
}

void ofdm_set_tx_bpf(struct OFDM *ofdm, bool val) {
    ofdm->tx_bpf_en = val;
}

/*
 * --------------------------------------
 * ofdm_mod - modulates one frame of bits
 * --------------------------------------
 */

void ofdm_mod(struct OFDM *ofdm, COMP *result, const int *tx_bits) {
    int length = ofdm_bitsperframe / ofdm_bps;
    complex float tx[ofdm_samplesperframe];
    complex float tx_sym_lin[length];
    int dibit[2];
    int s, i;

    if (ofdm_bps == 1) {
        /* Here we will have Nbitsperframe / 1 */

        for (s = 0; s < length; s++) {
            tx_sym_lin[s] = (float) (2 * tx_bits[s] - 1) + 0.0f * I;
        }
    } else if (ofdm_bps == 2) {
        /* Here we will have Nbitsperframe / 2 */

        for (s = 0, i = 0; i < length; s += 2, i++) {
            dibit[0] = tx_bits[s + 1] & 0x1;
            dibit[1] = tx_bits[s    ] & 0x1;
            tx_sym_lin[i] = qpsk_mod(dibit);
        }
    }

    ofdm_txframe(ofdm, tx, tx_sym_lin);

    /* convert to comp */

    for (i = 0; i < ofdm_samplesperframe; i++) {
        result[i].real = crealf(tx[i]);
        result[i].imag = cimagf(tx[i]);
    }
}

/*
 * ----------------------------------------------------------------------------------
 * ofdm_sync_search - attempts to find coarse sync parameters for modem initial sync
 * ----------------------------------------------------------------------------------
 */

int ofdm_sync_search(struct OFDM *ofdm, COMP *rxbuf_in) {
    int i, j;

    /* insert latest input samples into rxbuf so it is primed for when
       we have to call ofdm_demod() */

    for (i = 0, j = ofdm->nin; i < (ofdm_rxbuf - ofdm->nin); i++, j++) {
        ofdm->rxbuf[i] = ofdm->rxbuf[j];
    }

    /* insert latest input samples onto tail of rxbuf */

    for (i = (ofdm_rxbuf - ofdm->nin), j = 0; i < ofdm_rxbuf; i++, j++) {
        ofdm->rxbuf[i] = rxbuf_in[j].real + rxbuf_in[j].imag * I;
    }

    /* Attempt coarse timing estimate (i.e. detect start of frame) */

    int st = ofdm_m + ofdm_ncp + ofdm_samplesperframe;
    int en = st + 2 * ofdm_samplesperframe;
    int ct_est = est_timing(ofdm, &ofdm->rxbuf[st], (en - st));

    ofdm->coarse_foff_est_hz = est_freq_offset(ofdm, &ofdm->rxbuf[st], ct_est);

    if (ofdm->verbose) {
        fprintf(stderr, "   ct_est: %4d foff_est: %4.1f timing_valid: %d timing_mx: %5.4f\n",
                ct_est, (double) ofdm->coarse_foff_est_hz, ofdm->timing_valid, (double) ofdm->timing_mx);
    }

    if (ofdm->timing_valid) {
        /* potential candidate found .... */

        /* calculate number of samples we need on next buffer to get into sync */

        ofdm->nin = ofdm_samplesperframe + ct_est;

        /* reset modem states */

        ofdm->sample_point = ofdm->timing_est = 0;
        ofdm->foff_est_hz = ofdm->coarse_foff_est_hz;
    } else {
        ofdm->nin = ofdm_samplesperframe;
    }

    return ofdm->timing_valid;
}

/*
 * ------------------------------------------
 * ofdm_demod - Demodulates one frame of bits
 * ------------------------------------------
 */

void ofdm_demod(struct OFDM *ofdm, int *rx_bits, COMP *rxbuf_in) {
    complex float aphase_est_pilot_rect;
    float aphase_est_pilot[ofdm_nc + 2];
    float aamp_est_pilot[ofdm_nc + 2];
    float freq_err_hz;
    int i, j, k, rr, st, en, ft_est;
    int prev_timing_est = ofdm->timing_est;

    /* shift the buffer left based on nin */

    for (i = 0, j = ofdm->nin; i < (ofdm_rxbuf - ofdm->nin); i++, j++) {
        ofdm->rxbuf[i] = ofdm->rxbuf[j];
    }

    /* insert latest input samples onto tail of rxbuf */

    for (i = (ofdm_rxbuf - ofdm->nin), j = 0; i < ofdm_rxbuf; i++, j++) {
        ofdm->rxbuf[i] = rxbuf_in[j].real + rxbuf_in[j].imag * I;
    }

    /*
     * get user and calculated freq offset
     */

    float woff_est = TAU * ofdm->foff_est_hz / ofdm_fs;

    /* update timing estimate -------------------------------------------------- */

    if (ofdm->timing_en == true) {
        /* update timing at start of every frame */

        st = ((ofdm_m + ofdm_ncp) + ofdm_samplesperframe) - floorf(ofdm_ftwindowwidth / 2) + ofdm->timing_est;
        en = st + ofdm_samplesperframe - 1 + (ofdm_m + ofdm_ncp) + ofdm_ftwindowwidth;

        complex float work[(en - st)];

        /*
         * Adjust for the frequency error by shifting the phase
         * using a conjugate multiply
         */

        for (i = st, j = 0; i < en; i++, j++) {
            work[j] = ofdm->rxbuf[i] * cexpf(-I * woff_est * i);
        }

        ft_est = est_timing(ofdm, work, (en - st));
        ofdm->timing_est += (ft_est - ceilf(ofdm_ftwindowwidth / 2));

        /* keep the freq est statistic updated in case we lose sync,
           note we supply it with uncorrected rxbuf, note
           ofdm->coarse_fest_off_hz is unused in normal operation,
           but stored for use in tofdm.c */

        ofdm->coarse_foff_est_hz = est_freq_offset(ofdm, &ofdm->rxbuf[st], ft_est);

        /* first frame in trial sync will have a better freq offset est - lets use it */

        if (ofdm->frame_count == 0) {
            ofdm->foff_est_hz = ofdm->coarse_foff_est_hz;
            woff_est = TAU * ofdm->foff_est_hz / ofdm_fs;
        }

        if (ofdm->verbose > 1) {
            fprintf(stderr, "  ft_est: %2d timing_est: %2d sample_point: %2d\n", ft_est, ofdm->timing_est, ofdm->sample_point);
        }

        /* Black magic to keep sample_point inside cyclic prefix.  Or something like that. */

        ofdm->sample_point = max(ofdm->timing_est + (ofdm_ncp / 4), ofdm->sample_point);
        ofdm->sample_point = min(ofdm->timing_est + ofdm_ncp, ofdm->sample_point);
    }
    
    /*
     * Convert the time-domain samples to the frequency-domain using the rx_sym
     * data matrix. This will be  Nc+2 carriers of 11 symbols.
     *
     * You will notice there are Nc+2 BPSK symbols for each pilot symbol, and that there
     * are Nc QPSK symbols for each data symbol.
     *
     *  XXXXXXXXXXXXXXXXX  <-- Timing Slip
     * PPPPPPPPPPPPPPPPPPP <-- Previous Frames Pilot
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD      Ignore these past data symbols
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     * PPPPPPPPPPPPPPPPPPP <-- This Frames Pilot
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD      These are the current data symbols to be decoded
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     * PPPPPPPPPPPPPPPPPPP <-- Next Frames Pilot
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD      Ignore these next data symbols
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     *  DDDDDDDDDDDDDDDDD
     * PPPPPPPPPPPPPPPPPPP <-- Future Frames Pilot
     *  XXXXXXXXXXXXXXXXX  <-- Timing Slip
     *
     * So this algorithm will have seven data symbols and four pilot symbols to process.
     * The average of the four pilot symbols is our phase estimation.
     */

    for (i = 0; i < (ofdm_ns + 3); i++) {
        for (j = 0; j < (ofdm_nc + 2); j++) {
            ofdm->rx_sym[i][j] = 0.0f + 0.0f * I;
        }
    }

    /*
     * "Previous" pilot symbol is one modem frame above.
     */

    st = (ofdm_m + ofdm_ncp) + 1 + ofdm->sample_point;
    en = st + ofdm_m;

    complex float work[ofdm_m];

    /* down-convert at current timing instant---------------------------------- */

    for (j = st, k = 0; j < en; j++, k++) {
        work[k] = ofdm->rxbuf[j] * cexpf(-I * woff_est * j);
    }

    /*
     * Each symbol is of course (ofdm_m + ofdm_ncp) samples long and
     * becomes Nc+2 carriers after DFT.
     *
     * We put this carrier pilot symbol at the top of our matrix:
     *
     * 1 .................. Nc+2
     *
     * +----------------------+
     * |    Previous Pilot    |  rx_sym[0]
     * +----------------------+
     * |                      |
     *
     */

    dft(ofdm, ofdm->rx_sym[0], work);

    /*
     * "This" pilot comes after the extra symbol allotted at the top, and after
     * the "previous" pilot and previous data symbols (let's call it, the previous
     * modem frame).
     *
     * So we will now be starting at "this" pilot symbol, and continuing to the
     * "next" pilot symbol.
     *
     * In this routine we also process the current data symbols.
     */
    
    for (rr = 0; rr < (ofdm_ns + 1); rr++) {
        st = (ofdm_m + ofdm_ncp) + ofdm_samplesperframe + (rr * (ofdm_m + ofdm_ncp)) + 1 + ofdm->sample_point;
        en = st + ofdm_m;

        /* down-convert at current timing instant---------------------------------- */

        for (j = st, k = 0; j < en; j++, k++) {
            work[k] = ofdm->rxbuf[j] * cexpf(-I * woff_est * j);
        }

        /*
         * We put these Nc+2 carrier symbols into our matrix after the previous pilot:
         *
         * 1 .................. Nc+2
         *
         * |    Previous Pilot    |  rx_sym[0]
         * +----------------------+
         * |      This Pilot      |  rx_sym[1]
         * +----------------------+
         * |         Data         |  rx_sym[2]
         * +----------------------+
         * |         Data         |  rx_sym[3]
         * +----------------------+
         * |         Data         |  rx_sym[4]
         * +----------------------+
         * |         Data         |  rx_sym[5]
         * +----------------------+
         * |         Data         |  rx_sym[6]
         * +----------------------+
         * |         Data         |  rx_sym[7]
         * +----------------------+
         * |         Data         |  rx_sym[8]
         * +----------------------+
         * |      Next Pilot      |  rx_sym[9]
         * +----------------------+
         * |                      |  rx_sym[10]
         */

        dft(ofdm, ofdm->rx_sym[rr + 1], work);
    }

    /*
     * OK, now we want to process to the "future" pilot symbol. This is after
     * the "next" modem frame.
     *
     * We are ignoring the data symbols between the "next" pilot and "future" pilot.
     * We only want the "future" pilot symbol, to perform the averaging of all pilots.
     */

    st = (ofdm_m + ofdm_ncp) + (3 * ofdm_samplesperframe) + 1 + ofdm->sample_point;
    en = st + ofdm_m;

    /* down-convert at current timing instant---------------------------------- */

    for (j = st, k = 0; j < en; j++, k++) {
        work[k] = ofdm->rxbuf[j] * cexpf(-I * woff_est * j);
    }

    /*
     * We put the future pilot after all the previous symbols in the matrix:
     *
     * 1 .................. Nc+2
     *
     * |                      |  rx_sym[9]
     * +----------------------+
     * |     Future Pilot     |  rx_sym[10]
     * +----------------------+
     */

    dft(ofdm, ofdm->rx_sym[ofdm_ns + 2], work);

    /*
     * We are finished now with the DFT and down conversion
     * From here on down we are in frequency domain
     */

    /* est freq err based on all carriers ------------------------------------ */

    if (ofdm->foff_est_en == true) {
        /*
         * sym[1] is (this) pilot symbol, sym[9] is (next) pilot symbol.
         *
         * By subtracting the two averages of these pilots, we find the frequency
         * by the change in phase over time.
         */

        complex float freq_err_rect =
                conjf(vector_sum(ofdm->rx_sym[1], ofdm_nc + 2)) *
                vector_sum(ofdm->rx_sym[ofdm_ns + 1], ofdm_nc + 2);

        /* prevent instability in atan(im/re) when real part near 0 */

        freq_err_rect = freq_err_rect + 1E-6f;

        freq_err_hz = cargf(freq_err_rect) * ofdm_rs / (TAU * ofdm_ns);
        ofdm->foff_est_hz += (ofdm->foff_est_gain * freq_err_hz);
    }

    /* OK - now estimate and correct pilot phase  ---------------------------------- */

    for (i = 0; i < (ofdm_nc + 2); i++) {
        aphase_est_pilot[i] = 10.0f;
        aamp_est_pilot[i] = 0.0f;
    }

    /*
     * Basically we divide the Nc+2 pilots into groups of 3
     *
     * Then average the phase surrounding each of the data symbols.
     */

    for (i = 1; i < (ofdm_nc + 1); i++) {
        complex float symbol[3];

        for (j = (i - 1), k = 0; j < (i + 2); j++, k++) {
            symbol[k] = ofdm->rx_sym[1][j] * conjf(ofdm->pilots[j]); /* this pilot conjugate */
        }
        
        aphase_est_pilot_rect = vector_sum(symbol, 3);

        for (j = (i - 1), k = 0; j < (i + 2); j++, k++) {
            symbol[k] = ofdm->rx_sym[ofdm_ns + 1][j] * conjf(ofdm->pilots[j]); /* next pilot conjugate */
        }

        aphase_est_pilot_rect = aphase_est_pilot_rect + vector_sum(symbol, 3);

        /* use next step of pilots in past and future */

        for (j = (i - 1), k = 0; j < (i + 2); j++, k++) {
            symbol[k] = ofdm->rx_sym[0][j] * conjf(ofdm->pilots[j]); /* previous pilot */
        }

        aphase_est_pilot_rect = aphase_est_pilot_rect + vector_sum(symbol, 3);

        for (j = (i - 1), k = 0; j < (i + 2); j++, k++) {
            symbol[k] = ofdm->rx_sym[ofdm_ns + 2][j] * conjf(ofdm->pilots[j]); /* last pilot */
        }

        aphase_est_pilot_rect = aphase_est_pilot_rect + vector_sum(symbol, 3);
        aphase_est_pilot[i] = cargf(aphase_est_pilot_rect);

        /* amplitude is estimated over 12 pilots */

        aamp_est_pilot[i] = cabsf(aphase_est_pilot_rect / 12.0f);
    }

    /*
     * correct phase offset using phase estimate, and demodulate
     * bits, separate loop as it runs across cols (carriers) to get
     * frame bit ordering correct
     */

    complex float rx_corr;
    int abit[2];
    int bit_index = 0;
    float sum_amp = 0.0f;

    for (rr = 0; rr < ofdm_rowsperframe; rr++) {
        /*
         * Note the i starts with the second carrier, ends with Nc+1.
         * so we ignore the first and last carriers.
         *
         * Also note we are using sym[2..8] or the seven data symbols.
         */

        for (i = 1; i < (ofdm_nc + 1); i++) {
            if (ofdm->phase_est_en == true) {
                rx_corr = ofdm->rx_sym[rr + 2][i] * cexpf(-I * aphase_est_pilot[i]);
            } else {
                rx_corr = ofdm->rx_sym[rr + 2][i];
            }

            /*
             * Output complex data symbols after phase correction;
             * rx_np means the pilot symbols have been removed
             */

            ofdm->rx_np[(rr * ofdm_nc) + (i - 1)] = rx_corr;

            /*
             * Note even though amp ests are the same for each col,
             * the FEC decoder likes to have one amplitude per symbol
             * so convenient to log them all
             */

            ofdm->rx_amp[(rr * ofdm_nc) + (i - 1)] = aamp_est_pilot[i];
            sum_amp += aamp_est_pilot[i];

            /*
             * Note like amps in this implementation phase ests are the
             * same for each col, but we log them for each symbol anyway
             */

            ofdm->aphase_est_pilot_log[(rr * ofdm_nc) + (i - 1)] = aphase_est_pilot[i];

            if (ofdm_bps == 1) {
                rx_bits[bit_index++] = crealf(rx_corr) > 0.0f;
            } else if (ofdm_bps == 2) {
                /*
                 * Only one final task, decode what quadrant the phase
                 * is in, and return the dibits
                 */
                qpsk_demod(rx_corr, abit);
                rx_bits[bit_index++] = abit[1];
                rx_bits[bit_index++] = abit[0];
            }
        }
    }

    /* update mean amplitude estimate for LDPC decoder scaling */

    ofdm->mean_amp = 0.9f * ofdm->mean_amp + 0.1f * sum_amp / (ofdm_rowsperframe * ofdm_nc);

    /* Adjust nin to take care of sample clock offset */

    ofdm->nin = ofdm_samplesperframe;

    if (ofdm->timing_en == true) {
        ofdm->clock_offset_counter += prev_timing_est - ofdm->timing_est;

        int thresh = (ofdm_m + ofdm_ncp) / 8;
        int tshift = (ofdm_m + ofdm_ncp) / 4;

        if (ofdm->timing_est > thresh) {
            ofdm->nin = ofdm_samplesperframe + tshift;
            ofdm->timing_est -= tshift;
            ofdm->sample_point -= tshift;
        } else if (ofdm->timing_est < -thresh) {
            ofdm->nin = ofdm_samplesperframe - tshift;
            ofdm->timing_est += tshift;
            ofdm->sample_point += tshift;
        }
    }

    /* estimate signal and noise power, see ofdm_lib.m, cohpsk.m for
       more info */

    complex float *rx_np = ofdm->rx_np;

    float sig_var = 0.0f;

    for (i = 0; i < (ofdm_rowsperframe * ofdm_nc); i++) {
        sig_var += cnormf(rx_np[i]);
    }

    sig_var /= (ofdm_rowsperframe * ofdm_nc);
    float sig_rms = sqrtf(sig_var);

    float sum_x = 0.0f;
    float sum_xx = 0.0f;
    int n = 0;

    for (i = 0; i < ofdm_rowsperframe * ofdm_nc; i++) {
        complex float s = rx_np[i];

        if (fabsf(crealf(s)) > sig_rms) {
            sum_x += cimagf(s);
            sum_xx += cimagf(s) * cimagf(s);
            n++;
        }
    }

    /* with large interfering carriers this alg can break down - in
       that case set a benign value for noise_var that will produce a
       sensible (probably low) SNR est */

    float noise_var = 1.0f;

    if (n > 1) {
        noise_var = (n * sum_xx - sum_x * sum_x) / (n * (n - 1));
    }

    ofdm->noise_var = 2.0f * noise_var;
    ofdm->sig_var = sig_var;
}

/* iterate state machine ------------------------------------*/

void ofdm_sync_state_machine(struct OFDM *ofdm, int *rx_uw) {
    char next_state[ofdm_state_str];
    int i;

    strncpy(next_state, ofdm->sync_state, ofdm_state_str);
    ofdm->sync_start = ofdm->sync_end = 0;

    if (strcmp(ofdm->sync_state, "search") == 0) {
        if (ofdm->timing_valid) {
            ofdm->frame_count = 0;
            ofdm->sync_counter = 0;
            ofdm->sync_start = 1;
            ofdm->clock_offset_counter = 0;
            strncpy(next_state, "trial", ofdm_state_str);
        }
    }

    if (!strcmp(ofdm->sync_state, "synced") || !strcmp(ofdm->sync_state, "trial")) {
        ofdm->frame_count++;
        ofdm->frame_count_interleaver++;

        /* freq offset est may be too far out, and has aliases every 1/Ts, so
           we use a Unique Word to get a really solid indication of sync. */

        ofdm->uw_errors = 0;

        for (i = 0; i < ofdm_nuwbits; i++) {
            ofdm->uw_errors += ofdm->tx_uw[i] ^ rx_uw[i];
        }

        /* during trial sync we don't tolerate errors so much, we look
           for 3 consecutive frames with low error rate to confirm
           sync */

        if (!strcmp(ofdm->sync_state, "trial")) {
            if (ofdm->uw_errors > 2) {
                /* if we exceed thresh stay in trial sync */
                ofdm->sync_counter++;
                ofdm->frame_count = 0;
            }

            if (ofdm->sync_counter == 2) {
                /* if we get two bad frames drop sync and start again */
                strncpy(next_state, "search", ofdm_state_str);
                strncpy(ofdm->sync_state_interleaver, "search", ofdm_state_str);
            }

            if (ofdm->frame_count == 4) {
                /* three good frames, sync is OK! */
                strncpy(next_state, "synced", ofdm_state_str);
            }
        }

        /* once we have synced up we tolerate a higher error rate to wait out fades */

        if (!strcmp(ofdm->sync_state, "synced")) {
            if (ofdm->uw_errors > 2) {
                ofdm->sync_counter++;
            } else {
                ofdm->sync_counter = 0;
            }

            if ((ofdm->sync_mode == OFDM_SYNC_AUTO) && (ofdm->sync_counter == 12)) {
                /* run of consecutive bad frames ... drop sync */
                strncpy(next_state, "search", ofdm_state_str);
                strncpy(ofdm->sync_state_interleaver, "search", ofdm_state_str);
            }
        }
    }

    strncpy(ofdm->last_sync_state, ofdm->sync_state, ofdm_state_str);
    strncpy(ofdm->last_sync_state_interleaver, ofdm->sync_state_interleaver, ofdm_state_str);
    strncpy(ofdm->sync_state, next_state, ofdm_state_str);
}

/*---------------------------------------------------------------------------* \

  FUNCTIONS...: ofdm_set_sync
  AUTHOR......: David Rowe
  DATE CREATED: May 2018

  Operator control of sync state machine.  This mode is required to
  acquire sync up at very low SNRS.  This is difficult to implement,
  for example we may get a false sync, or the state machine may fall
  out of sync by mistake during a long fade.

  So with this API call we allow some operator assistance.

  Ensure this is called in the same thread as ofdm_sync_state_machine().

\*---------------------------------------------------------------------------*/

void ofdm_set_sync(struct OFDM *ofdm, int sync_cmd) {
    assert(ofdm != NULL);

    switch (sync_cmd) {
        case OFDM_SYNC_UNSYNC:
            /* force manual unsync, in case operator detects false sync,
               which will cause sync state machine to have another go at
               sync */
            strncpy(ofdm->sync_state, "search", ofdm_state_str);
            strncpy(ofdm->sync_state_interleaver, "search", ofdm_state_str);
            break;
        case OFDM_SYNC_AUTO:
            /* normal operating mode - sync state machine decides when to unsync */
            ofdm->sync_mode = OFDM_SYNC_AUTO;
            break;
        case OFDM_SYNC_MANUAL:
            /* allow sync state machine to sync, but not to unsync, the
               operator will decide that manually */
            ofdm->sync_mode = OFDM_SYNC_MANUAL;
            break;
        default:
            assert(0);
    }
}

/*---------------------------------------------------------------------------*\

  FUNCTION....: ofdm_get_demod_stats()
  AUTHOR......: David Rowe
  DATE CREATED: May 2018

  Fills stats structure with a bunch of demod information.

\*---------------------------------------------------------------------------*/

void ofdm_get_demod_stats(struct OFDM *ofdm, struct MODEM_STATS *stats) {
    int c, r;

    stats->Nc = ofdm_nc;
    assert(stats->Nc <= MODEM_STATS_NC_MAX);

    float snr_est = 10.0f * log10f((0.1f + (ofdm->sig_var / ofdm->noise_var)) * ofdm_nc * ofdm_rs / 3000.0f);
    float total = ofdm->frame_count * ofdm_samplesperframe;

    stats->snr_est = 0.9f * stats->snr_est + 0.1f * snr_est;
    stats->sync = !strcmp(ofdm->sync_state, "synced") || !strcmp(ofdm->sync_state, "trial");
    stats->foff = ofdm->foff_est_hz;
    stats->rx_timing = ofdm->timing_est;
    stats->clock_offset = 0;

    if (total != 0.0f) {
        stats->clock_offset = ofdm->clock_offset_counter / total;
    }

    stats->sync_metric = ofdm->timing_mx;

    assert(ofdm_rowsperframe < MODEM_STATS_NR_MAX);
    stats->nr = ofdm_rowsperframe;

    for (c = 0; c < ofdm_nc; c++) {
        for (r = 0; r < ofdm_rowsperframe; r++) {
            complex float rot = ofdm->rx_np[r * c] * cexpf(I * (M_PI / 4.0f));

            stats->rx_symbols[r][c].real = crealf(rot);
            stats->rx_symbols[r][c].imag = cimagf(rot);
        }
    }
}

/* Assemble modem frame of bits from UW, payload bits, and txt bits */

void ofdm_assemble_modem_frame(uint8_t modem_frame[],
        uint8_t payload_bits[],
        uint8_t txt_bits[]) {
    int b, t;

    int p = 0;
    int u = 0;

    for (b = 0; b < ofdm_bitsperframe - ofdm_ntxtbits; b++) {
        if ((u < ofdm_nuwbits) && (b == (uw_ind[u] - 1))) {
            modem_frame[b] = tx_uw[u++];
        } else {
            modem_frame[b] = payload_bits[p];
            p++;
        }
    }

    assert(u == ofdm_nuwbits);
    assert(p == (ofdm_bitsperframe - ofdm_nuwbits - ofdm_ntxtbits));

    for (t = 0; b < ofdm_bitsperframe; b++, t++) {
        modem_frame[b] = txt_bits[t];
    }

    assert(t == ofdm_ntxtbits);
}

/* Assemble modem frame from UW, payload symbols, and txt bits */

void ofdm_assemble_modem_frame_symbols(complex float modem_frame[],
        COMP payload_syms[],
        uint8_t txt_bits[]) {
    int Nsymsperframe = ofdm_bitsperframe / ofdm_bps;
    int Nuwsyms = ofdm_nuwbits / ofdm_bps;
    int Ntxtsyms = ofdm_ntxtbits / ofdm_bps;

    int s, t;

    int p = 0;
    int u = 0;

    for (s = 0; s < Nsymsperframe - Ntxtsyms; s++) {
        if ((u < Nuwsyms) && (s == (uw_ind_sym[u] - 1))) {
            modem_frame[s] = tx_uw_syms[u++];
        } else {
            modem_frame[s] = payload_syms[p].real + payload_syms[p].imag * I;
            p++;
        }
    }

    assert(u == Nuwsyms);
    assert(p == (Nsymsperframe - Nuwsyms - Ntxtsyms));

    int dibit[2];

    for (t = 0; s < Nsymsperframe; s++, t += ofdm_bps) {
        dibit[0] = txt_bits[t + 1] & 0x1;
        dibit[1] = txt_bits[t] & 0x1;
        modem_frame[s] = qpsk_mod(dibit);
    }

    assert(t == ofdm_ntxtbits);
}

void ofdm_disassemble_modem_frame(struct OFDM *ofdm,
        int rx_uw[],
        COMP codeword_syms[],
        float codeword_amps[],
        short txt_bits[]) {
    int Nsymsperframe = ofdm_bitsperframe / ofdm_bps;
    int Nuwsyms = ofdm_nuwbits / ofdm_bps;
    int Ntxtsyms = ofdm_ntxtbits / ofdm_bps;

    int s, t;

    int p = 0;
    int u = 0;

    int dibit[2];

    for (s = 0; s < Nsymsperframe - Ntxtsyms; s++) {
        if ((u < Nuwsyms) && (s == (uw_ind_sym[u] - 1))) {
            qpsk_demod(ofdm->rx_np[s], dibit);
            rx_uw[ofdm_bps * u    ] = dibit[1];
            rx_uw[ofdm_bps * u + 1] = dibit[0];
            u++;
        } else {
            codeword_syms[p].real = crealf(ofdm->rx_np[s]);
            codeword_syms[p].imag = cimagf(ofdm->rx_np[s]);
            codeword_amps[p] = ofdm->rx_amp[s];
            p++;
        }
    }

    assert(u == Nuwsyms);
    assert(p == (Nsymsperframe - Nuwsyms - Ntxtsyms));

    for (t = 0; s < Nsymsperframe; s++, t += ofdm_bps) {
        qpsk_demod(ofdm->rx_np[s], dibit);

        txt_bits[t    ] = dibit[1];
        txt_bits[t + 1] = dibit[0];
    }

    assert(t == ofdm_ntxtbits);
}


/*
 * Pseudo-random number generator that we can implement in C with
 * identical results to Octave.  Returns an unsigned int between 0
 * and 32767.  Used for generating test frames of various lengths.
 */

void ofdm_rand(uint16_t r[], int n) {
    uint64_t seed = 1;
    int i;

    for (i = 0; i < n; i++) {
        seed = (1103515245l * seed + 12345) % 32768;
        r[i] = seed;
    }
}


void ofdm_generate_payload_data_bits(int payload_data_bits[], int data_bits_per_frame) {
    int i;
    
    /* construct payload data bits */
                                
    uint16_t r[data_bits_per_frame];
    ofdm_rand(r, data_bits_per_frame);
    for(i=0; i<data_bits_per_frame; i++) {
        payload_data_bits[i] = r[i]>16384;
    }
}

void ofdm_print_info(struct OFDM *ofdm) {

    fprintf(stderr, "ofdm->foff_est_gain = %g\n", ofdm->foff_est_gain);
    fprintf(stderr, "ofdm->foff_est_hz = %g\n", ofdm->foff_est_hz);
    fprintf(stderr, "ofdm->timing_mx = %g\n", ofdm->timing_mx);
    fprintf(stderr, "ofdm->coarse_foff_est_hz = %g\n", ofdm->coarse_foff_est_hz);
    fprintf(stderr, "ofdm->timing_norm = %g\n", ofdm->timing_norm);
    fprintf(stderr, "ofdm->sig_var = %g\n", ofdm->sig_var);
    fprintf(stderr, "ofdm->noise_var = %g\n", ofdm->noise_var);
    fprintf(stderr, "ofdm->mean_amp = %g\n", ofdm->mean_amp);
    fprintf(stderr, "ofdm->clock_offset_counter = %d\n", ofdm->clock_offset_counter);
    fprintf(stderr, "ofdm->verbose = %d\n", ofdm->verbose);
    fprintf(stderr, "ofdm->sample_point = %d\n", ofdm->sample_point);
    fprintf(stderr, "ofdm->timing_est = %d\n", ofdm->timing_est);
    fprintf(stderr, "ofdm->timing_valid = %d\n", ofdm->timing_valid);
    fprintf(stderr, "ofdm->nin = %d\n", ofdm->nin);
    fprintf(stderr, "ofdm->uw_errors = %d\n", ofdm->uw_errors);
    fprintf(stderr, "ofdm->sync_counter = %d\n", ofdm->sync_counter);
    fprintf(stderr, "ofdm->frame_count = %d\n", ofdm->frame_count);
    fprintf(stderr, "ofdm->sync_start = %d\n", ofdm->sync_start);
    fprintf(stderr, "ofdm->sync_end = %d\n", ofdm->sync_end);
    fprintf(stderr, "ofdm->sync_mode = %d\n", ofdm->sync_mode);
    fprintf(stderr, "ofdm->frame_count_interleaver = %d\n", ofdm->frame_count_interleaver);
    fprintf(stderr, "ofdm->timing_en = %d\n", ofdm->timing_en);
    fprintf(stderr, "ofdm->foff_est_en = %d\n", ofdm->foff_est_en);
    fprintf(stderr, "ofdm->phase_est_en = %d\n", ofdm->phase_est_en);
    fprintf(stderr, "ofdm->tx_bpf_en = %d\n", ofdm->tx_bpf_en);
};
