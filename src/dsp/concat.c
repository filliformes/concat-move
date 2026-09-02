/**
 * Concat — Schwung sound generator (corpus / concatenative synthesis)
 * Author: Filliformes
 * License: MIT
 *
 * Analyse a FOLDER of audio → slice → describe (per-slice MultiStats: MFCC,
 * centroid, flatness, ZCR, loudness, YIN pitch) → RAM pool of retained slices →
 * navigate the descriptor map (kNN + repetition avoidance) and play a grain
 * cloud; or mosaic the live line/mic input.
 *
 * API: plugin_api_v2_t (8 fields — get_error REQUIRED). dsp.so.
 * Audio: 44100 Hz, 128 frames/block, stereo interleaved int16 OUTPUT.
 *
 * === SCAFFOLD STATE ===
 * This file is the loadable skeleton: parameter surface, page-aware knob
 * overlay, ui_hierarchy, state (de)serialisation, MIDI plumbing. render_block
 * currently outputs SILENCE — the analysis threads, corpus pool, query, and
 * grain engine are the next implementation stage (see references/dsp-notes.md
 * and CLAUDE.md). Do NOT ship v0.1 without the engine.
 *
 * Planned architecture (see design-spec.md):
 *   Thread 1 (render/SPI): schedule+play grains, run kNN query; in Mosaic mode
 *     read host->mapped_memory + host->audio_in_offset and extract target descriptors.
 *   Thread 2 (analyst, SCHED_OTHER, cores 0-2): dr_libs decode → normalise
 *     (mono/44.1k/int16) → spectral-flux onset slice → per-frame descriptors
 *     (kissfft 1024/hop512 → MelBands→DCT MFCC, SpectralShape, YINFFT) →
 *     MultiStats mean+std → RobustScaling → write index. Atomic progress counter.
 *   Thread 3 (loader, SCHED_OTHER): build mono-int16 pool, atomic pointer swap.
 *   OOM guard: pool = min(pool_mb, MemAvailable - 350MB), evict least-used.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include "ingest.h"

#define SAMPLE_RATE 44100.0f

/* ── Parameter table (table-driven get/set + page-aware knob overlay) ───────── */

enum {
    /* Corpus */
    P_POOL_MB, P_RESCAN, P_SLICE_MODE, P_MIN_SLICE, P_MAX_SLICE, P_ANALYZE, P_SILENCE_GATE,
    /* Map */
    P_X_DESC, P_Y_DESC, P_X_POS, P_Y_POS, P_RADIUS, P_K_NEAREST, P_FOLLOW, P_REPEAT_AVOID,
    /* Grain */
    P_DENSITY, P_GRAIN_SIZE, P_GRAIN_ENV, P_PITCH, P_PITCH_JITTER, P_PAN_SPREAD, P_REVERSE,
    /* Play / Out */
    P_PLAY_MODE, P_INPUT_SRC, P_MOSAIC_RATE, P_NOTE_MATCH, P_DRY_MIX, P_VOLUME, P_WIDTH, P_SAT,
    NPARAMS
};

enum { T_FLOAT, T_INT, T_ENUM };

static const char *ENV_SLICE_MODE[] = {"Onset", "Fixed"};
static const char *ENV_DESC[]       = {"Bright", "Pitch", "Loud", "Noisy", "Rolloff", "MFCC1", "MFCC2", "MFCC3"};
static const char *ENV_FOLLOW[]     = {"Manual", "LFO", "Sequence"};
static const char *ENV_GRAIN[]      = {"Hann", "Tukey", "Tri"};
static const char *ENV_PLAY_MODE[]  = {"Map", "Notes", "Mosaic"};
static const char *ENV_INPUT_SRC[]  = {"Line", "Mic"};
static const char *ENV_MOSAIC_RATE[]= {"Onset", "1/16", "1/8", "1/4"};
static const char *ENV_NOTE_MATCH[] = {"Off", "Pitch", "Zone"};

typedef struct {
    const char *key;
    const char *label;
    int   type;              /* T_FLOAT | T_INT | T_ENUM */
    float min, max, step;
    const char **opts;       /* enum options (NULL otherwise) */
    int   nopts;
    float def;
    const char *unit;        /* popup suffix (NULL = none); matches module.json */
} pdef_t;

static const pdef_t PARAMS[NPARAMS] = {
    /* Corpus */
    [P_POOL_MB]       = {"pool_mb",      "Pool Cap",     T_INT,   16, 512, 1,  NULL, 0, 128, "MB"},
    [P_RESCAN]        = {"rescan",       "Rescan",       T_INT,    0, 127, 1,  NULL, 0, 0},
    [P_SLICE_MODE]    = {"slice_mode",   "Slice Mode",   T_ENUM,   0,   1, 1,  ENV_SLICE_MODE, 2, 0},
    [P_MIN_SLICE]     = {"min_slice_ms", "Min Slice",    T_FLOAT, 20, 500, 5,  NULL, 0, 60,  "ms"},
    [P_MAX_SLICE]     = {"max_slice_ms", "Max Slice",    T_FLOAT,100,2000,10,  NULL, 0, 400, "ms"},
    [P_ANALYZE]       = {"analyze",      "Analyze",      T_INT,    0, 100, 1,  NULL, 0, 0},
    [P_SILENCE_GATE]  = {"silence_gate", "Silence Gate", T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.1f},
    /* Map */
    [P_X_DESC]        = {"x_desc",       "X Descriptor", T_ENUM,   0,   7, 1,  ENV_DESC, 8, 0},
    [P_Y_DESC]        = {"y_desc",       "Y Descriptor", T_ENUM,   0,   7, 1,  ENV_DESC, 8, 1},
    [P_X_POS]         = {"x_pos",        "X Position",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.5f},
    [P_Y_POS]         = {"y_pos",        "Y Position",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.5f},
    [P_RADIUS]        = {"radius",       "Radius",       T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.15f},
    [P_K_NEAREST]     = {"k_nearest",    "K Nearest",    T_INT,    1,  16, 1,  NULL, 0, 4},
    [P_FOLLOW]        = {"follow",       "Follow",       T_ENUM,   0,   2, 1,  ENV_FOLLOW, 3, 0},
    [P_REPEAT_AVOID]  = {"repeat_avoid", "Repeat Avoid", T_INT,    0,  16, 1,  NULL, 0, 4},
    /* Grain */
    [P_DENSITY]       = {"density",      "Density",      T_FLOAT,  1, 100, 1,  NULL, 0, 20,  "g/s"},
    [P_GRAIN_SIZE]    = {"grain_size",   "Grain Size",   T_FLOAT, 10, 500, 5,  NULL, 0, 120, "ms"},
    [P_GRAIN_ENV]     = {"grain_env",    "Grain Env",    T_ENUM,   0,   2, 1,  ENV_GRAIN, 3, 0},
    [P_PITCH]         = {"pitch",        "Pitch",        T_FLOAT,-24,  24, 1,  NULL, 0, 0,   "st"},
    [P_PITCH_JITTER]  = {"pitch_jitter", "Pitch Jitter", T_FLOAT,  0,   1, 0.01f, NULL, 0, 0},
    [P_PAN_SPREAD]    = {"pan_spread",   "Pan Spread",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.3f},
    [P_REVERSE]       = {"reverse",      "Reverse",      T_FLOAT,  0,   1, 0.01f, NULL, 0, 0},
    /* Play / Out */
    [P_PLAY_MODE]     = {"play_mode",    "Play Mode",    T_ENUM,   0,   2, 1,  ENV_PLAY_MODE, 3, 0},
    [P_INPUT_SRC]     = {"input_src",    "Input Src",    T_ENUM,   0,   1, 1,  ENV_INPUT_SRC, 2, 0},
    [P_MOSAIC_RATE]   = {"mosaic_rate",  "Mosaic Rate",  T_ENUM,   0,   3, 1,  ENV_MOSAIC_RATE, 4, 0},
    [P_NOTE_MATCH]    = {"note_match",   "Note Match",   T_ENUM,   0,   2, 1,  ENV_NOTE_MATCH, 3, 1},
    [P_DRY_MIX]       = {"dry_mix",      "Dry Mix",      T_FLOAT,  0,   1, 0.01f, NULL, 0, 0},
    [P_VOLUME]        = {"volume",       "Volume",       T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.8f},
    [P_WIDTH]         = {"width",        "Width",        T_FLOAT,  0,   1, 0.01f, NULL, 0, 1.0f},
    [P_SAT]           = {"sat",          "Saturation",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0},
};

/* ui_hierarchy = the knob grid. Root is the landing page (Corpus/setup) with the
 * filepath **Source** cell (`corpus_file`) first; Map / Grain / Play are further
 * grid pages reached by paging. A `filepath` param IS a grid cell (breakbeat's
 * A_sample_path pattern) — jog-click it to open the file browser. Knob turns route
 * as set_param(key)/get_param(key) with metadata from chain_params, so there is no
 * legacy knob_N_* overlay. Ships in module.json too — keep the two in sync. */
static const char UI_HIERARCHY[] =
"{\"modes\":null,\"levels\":{"
"\"root\":{\"name\":\"Concat\",\"knobs\":[\"corpus_file\",\"pool_mb\",\"rescan\",\"slice_mode\",\"min_slice_ms\",\"max_slice_ms\",\"analyze\",\"silence_gate\"],\"params\":[{\"level\":\"map\",\"label\":\"Map\"},{\"level\":\"grain\",\"label\":\"Grain\"},{\"level\":\"play\",\"label\":\"Play / Out\"}]},"
"\"map\":{\"name\":\"Map\",\"knobs\":[\"x_desc\",\"y_desc\",\"x_pos\",\"y_pos\",\"radius\",\"k_nearest\",\"follow\",\"repeat_avoid\"],\"params\":[\"x_desc\",\"y_desc\",\"x_pos\",\"y_pos\",\"radius\",\"k_nearest\",\"follow\",\"repeat_avoid\"]},"
"\"grain\":{\"name\":\"Grain\",\"knobs\":[\"density\",\"grain_size\",\"grain_env\",\"pitch\",\"pitch_jitter\",\"pan_spread\",\"reverse\"],\"params\":[\"density\",\"grain_size\",\"grain_env\",\"pitch\",\"pitch_jitter\",\"pan_spread\",\"reverse\"]},"
"\"play\":{\"name\":\"Play / Out\",\"knobs\":[\"play_mode\",\"input_src\",\"mosaic_rate\",\"note_match\",\"dry_mix\",\"volume\",\"width\",\"sat\"],\"params\":[\"play_mode\",\"input_src\",\"mosaic_rate\",\"note_match\",\"dry_mix\",\"volume\",\"width\",\"sat\"]}"
"}}";

/* ── Instance state ─────────────────────────────────────────────────────────── */

typedef struct {
    float pv[NPARAMS];      /* live param values (enums/ints stored as float) */

    /* MIDI (Notes mode) */
    int   midi_note;
    int   note_active;
    float velocity;
    float pitch_bend;       /* semitones */
    float mod_wheel;

    /* TODO corpus engine: pool, index (slice descriptors), analyst/loader threads +
     * atomic progress, grain voices[24], repeat-avoid ring, mosaic FFT state. */
    char  corpus_file[512]; /* audio file picked via the filepath browser */
    char  corpus_path[512]; /* its parent dir — the folder analysed as the corpus */
} concat_t;

static void set_param(void *instance, const char *key, const char *val);   /* fwd */

/* ── Helpers ────────────────────────────────────────────────────────────────── */

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* Enum parse: accept option string OR numeric index; return clamped index. */
static int parse_enum(const char *val, const char **opts, int n) {
    for (int i = 0; i < n; i++) if (opts[i] && strcmp(val, opts[i]) == 0) return i;
    int idx = atoi(val);
    return idx < 0 ? 0 : (idx >= n ? n - 1 : idx);
}

static void apply_value(concat_t *inst, int idx, const char *val) {
    const pdef_t *p = &PARAMS[idx];
    float v;
    if (p->type == T_ENUM) v = (float)parse_enum(val, p->opts, p->nopts);
    else                   v = (float)atof(val);
    inst->pv[idx] = clampf(v, p->min, p->max);
    /* TODO: rescan action (P_RESCAN != 0 → re-arm analyst); slice/pool changes
     * → re-analyse or re-guard pool. Handled once the engine exists. */
}

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    concat_t *inst = calloc(1, sizeof(concat_t));
    if (!inst) return NULL;
    for (int i = 0; i < NPARAMS; i++) inst->pv[i] = PARAMS[i].def;
    if (json_defaults && *json_defaults)
        set_param(inst, "state", json_defaults);        /* restore persisted params */
    return inst;
}

static void destroy_instance(void *instance) {
    concat_t *inst = (concat_t *)instance;
    if (!inst) return;
    /* TODO: stop analyst/loader threads, free pool + index. */
    free(inst);
}

/* ── MIDI ───────────────────────────────────────────────────────────────────── */

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    concat_t *inst = (concat_t *)instance;
    if (!inst || len < 1) return;

    uint8_t status = msg[0] & 0xF0;

    /* Realtime clock (len==1) — for follow=Sequence / tempo-synced mosaic_rate (TODO). */
    if (len == 1) {
        /* 0xF8 tick / 0xFA start / 0xFB continue / 0xFC stop */
        return;
    }
    if (len < 3) return;
    uint8_t d1 = msg[1], d2 = msg[2];

    if (status == 0x90 && d2 > 0) {          /* Note On — Notes mode: pitch-match nearest slice (TODO) */
        inst->midi_note   = d1;
        inst->velocity    = d2 / 127.0f;
        inst->note_active = 1;
    } else if (status == 0x80 || (status == 0x90 && d2 == 0)) {  /* Note Off */
        if (inst->midi_note == d1) inst->note_active = 0;
    } else if (status == 0xE0) {             /* Pitch bend → ± transpose active grains */
        int bend = (d2 << 7) | d1;
        inst->pitch_bend = (bend - 8192) / 8192.0f * 2.0f;
    } else if (status == 0xB0 && d1 == 1) {  /* Mod wheel → radius/density (TODO) */
        inst->mod_wheel = d2 / 127.0f;
    } else if (status == 0xB0 && d1 == 123) {/* All notes off */
        inst->note_active = 0;
    }
}

/* ── set_param ──────────────────────────────────────────────────────────────── */

static void set_param(void *instance, const char *key, const char *val) {
    concat_t *inst = (concat_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "corpus_file") == 0) {            /* filepath browser → an audio file; use its folder */
        strncpy(inst->corpus_file, val, sizeof(inst->corpus_file) - 1);
        inst->corpus_file[sizeof(inst->corpus_file) - 1] = 0;
        strncpy(inst->corpus_path, val, sizeof(inst->corpus_path) - 1);
        inst->corpus_path[sizeof(inst->corpus_path) - 1] = 0;
        char *slash = strrchr(inst->corpus_path, '/');   /* dirname → the corpus folder */
        if (slash && slash != inst->corpus_path) *slash = 0;
        /* TODO: arm analyst on inst->corpus_path. */
        return;
    }

    /* State deserialize: newline-separated key=value (schema-robust). */
    if (strcmp(key, "state") == 0) {
        const char *p = val;
        while (*p) {
            const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
            const char *eq  = (const char *)memchr(p, '=', eol - p);
            if (eq && eq > p) {
                char k[64], v[256];
                int kl = (int)(eq - p), vl = (int)(eol - eq - 1);
                if (kl > 0 && kl < (int)sizeof(k) && vl >= 0 && vl < (int)sizeof(v)) {
                    memcpy(k, p, kl); k[kl] = 0;
                    memcpy(v, eq + 1, vl); v[vl] = 0;
                    if (strcmp(k, "state") != 0) set_param(inst, k, v);
                }
            }
            if (!*eol) break;
            p = eol + 1;
        }
        return;
    }

    /* Named param. (analyze is DSP-driven progress → ignore external writes.) */
    for (int i = 0; i < NPARAMS; i++)
        if (strcmp(key, PARAMS[i].key) == 0) {
            if (i != P_ANALYZE) apply_value(inst, i, val);
            return;
        }
}

/* ── get_param ──────────────────────────────────────────────────────────────── */

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    concat_t *inst = (concat_t *)instance;
    if (!inst || !key) return -1;

    /* Serve chain_params from the DSP (granny/breakbeat pattern) — the module.json
     * fallback does not reliably deliver param metadata to the knob grid, so a knob
     * with no metadata renders as a float 0..1. corpus_file is declared filepath here
     * so the grid draws it as a browser cell, not a float knob. (~2.7 KB < buffer.) */
    if (strcmp(key, "chain_params") == 0) {
        int n = 0;
#define CP_EMIT(...) do { if (n < buf_len) n += snprintf(buf + n, buf_len - n, __VA_ARGS__); } while (0)
        CP_EMIT("[{\"key\":\"corpus_file\",\"name\":\"Source\",\"type\":\"filepath\","
                "\"root\":\"/data/UserData\",\"filter\":[\".wav\",\".flac\",\".mp3\"]}");
        for (int i = 0; i < NPARAMS; i++) {
            const pdef_t *p = &PARAMS[i];
            CP_EMIT(",{\"key\":\"%s\",\"name\":\"%s\",", p->key, p->label);
            if (p->type == T_ENUM) {
                CP_EMIT("\"type\":\"enum\",\"options\":[");
                for (int j = 0; j < p->nopts; j++) CP_EMIT("%s\"%s\"", j ? "," : "", p->opts[j]);
                CP_EMIT("]}");
            } else if (p->type == T_INT) {
                CP_EMIT("\"type\":\"int\",\"min\":%d,\"max\":%d", (int)p->min, (int)p->max);
                if (p->unit) CP_EMIT(",\"unit\":\"%s\"", p->unit);
                CP_EMIT("}");
            } else {
                CP_EMIT("\"type\":\"float\",\"min\":%g,\"max\":%g,\"step\":%g",
                        (double)p->min, (double)p->max, (double)p->step);
                if (p->unit) CP_EMIT(",\"unit\":\"%s\"", p->unit);
                CP_EMIT("}");
            }
        }
        CP_EMIT("]");
#undef CP_EMIT
        return (n >= buf_len) ? (buf_len - 1) : n;
    }

    /* Sound generators MUST return ui_hierarchy from get_param. */
    if (strcmp(key, "ui_hierarchy") == 0) {
        int n = (int)strlen(UI_HIERARCHY);
        if (n >= buf_len) n = buf_len - 1;
        memcpy(buf, UI_HIERARCHY, n); buf[n] = 0;
        return n;
    }

    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "Concat");
    if (strcmp(key, "corpus_file") == 0) return snprintf(buf, buf_len, "%s", inst->corpus_file);
    if (strcmp(key, "corpus_path") == 0) return snprintf(buf, buf_len, "%s", inst->corpus_path);

    /* State serialize: one key=value line per persistent param. */
    if (strcmp(key, "state") == 0) {
        int n = 0;
        for (int i = 0; i < NPARAMS; i++) {
            if (i == P_RESCAN || i == P_ANALYZE) continue;  /* transient/read-only */
            if (n >= buf_len) return buf_len - 1;           /* guard: buf full, avoid size_t underflow */
            const pdef_t *p = &PARAMS[i];
            if (p->type == T_FLOAT) n += snprintf(buf + n, buf_len - n, "%s=%.6f\n", p->key, (double)inst->pv[i]);
            else                    n += snprintf(buf + n, buf_len - n, "%s=%d\n",   p->key, (int)lrintf(inst->pv[i]));
        }
        if (inst->corpus_file[0] && n < buf_len)
            n += snprintf(buf + n, buf_len - n, "corpus_file=%s\n", inst->corpus_file);
        return n;
    }

    /* Named param value. Enums return string names (required for menu editing). */
    for (int i = 0; i < NPARAMS; i++)
        if (strcmp(key, PARAMS[i].key) == 0) {
            const pdef_t *p = &PARAMS[i];
            if (p->type == T_ENUM) {
                int idx = (int)(inst->pv[i] + 0.5f);
                if (idx < 0) idx = 0;
                if (idx >= p->nopts) idx = p->nopts - 1;
                return snprintf(buf, buf_len, "%s", p->opts[idx]);
            }
            if (p->type == T_INT) return snprintf(buf, buf_len, "%d", (int)lrintf(inst->pv[i]));
            return snprintf(buf, buf_len, "%.4f", (double)inst->pv[i]);
        }

    /* CRITICAL: unknown keys return -1 (not 0), or Master FX float editing breaks. */
    return -1;
}

/* ── Audio ──────────────────────────────────────────────────────────────────── */

static void render_block(void *instance, int16_t *out_lr, int frames) {
    concat_t *inst = (concat_t *)instance;
    (void)inst;
    /*
     * SCAFFOLD: silence. No corpus is loaded until a folder is analysed, so
     * silence is the correct init state. Replace with the grain engine:
     *   1. If play_mode == Mosaic: read line/mic in (host->mapped_memory +
     *      host->audio_in_offset), FFT, extract target descriptors this block.
     *   2. Determine target point (Map: x_pos/y_pos; Notes: nearest by pitch;
     *      Mosaic: live descriptor vector; follow=LFO/Sequence drives it).
     *   3. Schedule grains at `density`; each grain: kNN query (weighted-Euclidean,
     *      RobustScaled dims) → random among K within `radius`, excluding the
     *      repeat_avoid ring buffer → play slice audio with grain_env window,
     *      pitch (+jitter), pan_spread, optional reverse. Grain-LOCK window-phase
     *      params at grain start.
     *   4. Sum grains, apply width + sat + volume, add dry_mix (Mosaic), clamp int16.
     * NEVER alloc / printf / lock here. Pool + index live in resident memory.
     */
    memset(out_lr, 0, sizeof(int16_t) * 2 * frames);
}

/* ── API v2 export (8 fields — get_error REQUIRED) ──────────────────────────── */

typedef struct {
    uint32_t api_version;
    void* (*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*on_midi)(void *, const uint8_t *, int, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    int   (*get_error)(void *, char *, int);
    void  (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

__attribute__((visibility("default")))
plugin_api_v2_t *move_plugin_init_v2(const void *host) {
    (void)host;
    static plugin_api_v2_t api = {
        .api_version      = 2,
        .create_instance  = create_instance,
        .destroy_instance = destroy_instance,
        .on_midi          = on_midi,
        .set_param        = set_param,
        .get_param        = get_param,
        .get_error        = NULL,
        .render_block     = render_block,
    };
    return &api;
}
