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

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>
#include "ingest.h"
#include "corpus_engine.h"
#include "grain.h"

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
    [P_SILENCE_GATE]  = {"silence_gate", "Silence Gate", T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.02f},
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
"\"root\":{\"name\":\"Concat\",\"knobs\":[\"corpus_file\",\"pool_mb\",\"rescan\",\"slice_mode\",\"min_slice_ms\",\"max_slice_ms\",\"analyze\",\"silence_gate\"],\"params\":[\"corpus_file\",\"pool_mb\",\"rescan\",\"slice_mode\",\"min_slice_ms\",\"max_slice_ms\",\"analyze\",\"silence_gate\",{\"level\":\"map\",\"label\":\"Map\"},{\"level\":\"grain\",\"label\":\"Grain\"},{\"level\":\"play\",\"label\":\"Play / Out\"}]},"
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

    char  corpus_file[512]; /* audio file picked via the filepath browser */
    char  corpus_path[512]; /* its parent dir — the folder analysed as the corpus */

    /* Engine */
    grain_engine_t   ge;                 /* render-thread grain cloud */
    corpus_index_t  *index;              /* current corpus (atomic acquire/release) */
    pthread_t        analyst;            /* single worker; builds the index off-thread */
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    int              arm;                /* a (re)analysis is requested */
    int              shutdown;
    int              analyst_started;
    char             pending_path[512];
    int              a_slice_mode, a_min_len, a_max_len, a_pool_samples;  /* snapshot at arm */
    float            a_sil;
    volatile int     progress;           /* 0..100, served by get_param("analyze") */
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

/* ── Engine: analyst worker + helpers ───────────────────────────────────────── */

/* Map x_desc/y_desc enum (Bright/Pitch/Loud/Noisy/Rolloff/MFCC1-3) → descriptor index. */
static int desc_index(int e) {
    switch (e) {
        case 0: return D_CENTROID_MEAN;  case 1: return D_PITCH;      case 2: return D_LOUD_MEAN;
        case 3: return D_FLATNESS_MEAN;  case 4: return D_ZCR_MEAN;   /* Rolloff → ZCR proxy */
        case 5: return D_MFCC0_MEAN + 1; case 6: return D_MFCC0_MEAN + 2; case 7: return D_MFCC0_MEAN + 3;
        default: return D_CENTROID_MEAN;
    }
}

/* MemAvailable in MB (parse /proc/meminfo directly — never shm_open). 0 on failure. */
static long mem_available_mb(void) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return 0;
    char b[2048];
    int n = (int)read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    char *p = strstr(b, "MemAvailable:");
    return p ? atol(p + 13) / 1024 : 0;   /* kB → MB */
}

/* Snapshot the corpus path + analysis params and wake the analyst. */
static void arm_analyst(concat_t *inst) {
    if (!inst->analyst_started || inst->corpus_path[0] == 0) return;
    pthread_mutex_lock(&inst->lock);
    strncpy(inst->pending_path, inst->corpus_path, sizeof(inst->pending_path) - 1);
    inst->pending_path[sizeof(inst->pending_path) - 1] = 0;
    inst->a_slice_mode = (int)inst->pv[P_SLICE_MODE];
    inst->a_min_len = (int)(inst->pv[P_MIN_SLICE] * 44.1f);
    inst->a_max_len = (int)(inst->pv[P_MAX_SLICE] * 44.1f);
    inst->a_sil = inst->pv[P_SILENCE_GATE];
    long pool_mb = (long)inst->pv[P_POOL_MB];
    long avail = mem_available_mb();                 /* OOM guard: leave a 350 MB reserve */
    long cap = pool_mb;
    if (avail > 0) { long usable = avail - 350; if (usable < 32) usable = 32; if (cap > usable) cap = usable; }
    if (cap < 8) cap = 8;
    inst->a_pool_samples = (int)(cap * 1024 * 1024 / 2);   /* MB → int16 samples */
    inst->arm = 1;
    pthread_cond_signal(&inst->cond);
    pthread_mutex_unlock(&inst->lock);
}

static void *analyst_thread(void *arg) {
    concat_t *inst = (concat_t *)arg;
    struct sched_param sp; sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);         /* demote off the shim's FIFO 70 */
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0, &cs); CPU_SET(1, &cs); CPU_SET(2, &cs);
    sched_setaffinity(0, sizeof(cs), &cs);           /* cores 0-2 (core 3 = SPI FIFO 90) */

    for (;;) {
        pthread_mutex_lock(&inst->lock);
        while (!inst->arm && !inst->shutdown) pthread_cond_wait(&inst->cond, &inst->lock);
        if (inst->shutdown) { pthread_mutex_unlock(&inst->lock); break; }
        inst->arm = 0;
        char path[512];
        strncpy(path, inst->pending_path, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
        int sm = inst->a_slice_mode, mn = inst->a_min_len, mx = inst->a_max_len, ps = inst->a_pool_samples;
        float sg = inst->a_sil;
        pthread_mutex_unlock(&inst->lock);

        if (path[0]) {
            inst->progress = 0;
            corpus_index_t *ni = corpus_index_build(path, 44100, sm, mn, mx, sg, ps, &inst->progress);
            corpus_index_t *old = __atomic_exchange_n(&inst->index, ni, __ATOMIC_ACQ_REL);
            if (old) { usleep(60000); corpus_index_free(old); }  /* let one render block pass */
            inst->progress = ni ? 100 : 0;
        }
    }
    return NULL;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    concat_t *inst = calloc(1, sizeof(concat_t));
    if (!inst) return NULL;
    for (int i = 0; i < NPARAMS; i++) inst->pv[i] = PARAMS[i].def;
    grain_engine_init(&inst->ge);
    pthread_mutex_init(&inst->lock, NULL);
    pthread_cond_init(&inst->cond, NULL);
    if (pthread_create(&inst->analyst, NULL, analyst_thread, inst) == 0) inst->analyst_started = 1;
    if (json_defaults && *json_defaults)
        set_param(inst, "state", json_defaults);        /* restore params; may arm the analyst */
    return inst;
}

static void destroy_instance(void *instance) {
    concat_t *inst = (concat_t *)instance;
    if (!inst) return;
    if (inst->analyst_started) {
        pthread_mutex_lock(&inst->lock);
        inst->shutdown = 1;
        pthread_cond_signal(&inst->cond);
        pthread_mutex_unlock(&inst->lock);
        pthread_join(inst->analyst, NULL);
        pthread_mutex_destroy(&inst->lock);
        pthread_cond_destroy(&inst->cond);
    }
    corpus_index_free(inst->index);
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
        arm_analyst(inst);                                /* analyse the picked folder */
        return;
    }

    if (strcmp(key, "rescan") == 0) {                     /* Pattern-3 action → re-analyse */
        int v = atoi(val);
        inst->pv[P_RESCAN] = (float)(v < 0 ? 0 : (v > 127 ? 127 : v));
        if (v != 0) arm_analyst(inst);
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
                "\"root\":\"/data/UserData/UserLibrary/Samples\",\"filter\":[\".wav\",\".flac\",\".mp3\"]}");
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
    if (strcmp(key, "analyze") == 0) return snprintf(buf, buf_len, "%d", inst->progress);  /* live 0..100 */
    if (strcmp(key, "_dbg") == 0) {   /* debug introspection */
        corpus_index_t *ix = __atomic_load_n(&inst->index, __ATOMIC_ACQUIRE);
        return snprintf(buf, buf_len, "started=%d arm=%d prog=%d idx=%d nslices=%d pool=%d path=[%s]",
                        inst->analyst_started, inst->arm, inst->progress,
                        ix != NULL, ix ? ix->nslices : -1, ix ? ix->pool_len : -1, inst->corpus_path);
    }

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
    corpus_index_t *ix = __atomic_load_n(&inst->index, __ATOMIC_ACQUIRE);
    if (!ix || ix->nslices == 0) { memset(out_lr, 0, sizeof(int16_t) * 2 * frames); return; }

    /* Repeat-avoid depth can change live. */
    query_set_repeat_avoid(&ix->query, (int)inst->pv[P_REPEAT_AVOID]);

    grain_params_t gp;
    gp.k_nearest    = (int)inst->pv[P_K_NEAREST];
    gp.radius       = inst->pv[P_RADIUS];
    gp.density      = inst->pv[P_DENSITY];
    gp.grain_size_ms = inst->pv[P_GRAIN_SIZE];
    gp.grain_env    = (int)inst->pv[P_GRAIN_ENV];
    gp.pitch_semi   = inst->pv[P_PITCH];
    gp.pitch_jitter = inst->pv[P_PITCH_JITTER];
    gp.pan_spread   = inst->pv[P_PAN_SPREAD];
    gp.reverse_prob = inst->pv[P_REVERSE];
    gp.volume       = inst->pv[P_VOLUME];
    gp.width        = inst->pv[P_WIDTH];

    /* v1: Map navigation (x_pos/y_pos over the two chosen descriptors). Notes /
     * Mosaic targeting = TODO; they render the Map target for now. */
    float t[NDESC], w[NDESC];
    query_target_map(&ix->query, desc_index((int)inst->pv[P_X_DESC]), desc_index((int)inst->pv[P_Y_DESC]),
                     inst->pv[P_X_POS], inst->pv[P_Y_POS], t, w);
    grain_render(&inst->ge, ix, t, w, &gp, out_lr, frames, 44100);
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
