/**
 * Concat — corpus descriptor layer (clean-room, first-party MIT). See corpus.h.
 * Offline (analyst thread) only; malloc is allowed here, never in render.
 */
#include "corpus.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

__thread volatile int *concat_cancel_flag = NULL;

static int cmpf(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}
static float percentile_sorted(const float *v, int n, float p) {
    if (n <= 0) return 0.0f;
    if (n == 1) return v[0];
    float idx = p * (n - 1);
    int lo = (int)idx; float fr = idx - lo;
    return (lo + 1 < n) ? v[lo] + fr * (v[lo + 1] - v[lo]) : v[lo];
}

/* ── Per-slice descriptor (MultiStats mean+std over frames) ─────────────────── */

void concat_slice_descriptor(analysis_t *a, const float *slice, int len, float *out) {
    int N = a->fft_size, hop = N / 2;
    float *frame = (float *)malloc(sizeof(float) * N);
    float *mag   = (float *)malloc(sizeof(float) * a->n_bins);
    float  mfcc[64];
    int    cap   = len / hop + 2;
    float *pitches = (float *)malloc(sizeof(float) * (cap > 0 ? cap : 1));
    memset(out, 0, sizeof(float) * NDESC);
    if (!frame || !mag || !pitches) { free(frame); free(mag); free(pitches); return; }

    rstat_t s_mfcc[13], s_cent, s_flat, s_zcr, s_loud, s_conf;
    int nmfcc = a->n_mfcc; if (nmfcc > 13) nmfcc = 13;   /* s_mfcc[] + NDESC layout fix 13 MFCCs */
    for (int i = 0; i < nmfcc; i++) rstat_init(&s_mfcc[i]);
    rstat_init(&s_cent); rstat_init(&s_flat); rstat_init(&s_zcr);
    rstat_init(&s_loud); rstat_init(&s_conf);
    float peak = 0.0f; int npitch = 0, nframes = 0;
    int yin_step = (len / hop) / 8; if (yin_step < 1) yin_step = 1;   /* sample YIN ~8 frames/slice */

    for (int p = 0; p < len; p += hop) {
        int copy = len - p; if (copy > N) copy = N;
        if (copy < hop && nframes > 0) break;            /* skip tiny tail */
        for (int i = 0; i < copy; i++) frame[i] = slice[p + i];
        for (int i = copy; i < N; i++) frame[i] = 0.0f;  /* zero-pad */

        analysis_magnitude(a, frame, mag);
        analysis_mfcc(a, mag, mfcc);
        for (int i = 0; i < nmfcc; i++) rstat_add(&s_mfcc[i], mfcc[i]);
        rstat_add(&s_cent, analysis_centroid(a, mag));
        rstat_add(&s_flat, analysis_flatness(a, mag));
        rstat_add(&s_zcr,  analysis_zcr(&slice[p], copy));
        rstat_add(&s_loud, analysis_rms(&slice[p], copy));
        if (nframes % yin_step == 0) {           /* YIN is the costliest kernel — sample, don't run every frame */
            float conf; float pit = analysis_yin(a, frame, 50.0f, 2000.0f, &conf);
            rstat_add(&s_conf, conf);
            if (pit > 0.0f) pitches[npitch++] = pit;
        }
        for (int i = 0; i < copy; i++) { float v = fabsf(slice[p + i]); if (v > peak) peak = v; }
        nframes++;
        if (copy < N) break;                             /* last (partial) frame */
    }

    for (int i = 0; i < nmfcc; i++) {
        out[D_MFCC0_MEAN + i] = rstat_mean(&s_mfcc[i]);
        out[D_MFCC0_STD  + i] = rstat_std(&s_mfcc[i]);
    }
    out[D_CENTROID_MEAN] = rstat_mean(&s_cent); out[D_CENTROID_STD] = rstat_std(&s_cent);
    out[D_FLATNESS_MEAN] = rstat_mean(&s_flat); out[D_FLATNESS_STD] = rstat_std(&s_flat);
    out[D_ZCR_MEAN]      = rstat_mean(&s_zcr);  out[D_ZCR_STD]      = rstat_std(&s_zcr);
    out[D_LOUD_MEAN]     = rstat_mean(&s_loud); out[D_LOUD_PEAK]    = peak;
    if (npitch > 0) {
        qsort(pitches, npitch, sizeof(float), cmpf);
        out[D_PITCH] = percentile_sorted(pitches, npitch, 0.5f);   /* median voiced */
    }
    out[D_PITCH_CONF] = rstat_mean(&s_conf);
    free(frame); free(mag); free(pitches);
}

/* ── Onset slicing (spectral flux → median-subtract → peak-pick) ────────────── */

int concat_onset_slice(analysis_t *a, const float *buf, int len,
                       int min_len, int max_len, float sil_gate,
                       slice_range_t *out, int max_slices) {
    int N = a->fft_size, hop = N / 2;
    if (len < N) {
        if (max_slices > 0 && analysis_rms(buf, len) >= sil_gate) { out[0].start = 0; out[0].len = len; return 1; }
        return 0;
    }
    int nf = (len - N) / hop + 1;
    float *flux  = (float *)calloc(nf, sizeof(float));
    float *df    = (float *)calloc(nf, sizeof(float));
    float *mag   = (float *)malloc(sizeof(float) * a->n_bins);
    float *prev  = (float *)calloc(a->n_bins, sizeof(float));
    float *frame = (float *)malloc(sizeof(float) * N);
    int   *onset = (int *)malloc(sizeof(int) * (nf + 2));
    if (!flux || !df || !mag || !prev || !frame || !onset) {
        free(flux); free(df); free(mag); free(prev); free(frame); free(onset); return 0;
    }

    for (int i = 0; i < nf; i++) {
        if ((i & 255) == 0 && concat_cancelled()) {          /* worker asked to stop */
            free(flux); free(df); free(mag); free(prev); free(frame); free(onset); return 0;
        }
        int p = i * hop;
        for (int j = 0; j < N; j++) frame[j] = buf[p + j];
        analysis_magnitude(a, frame, mag);
        flux[i] = i ? analysis_flux(a, mag, prev) : 0.0f;
        memcpy(prev, mag, sizeof(float) * a->n_bins);
    }
    const int mw = 3;                                    /* median filter half-window */
    for (int i = 0; i < nf; i++) {
        float win[7]; int c = 0;
        for (int k = -mw; k <= mw; k++) { int idx = i + k; if (idx >= 0 && idx < nf) win[c++] = flux[idx]; }
        qsort(win, c, sizeof(float), cmpf);
        float med = percentile_sorted(win, c, 0.5f);
        float v = flux[i] - med; df[i] = v > 0.0f ? v : 0.0f;
    }
    /* Adaptive threshold: local mean+std over a moving window (prefix sums), so a
     * file whose loudness varies isn't over-segmented in loud passages nor missed
     * in quiet ones — unlike a single global mean+std. */
    double *ps = (double *)calloc(nf + 1, sizeof(double));
    double *pss = (double *)calloc(nf + 1, sizeof(double));
    if (!ps || !pss) { free(ps); free(pss); free(flux); free(df); free(mag); free(prev); free(frame); free(onset); return 0; }
    for (int i = 0; i < nf; i++) { ps[i + 1] = ps[i] + df[i]; pss[i + 1] = pss[i] + (double)df[i] * df[i]; }
    const int W = 32;                                   /* ±~0.37 s window */

    int debounce = min_len / hop; if (debounce < 1) debounce = 1;
    int no = 0, last = -debounce - 1;
    onset[no++] = 0;                                     /* segment always starts at 0 */
    for (int i = 1; i < nf - 1; i++) {
        int lo = i - W < 0 ? 0 : i - W, hi = i + W >= nf ? nf - 1 : i + W, cnt = hi - lo + 1;
        double s = ps[hi + 1] - ps[lo], ss = pss[hi + 1] - pss[lo];
        double mean = s / cnt, v = ss / cnt - mean * mean; if (v < 0.0) v = 0.0;
        float thr = (float)(mean + sqrt(v)); if (thr < 1e-6f) thr = 1e-6f;
        if (df[i] > thr && df[i] >= df[i - 1] && df[i] > df[i + 1] && (i - last) >= debounce) {
            onset[no++] = i * hop; last = i;
        }
    }
    free(ps); free(pss);

    int ns = 0;
    for (int k = 0; k < no && ns < max_slices; k++) {
        int s = onset[k], e = (k + 1 < no) ? onset[k + 1] : len;
        for (int pos = s; pos < e && ns < max_slices; ) {
            int l = e - pos; if (l > max_len) l = max_len;
            if (l < min_len && (e - s) >= min_len) break; /* drop small remainder of a split */
            if (l <= 0) break;
            if (analysis_rms(&buf[pos], l) >= sil_gate) { out[ns].start = pos; out[ns].len = l; ns++; }
            pos += l;
        }
    }
    free(flux); free(df); free(mag); free(prev); free(frame); free(onset);
    return ns;
}

int concat_fixed_slice(analysis_t *a, const float *buf, int len,
                       int grain_len, float sil_gate,
                       slice_range_t *out, int max_slices) {
    (void)a;
    if (grain_len < 1) return 0;
    int hop = grain_len / 2; if (hop < 1) hop = 1;
    int ns = 0;
    for (int pos = 0; pos + grain_len <= len && ns < max_slices; pos += hop)
        if (analysis_rms(&buf[pos], grain_len) >= sil_gate) { out[ns].start = pos; out[ns].len = grain_len; ns++; }
    return ns;
}

/* ── Normalisation ──────────────────────────────────────────────────────────── */

void concat_robust_fit(const float *desc, int nslices, float *median, float *iqr) {
    float *col = (float *)malloc(sizeof(float) * (nslices > 0 ? nslices : 1));
    for (int d = 0; d < NDESC; d++) {
        for (int i = 0; i < nslices; i++) col[i] = desc[i * NDESC + d];
        qsort(col, nslices, sizeof(float), cmpf);
        float q1 = percentile_sorted(col, nslices, 0.25f);
        float q3 = percentile_sorted(col, nslices, 0.75f);
        median[d] = percentile_sorted(col, nslices, 0.5f);
        float r = q3 - q1; iqr[d] = r > 1e-6f ? r : 1e-6f;
    }
    free(col);
}

void concat_robust_apply(const float *vec, const float *median, const float *iqr, float *out) {
    for (int d = 0; d < NDESC; d++) out[d] = (vec[d] - median[d]) / iqr[d];
}

void concat_minmax_fit(const float *desc, int nslices, float *mn, float *mx) {
    for (int d = 0; d < NDESC; d++) { mn[d] = 1e30f; mx[d] = -1e30f; }
    for (int i = 0; i < nslices; i++)
        for (int d = 0; d < NDESC; d++) {
            float v = desc[i * NDESC + d];
            if (v < mn[d]) mn[d] = v;
            if (v > mx[d]) mx[d] = v;
        }
}
