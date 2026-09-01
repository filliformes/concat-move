/**
 * Concat — descriptor analysis kernels (clean-room, first-party MIT).
 *
 * Per-frame audio descriptors for corpus analysis, reimplemented from the
 * algorithm designs studied in FluCoMa (BSD-3, not copied) — see
 * references/dsp-notes.md. Pure float C, no external deps (in-house radix-2 FFT).
 *
 * All functions are OFFLINE-thread only (analyst); none allocate or run in render.
 * Tables (window, mel filterbank, DCT) are precomputed once in analysis_create().
 */
#ifndef CONCAT_ANALYSIS_H
#define CONCAT_ANALYSIS_H

typedef struct {
    int sr;
    int fft_size;      /* e.g. 1024 (power of two) */
    int n_bins;        /* fft_size/2 + 1 */
    int n_mel;         /* mel bands, e.g. 40 */
    int n_mfcc;        /* MFCC coeffs kept, e.g. 13 */

    float *window;     /* Hann[fft_size] */
    float *mel_fb;     /* [n_mel * n_bins] triangular filterbank */
    float *dct;        /* [n_mfcc * n_mel] orthonormal DCT-II matrix */

    /* scratch (offline thread, single-threaded use) */
    float *re, *im;    /* [fft_size] */
    float *mag_scr;    /* [n_bins] */
} analysis_t;

analysis_t *analysis_create(int sr, int fft_size, int n_mel, int n_mfcc);
void        analysis_destroy(analysis_t *a);

/* Windowed magnitude spectrum: frame[fft_size] (raw) -> mag[n_bins]. */
void  analysis_magnitude(analysis_t *a, const float *frame, float *mag);

/* MFCC from a magnitude spectrum -> mfcc[n_mfcc] (coeff 0 = log-energy-ish). */
void  analysis_mfcc(analysis_t *a, const float *mag, float *mfcc);

/* Spectral centroid (Hz) from magnitude. */
float analysis_centroid(analysis_t *a, const float *mag);

/* Spectral flatness (linear ratio 0..1; ~1 = noisy/flat, ->0 = tonal). */
float analysis_flatness(analysis_t *a, const float *mag);

/* Spectral flux: half-wave-rectified magnitude increase, mean over bins. */
float analysis_flux(analysis_t *a, const float *mag, const float *prev_mag);

/* Zero-crossing rate (crossings / sample) of a time-domain frame. */
float analysis_zcr(const float *frame, int n);

/* RMS of a time-domain frame (linear). */
float analysis_rms(const float *frame, int n);

/* Time-domain YIN pitch (Hz) over frame[fft_size]; returns 0 if unvoiced.
 * *confidence in [0,1] (1 - aperiodicity at the chosen lag). */
float analysis_yin(analysis_t *a, const float *frame, float min_hz, float max_hz, float *confidence);

/* ── Welford running stats (per-slice MultiStats: mean + std) ─────────────── */
typedef struct { double n, mean, m2; float mn, mx; } rstat_t;
void  rstat_init(rstat_t *s);
void  rstat_add(rstat_t *s, float x);
float rstat_mean(const rstat_t *s);
float rstat_std(const rstat_t *s);   /* population std (0 if <2 samples) */

#endif /* CONCAT_ANALYSIS_H */
