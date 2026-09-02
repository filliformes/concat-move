/**
 * Concat — grain engine (clean-room MIT). See grain.h. Render-safe.
 */
#include "grain.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void build_env_tables(void);   /* fwd — defined after grain_env_math */

void grain_engine_init(grain_engine_t *ge) {
    memset(ge, 0, sizeof(*ge));
    ge->rng = 0x2545F491u;
    build_env_tables();
}

static float g_rand(grain_engine_t *ge) {        /* uniform 0..1 */
    unsigned x = ge->rng ? ge->rng : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    ge->rng = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}

static float grain_env_math(int env, int age, int glen) {
    const float PI = (float)M_PI;
    float x = glen > 0 ? (float)age / glen : 0.0f;     /* 0..1 */
    if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
    switch (env) {
        case 1: {                                       /* Tukey (25% cosine tapers) */
            const float a = 0.25f;
            if (x < a)        return 0.5f * (1.0f - cosf(PI * x / a));
            if (x > 1.0f - a) return 0.5f * (1.0f - cosf(PI * (1.0f - x) / a));
            return 1.0f;
        }
        case 2: return x < 0.5f ? 2.0f * x : 2.0f * (1.0f - x);      /* Triangular */
        case 3: { float d = x - 0.5f; return expf(-18.0f * d * d); } /* Gaussian */
        case 4: {                                       /* Expodec: fast attack, exp decay */
            float atk = x < 0.02f ? x / 0.02f : 1.0f;
            return atk * expf(-5.0f * x);
        }
        case 5: {                                       /* Reverse Expodec: exp swell, fast release */
            float y = 1.0f - x;
            float atk = y < 0.02f ? y / 0.02f : 1.0f;
            return atk * expf(-5.0f * y);
        }
        case 6: {                                       /* Trapezoid (linear tapers, flat top) */
            const float r = 0.1f;
            if (x < r)        return x / r;
            if (x > 1.0f - r) return (1.0f - x) / r;
            return 1.0f;
        }
        case 7: return 0.42f - 0.5f * cosf(2.0f * PI * x) + 0.08f * cosf(4.0f * PI * x);  /* Blackman */
        default: return 0.5f * (1.0f - cosf(2.0f * PI * x));         /* Hann */
    }
}

/* Precomputed envelope tables — avoids per-sample transcendentals with 128 voices. */
static float g_env_tables[8][GRAIN_ENV_TABLE];
static int   g_env_built = 0;
static void build_env_tables(void) {
    if (g_env_built) return;
    for (int e = 0; e < 8; e++)
        for (int i = 0; i < GRAIN_ENV_TABLE; i++)
            g_env_tables[e][i] = grain_env_math(e, i, GRAIN_ENV_TABLE - 1);
    g_env_built = 1;
}
static inline float grain_env_val(int env, int age, int glen) {
    if (env < 0 || env > 7) env = 0;
    float x = glen > 0 ? (float)age / glen : 0.0f;
    if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
    return g_env_tables[env][(int)(x * (GRAIN_ENV_TABLE - 1))];
}

static void spawn_grain(grain_engine_t *ge, corpus_index_t *ix,
                        const float *target, const float *weight,
                        const grain_params_t *gp, float gain, float target_hz, int sr) {
    int sel = query_select(&ix->query, target, weight, gp->k_nearest, gp->radius);
    if (sel < 0 || sel >= ix->nslices) return;

    /* Pick a free voice within the polyphony cap, else steal the oldest. */
    int nv = gp->max_voices; if (nv < 1) nv = 1; if (nv > GRAIN_VOICES) nv = GRAIN_VOICES;
    int vi = -1, oldest = -1, oldest_age = -1;
    for (int v = 0; v < nv; v++) {
        if (!ge->voices[v].active) { vi = v; break; }
        if (ge->voices[v].age > oldest_age) { oldest_age = ge->voices[v].age; oldest = v; }
    }
    if (vi < 0) vi = oldest;
    grain_t *g = &ge->voices[vi];

    const cslice_t *sl = &ix->slices[sel];
    g->audio = &ix->pool[sl->pool_off];
    g->len   = sl->len;

    float gms = gp->grain_size_ms;                        /* random size spread per grain */
    if (gp->grain_rand > 0.0f) gms *= 1.0f + (g_rand(ge) * 2.0f - 1.0f) * gp->grain_rand;
    int glen = (int)(gms * 0.001f * sr);                 /* may exceed the slice — slice loops under the window */
    if (glen < 1) glen = 1;
    g->grain_len = glen;
    g->age  = 0;
    g->gain = gain;

    /* A grain that outlasts its slice loops it; crossfade the loop seam to kill the click. */
    if (glen > g->len) {
        int xf = (int)(0.020f * sr);              /* 20 ms seam crossfade */
        if (xf > g->len / 2) xf = g->len / 2;     /* short slice → cap to half its length */
        if (xf < 1) xf = 1;
        g->xfade = xf;
    } else {
        g->xfade = 0;                             /* fits the slice → straight read, window handles edges */
    }
    int period = g->len - g->xfade;

    float jit = 1.0f + (g_rand(ge) * 2.0f - 1.0f) * gp->pitch_jitter * 0.5f;
    g->rate = powf(2.0f, gp->pitch_semi / 12.0f) * jit;
    if (target_hz > 0.0f && sl->pitch > 0.0f) g->rate *= target_hz / sl->pitch;   /* Pitch mode: retune to the note */
    if (g->rate < 0.01f) g->rate = 0.01f;

    g->reverse = (g_rand(ge) < gp->reverse_prob) ? 1 : 0;
    g->pos = g->reverse ? (double)(period - 1) : 0.0;

    float pan = (g_rand(ge) * 2.0f - 1.0f) * gp->pan_spread;   /* -spread..spread */
    g->panl = 0.5f * (1.0f - pan);
    g->panr = 0.5f * (1.0f + pan);
    g->active = 1;
}

static inline int16_t clamp16(float x) {
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;
    return (int16_t)(x * 32767.0f);
}

void grain_schedule(grain_engine_t *ge, corpus_index_t *ix,
                    const float *target, const float *weight, const grain_params_t *gp,
                    float gain, float target_hz, double *accum, int frames, int sr) {
    if (!ix || ix->nslices == 0) return;
    *accum += (double)gp->density * frames / sr;
    while (*accum >= 1.0) { spawn_grain(ge, ix, target, weight, gp, gain, target_hz, sr); *accum -= 1.0; }
}

void grain_mix(grain_engine_t *ge, const grain_params_t *gp, int16_t *out_lr, int frames, int sr) {
    (void)sr;
    for (int i = 0; i < frames; i++) {
        float l = 0.0f, r = 0.0f;
        for (int v = 0; v < GRAIN_VOICES; v++) {
            grain_t *g = &ge->voices[v];
            if (!g->active) continue;
            int len = g->len, xf = g->xfade, period = len - xf;   /* loop period (== len when xf==0) */
            double p = g->pos;

            int ip = (int)p; if (ip >= len) ip -= len;
            int ip1 = ip + 1; if (ip1 >= len) ip1 -= len;
            float frac = (float)(p - floor(p));
            float samp = g->audio[ip] * (1.0f / 32768.0f)
                       + frac * (g->audio[ip1] - g->audio[ip]) * (1.0f / 32768.0f);

            if (xf > 0 && p < xf) {           /* seam region: blend head with the slice tail (equal-length) */
                double tp = p + period;       /* tail position in [period, len) */
                int it = (int)tp; if (it >= len) it -= len;
                int it1 = it + 1; if (it1 >= len) it1 -= len;
                float tfrac = (float)(tp - floor(tp));
                float tail = g->audio[it] * (1.0f / 32768.0f)
                           + tfrac * (g->audio[it1] - g->audio[it]) * (1.0f / 32768.0f);
                float wgt = (float)(p / xf);  /* 0 → tail, 1 → head */
                samp = tail * (1.0f - wgt) + samp * wgt;
            }

            float env = grain_env_val(gp->grain_env, g->age, g->grain_len);
            /* Guaranteed edge declick: force a true-zero fade in/out for every window
             * (Gaussian/Expodec/etc. don't reach 0 at their ends → they'd click otherwise). */
            const int decl = 32;   /* ~0.7 ms */
            if (g->age < decl)                        env *= (float)g->age / decl;
            else if (g->age >= g->grain_len - decl)   env *= (float)(g->grain_len - g->age) / decl;
            samp *= env * g->gain;
            l += samp * g->panl;
            r += samp * g->panr;
            g->pos += g->reverse ? -g->rate : g->rate;
            if (g->pos >= period) g->pos -= period;
            if (g->pos < 0)       g->pos += period;
            g->age++;
            if (g->age >= g->grain_len) g->active = 0;
        }
        float mid = 0.5f * (l + r), side = 0.5f * (l - r) * gp->width;
        out_lr[i * 2]     = clamp16((mid + side) * gp->volume);
        out_lr[i * 2 + 1] = clamp16((mid - side) * gp->volume);
    }
}
