/**
 * Concat — descriptor analysis kernels (clean-room, first-party MIT).
 * See analysis.h and references/dsp-notes.md. Offline (analyst thread) only.
 */
#include "analysis.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define A_EPS 1e-10

/* ── In-house iterative radix-2 FFT (in-place, incremental twiddles) ───────── */
/* sign = -1 forward, +1 inverse (unnormalised). n must be a power of two. */
static void fft_radix2(float *re, float *im, int n, int sign) {
    for (int i = 1, j = 0; i < n; i++) {           /* bit-reversal permutation */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = sign * 2.0 * M_PI / len;
        float wlr = (float)cos(ang), wli = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int aa = i + k, bb = i + k + len / 2;
                float ur = re[aa], ui = im[aa];
                float vr = re[bb] * wr - im[bb] * wi;
                float vi = re[bb] * wi + im[bb] * wr;
                re[aa] = ur + vr; im[aa] = ui + vi;
                re[bb] = ur - vr; im[bb] = ui - vi;
                float nwr = wr * wlr - wi * wli;    /* rotate twiddle */
                wi = wr * wli + wi * wlr; wr = nwr;
            }
        }
    }
}

static double hz2mel(double f) { return 1127.01048 * log(1.0 + f / 700.0); }
static double mel2hz(double m) { return 700.0 * (exp(m / 1127.01048) - 1.0); }

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */

analysis_t *analysis_create(int sr, int fft_size, int n_mel, int n_mfcc) {
    if (n_mel > 64) n_mel = 64;                    /* logmel stack bound */
    analysis_t *a = (analysis_t *)calloc(1, sizeof(analysis_t));
    if (!a) return NULL;
    a->sr = sr; a->fft_size = fft_size; a->n_bins = fft_size / 2 + 1;
    a->n_mel = n_mel; a->n_mfcc = n_mfcc;

    a->window  = (float *)malloc(sizeof(float) * fft_size);
    a->mel_fb  = (float *)malloc(sizeof(float) * n_mel * a->n_bins);
    a->dct     = (float *)malloc(sizeof(float) * n_mfcc * n_mel);
    a->re      = (float *)malloc(sizeof(float) * fft_size);
    a->im      = (float *)malloc(sizeof(float) * fft_size);
    a->mag_scr = (float *)malloc(sizeof(float) * a->n_bins);
    if (!a->window || !a->mel_fb || !a->dct || !a->re || !a->im || !a->mag_scr) {
        analysis_destroy(a); return NULL;
    }

    for (int i = 0; i < fft_size; i++)             /* Hann */
        a->window[i] = 0.5f * (1.0f - (float)cos(2.0 * M_PI * i / fft_size));   /* periodic (DFT-even) Hann */

    double lo = 20.0, hi = sr / 2.0;               /* triangular mel filterbank */
    double mlo = hz2mel(lo), mhi = hz2mel(hi);
    for (int m = 0; m < n_mel; m++) {
        double fl = mel2hz(mlo + (mhi - mlo) * (m + 0) / (n_mel + 1));
        double fc = mel2hz(mlo + (mhi - mlo) * (m + 1) / (n_mel + 1));
        double fr = mel2hz(mlo + (mhi - mlo) * (m + 2) / (n_mel + 1));
        for (int k = 0; k < a->n_bins; k++) {
            double f = (double)k * sr / fft_size, w = 0.0;
            if (f >= fl && f <= fc && fc > fl)      w = (f - fl) / (fc - fl);
            else if (f > fc && f <= fr && fr > fc)  w = (fr - f) / (fr - fc);
            a->mel_fb[m * a->n_bins + k] = (float)w;
        }
    }

    for (int i = 0; i < n_mfcc; i++) {             /* orthonormal DCT-II */
        double scale = (i == 0) ? sqrt(1.0 / n_mel) : sqrt(2.0 / n_mel);
        for (int j = 0; j < n_mel; j++)
            a->dct[i * n_mel + j] = (float)(scale * cos(M_PI * i * (j + 0.5) / n_mel));
    }
    return a;
}

void analysis_destroy(analysis_t *a) {
    if (!a) return;
    free(a->window); free(a->mel_fb); free(a->dct);
    free(a->re); free(a->im); free(a->mag_scr);
    free(a);
}

/* ── Descriptors ────────────────────────────────────────────────────────────── */

void analysis_magnitude(analysis_t *a, const float *frame, float *mag) {
    for (int i = 0; i < a->fft_size; i++) { a->re[i] = frame[i] * a->window[i]; a->im[i] = 0.0f; }
    fft_radix2(a->re, a->im, a->fft_size, -1);
    for (int k = 0; k < a->n_bins; k++)
        mag[k] = sqrtf(a->re[k] * a->re[k] + a->im[k] * a->im[k]);
}

void analysis_mfcc(analysis_t *a, const float *mag, float *mfcc) {
    float logmel[64];
    for (int m = 0; m < a->n_mel; m++) {
        const float *fb = &a->mel_fb[m * a->n_bins];
        double acc = 0.0;
        for (int k = 0; k < a->n_bins; k++) { double mg = mag[k]; acc += (double)fb[k] * mg * mg; }
        logmel[m] = (float)log(acc > A_EPS ? acc : A_EPS);
    }
    for (int i = 0; i < a->n_mfcc; i++) {
        const float *dr = &a->dct[i * a->n_mel];
        double acc = 0.0;
        for (int j = 0; j < a->n_mel; j++) acc += (double)dr[j] * logmel[j];
        mfcc[i] = (float)acc;
    }
}

float analysis_centroid(analysis_t *a, const float *mag) {
    double num = 0.0, den = 0.0;
    for (int k = 1; k < a->n_bins; k++) {
        double f = (double)k * a->sr / a->fft_size;
        num += f * mag[k]; den += mag[k];
    }
    return den > A_EPS ? (float)(num / den) : 0.0f;
}

float analysis_flatness(analysis_t *a, const float *mag) {
    double logsum = 0.0, arith = 0.0; int cnt = a->n_bins - 1;
    for (int k = 1; k < a->n_bins; k++) {
        double m = mag[k] > A_EPS ? mag[k] : A_EPS;
        logsum += log(m); arith += m;
    }
    if (cnt <= 0 || arith <= A_EPS) return 0.0f;
    double geo = exp(logsum / cnt), am = arith / cnt;
    double r = geo / am;
    return (float)(r < 0.0 ? 0.0 : (r > 1.0 ? 1.0 : r));
}

float analysis_flux(analysis_t *a, const float *mag, const float *prev_mag) {
    double s = 0.0;
    for (int k = 0; k < a->n_bins; k++) { float d = mag[k] - prev_mag[k]; if (d > 0.0f) s += d; }
    return (float)(s / a->n_bins);
}

float analysis_zcr(const float *frame, int n) {
    int c = 0;
    for (int i = 1; i < n; i++) if ((frame[i] >= 0.0f) != (frame[i - 1] >= 0.0f)) c++;
    return n > 1 ? (float)c / (n - 1) : 0.0f;
}

float analysis_rms(const float *frame, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)frame[i] * frame[i];
    return n > 0 ? (float)sqrt(s / n) : 0.0f;
}

/* Time-domain YIN (de Cheveigné & Kawahara). frame length = fft_size, W = fft_size/2. */
float analysis_yin(analysis_t *a, const float *frame, float min_hz, float max_hz, float *confidence) {
    int W = a->fft_size / 2;
    int tau_min = (int)(a->sr / max_hz); if (tau_min < 2) tau_min = 2;
    int tau_max = (int)(a->sr / min_hz); if (tau_max > W - 1) tau_max = W - 1;
    if (tau_max <= tau_min) { if (confidence) *confidence = 0.0f; return 0.0f; }

    float *d = a->re;                              /* scratch: cumulative-mean-normalised diff */
    double running = 0.0;
    d[0] = 1.0f;
    for (int tau = 1; tau <= tau_max; tau++) {
        double sum = 0.0;
        for (int j = 0; j < W; j++) { double diff = frame[j] - frame[j + tau]; sum += diff * diff; }
        running += sum;
        d[tau] = running > 0.0 ? (float)(sum * tau / running) : 1.0f;
    }

    const float THRESH = 0.1f;
    int chosen = -1;
    for (int tau = tau_min; tau <= tau_max; tau++) {
        if (d[tau] < THRESH) {
            while (tau + 1 <= tau_max && d[tau + 1] < d[tau]) tau++;
            chosen = tau; break;
        }
    }
    if (chosen < 0) {                              /* fall back to global minimum */
        float mn = 1e30f;
        for (int tau = tau_min; tau <= tau_max; tau++) if (d[tau] < mn) { mn = d[tau]; chosen = tau; }
    }

    float better = (float)chosen;                  /* parabolic interpolation */
    if (chosen > tau_min && chosen < tau_max) {
        float s0 = d[chosen - 1], s1 = d[chosen], s2 = d[chosen + 1];
        float denom = s0 - 2.0f * s1 + s2;
        if (fabsf(denom) > 1e-12f) better = chosen + 0.5f * (s0 - s2) / denom;
    }

    float aper = d[chosen];
    if (confidence) *confidence = 1.0f - (aper < 0.0f ? 0.0f : (aper > 1.0f ? 1.0f : aper));
    if (aper > 0.6f) return 0.0f;                  /* too aperiodic → unvoiced */
    return (float)a->sr / better;
}

/* ── Welford running stats ──────────────────────────────────────────────────── */

void rstat_init(rstat_t *s) { s->n = 0; s->mean = 0; s->m2 = 0; s->mn = 1e30f; s->mx = -1e30f; }
void rstat_add(rstat_t *s, float x) {
    s->n += 1.0; double d = x - s->mean; s->mean += d / s->n; s->m2 += d * (x - s->mean);
    if (x < s->mn) s->mn = x;
    if (x > s->mx) s->mx = x;
}
float rstat_mean(const rstat_t *s) { return (float)s->mean; }
float rstat_std(const rstat_t *s) { return s->n > 1.0 ? (float)sqrt(s->m2 / s->n) : 0.0f; }
