/**
 * Concat — grain engine (clean-room MIT): the render-thread grain cloud.
 *
 * Schedules grains at `density` grains/s; each grain runs query_select on the
 * corpus index to pick a slice, then plays that slice's pool audio through a
 * window envelope with pitch/pan/reverse. Render-safe: no alloc, no locks.
 */
#ifndef CONCAT_GRAIN_H
#define CONCAT_GRAIN_H

#include <stdint.h>
#include "corpus_engine.h"

#define GRAIN_VOICES 128
#define GRAIN_ENV_TABLE 1024

typedef struct {
    int            active;
    const int16_t *audio;      /* pool + slice.pool_off */
    int            len;        /* slice length (samples) */
    double         pos;        /* fractional read position */
    double         rate;       /* playback rate (pitch) */
    int            grain_len;  /* grain length (output samples) */
    int            xfade;      /* loop-seam crossfade samples (0 = grain fits slice, no loop) */
    int            age;        /* samples elapsed */
    float          panl, panr;
    float          gain;       /* static per-grain gain (velocity etc.), frozen at spawn */
    const float   *amp;        /* LIVE envelope level (note-voice), NULL = constant 1 — lets a
                                * note's grains fade together on release regardless of length */
    const int     *ampgen;     /* voice generation; if it moves past `gen` the voice was reused → mute */
    int            gen;        /* voice generation captured at spawn */
    int            reverse;
} grain_t;

/* Live per-note amplitude source for a scheduled grain. `gen`/`gen_val` guard against a
 * freed note-voice slot being reused by a new note (would else "ghost re-amplify" old grains). */
typedef struct { const float *level; const int *gen; int gen_val; } grain_amp_t;

typedef struct {
    grain_t  voices[GRAIN_VOICES];
    double   sched_accum;      /* fractional grain scheduler */
    unsigned rng;
} grain_engine_t;

typedef struct {
    int   k_nearest;   float radius;
    float smooth;                  /* 0..1 concat-cost continuity weight (grain-to-grain smoothing) */
    float density;                 /* grains/s */
    float grain_size_ms;
    float grain_rand;              /* 0..1 random size spread per grain */
    int   grain_env;               /* 0 Hann .. 7 Blackman */
    float pitch_semi, pitch_jitter;
    float pan_spread, reverse_prob;
    float volume, width;
    float target_hz;               /* Notes mode: retune each grain to this pitch (0 = off) */
    int   max_voices;              /* polyphony cap (1..GRAIN_VOICES) */
} grain_params_t;

void grain_engine_init(grain_engine_t *ge);

/* Deactivate all grains (their audio pointers reference a specific corpus pool).
 * Call before the pool they point into is freed — e.g. on an index hot-swap. */
void grain_flush(grain_engine_t *ge);

/* Schedule grains for ONE voice/target into the shared grain pool. `accum` is that
 * voice's fractional grain scheduler; `gain` scales its grains (a note-voice passes
 * its envelope level); `target_hz` > 0 retunes each grain to that pitch. */
void grain_schedule(grain_engine_t *ge, corpus_index_t *ix,
                    const float *target, const float *weight, const grain_params_t *gp,
                    float gain, const grain_amp_t *amp, float target_hz, double *accum, int frames, int sr);

/* Render every active grain into out_lr (grain loops its slice; applies width + volume). */
void grain_mix(grain_engine_t *ge, const grain_params_t *gp, int16_t *out_lr, int frames, int sr);

#endif /* CONCAT_GRAIN_H */
