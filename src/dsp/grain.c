/**
 * Concat — grain engine (clean-room MIT). See grain.h. Render-safe.
 */
#include "grain.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void grain_engine_init(grain_engine_t *ge) {
    memset(ge, 0, sizeof(*ge));
    ge->rng = 0x2545F491u;
}

static float g_rand(grain_engine_t *ge) {        /* uniform 0..1 */
    unsigned x = ge->rng ? ge->rng : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    ge->rng = x;
    return (x >> 8) * (1.0f / 16777216.0f);
}

static float grain_env_val(int env, int age, int glen) {
    float x = glen > 0 ? (float)age / glen : 0.0f;     /* 0..1 */
    if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
    if (env == 2) return x < 0.5f ? 2.0f * x : 2.0f * (1.0f - x);   /* triangular */
    if (env == 1) {                                    /* Tukey (25% tapers) */
        const float a = 0.25f;
        if (x < a)       return 0.5f * (1.0f - cosf((float)M_PI * x / a));
        if (x > 1.0f - a) return 0.5f * (1.0f - cosf((float)M_PI * (1.0f - x) / a));
        return 1.0f;
    }
    return 0.5f * (1.0f - cosf(2.0f * (float)M_PI * x));            /* Hann */
}

static void spawn_grain(grain_engine_t *ge, corpus_index_t *ix,
                        const float *target, const float *weight,
                        const grain_params_t *gp, int sr) {
    int sel = query_select(&ix->query, target, weight, gp->k_nearest, gp->radius);
    if (sel < 0 || sel >= ix->nslices) return;

    /* Pick a free voice, else steal the oldest. */
    int vi = -1, oldest = -1, oldest_age = -1;
    for (int v = 0; v < GRAIN_VOICES; v++) {
        if (!ge->voices[v].active) { vi = v; break; }
        if (ge->voices[v].age > oldest_age) { oldest_age = ge->voices[v].age; oldest = v; }
    }
    if (vi < 0) vi = oldest;
    grain_t *g = &ge->voices[vi];

    const cslice_t *sl = &ix->slices[sel];
    g->audio = &ix->pool[sl->pool_off];
    g->len   = sl->len;

    int glen = (int)(gp->grain_size_ms * 0.001f * sr);   /* grain-lock length at spawn */
    if (glen > g->len) glen = g->len;
    if (glen < 1) glen = 1;
    g->grain_len = glen;
    g->age = 0;

    float jit = 1.0f + (g_rand(ge) * 2.0f - 1.0f) * gp->pitch_jitter * 0.5f;
    g->rate = powf(2.0f, gp->pitch_semi / 12.0f) * jit;
    if (g->rate < 0.01f) g->rate = 0.01f;

    g->reverse = (g_rand(ge) < gp->reverse_prob) ? 1 : 0;
    g->pos = g->reverse ? (double)(g->len - 1) : 0.0;

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

void grain_render(grain_engine_t *ge, corpus_index_t *ix,
                  const float *target, const float *weight,
                  const grain_params_t *gp, int16_t *out_lr, int frames, int sr) {
    if (!ix || ix->nslices == 0) { memset(out_lr, 0, sizeof(int16_t) * 2 * frames); return; }

    double per_sample = (double)gp->density / sr;   /* grains scheduled per output sample */

    for (int i = 0; i < frames; i++) {
        ge->sched_accum += per_sample;
        while (ge->sched_accum >= 1.0) { spawn_grain(ge, ix, target, weight, gp, sr); ge->sched_accum -= 1.0; }

        float l = 0.0f, r = 0.0f;
        for (int v = 0; v < GRAIN_VOICES; v++) {
            grain_t *g = &ge->voices[v];
            if (!g->active) continue;
            int ip = (int)g->pos;
            if (ip < 0 || ip >= g->len) { g->active = 0; continue; }
            float s0 = g->audio[ip] * (1.0f / 32768.0f);
            float s1 = (ip + 1 < g->len) ? g->audio[ip + 1] * (1.0f / 32768.0f) : s0;
            float samp = s0 + (float)(g->pos - ip) * (s1 - s0);
            samp *= grain_env_val(gp->grain_env, g->age, g->grain_len);
            l += samp * g->panl;
            r += samp * g->panr;
            g->pos += g->reverse ? -g->rate : g->rate;
            g->age++;
            if (g->age >= g->grain_len) g->active = 0;
        }

        /* stereo width (mid/side) then volume */
        float mid = 0.5f * (l + r), side = 0.5f * (l - r) * gp->width;
        l = (mid + side) * gp->volume;
        r = (mid - side) * gp->volume;
        out_lr[i * 2]     = clamp16(l);
        out_lr[i * 2 + 1] = clamp16(r);
    }
}
