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
#include <dirent.h>
#include <sys/stat.h>
#include "ingest.h"
#include "corpus_engine.h"
#include "grain.h"

#define SAMPLE_RATE 44100.0f
#define ANALYSIS_N  1024

/* Minimal view of host_api_v1_t — fields up to audio_in_offset in the exact order
 * from schwung src/host/plugin_api_v1.h, so mapped_memory/audio_in_offset land at
 * the right offsets. We read only the live line/mic input for Mosaic mode. */
typedef struct {
    uint32_t api_version;
    int      sample_rate;
    int      frames_per_block;
    uint8_t *mapped_memory;
    int      audio_out_offset;
    int      audio_in_offset;
} concat_host_t;
static const concat_host_t *g_host = NULL;

/* On-disk index cache root (survives module redeploys; on the user partition, not
 * the full rootfs). A signature over file mtimes/sizes + slice params validates it. */
#define CONCAT_CACHE_DIR "/data/UserData/schwung/concat-cache"

/* ── Parameter table (table-driven get/set + page-aware knob overlay) ───────── */

enum {
    /* Corpus */
    P_SRC_SCOPE, P_POOL_MB, P_RESCAN, P_SLICE_MODE, P_MIN_SLICE, P_MAX_SLICE, P_ANALYZE, P_SILENCE_GATE,
    /* Map */
    P_MAP_MODE, P_X_DESC, P_Y_DESC, P_X_POS, P_Y_POS, P_RADIUS, P_K_NEAREST, P_FOLLOW, P_REPEAT_AVOID, P_SMOOTH, P_LFO_RATE, P_LFO_SHAPE,
    /* Grain */
    P_DENSITY, P_GRAIN_SIZE, P_GRAIN_ENV, P_PITCH, P_PITCH_JITTER, P_PAN_SPREAD, P_REVERSE, P_GRAIN_RAND,
    /* Play / Out */
    P_PLAY_MODE, P_NOTE_MATCH, P_DRY_MIX, P_VOLUME, P_WIDTH, P_SAT,
    /* Out — DJ filter + saturation (all modes) */
    P_CUTOFF, P_RESO,
    /* Expression — pad pressure (aftertouch) → destination */
    P_PRESSURE, P_PRESS_AMT, P_VEL_AMT, P_POLY,
    /* Shape — amp + filter envelopes */
    P_AMP_ATK, P_AMP_DEC, P_AMP_SUS, P_AMP_REL, P_FILT_AMT, P_FILT_ATK, P_FILT_DEC, P_FILT_SUS,
    NPARAMS
};

enum { T_FLOAT, T_INT, T_ENUM };

static const char *ENV_SRC_SCOPE[]  = {"Folder", "File"};
static const char *ENV_SLICE_MODE[] = {"Onset", "Fixed"};
static const char *ENV_DESC[]       = {"Bright", "Pitch", "Loud", "Noisy", "Rolloff", "MFCC1", "MFCC2", "MFCC3"};
static const char *ENV_FOLLOW[]     = {"Manual", "LFO", "Sequence"};
static const char *ENV_GRAIN[]      = {"Hann", "Tukey", "Tri", "Gauss", "Expo", "RevExpo", "Trapez", "Black"};
static const char *ENV_PLAY_MODE[]  = {"Map", "Notes", "Mosaic"};
static const char *ENV_PRESSURE[]   = {"Off", "Proximity", "Density", "Bright", "Sat", "Pitch", "Grain", "Jitter"};
static const char *ENV_LFO_SHAPE[]  = {"Circle", "SineX", "SineY", "S&H"};
static const char *ENV_MAP_MODE[]   = {"Raw", "Learned"};
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
    const char *viz;         /* raw viz JSON body, e.g. "\"group\":\"amp\",\"role\":\"attack\"" or "false" */
} pdef_t;

static const pdef_t PARAMS[NPARAMS] = {
    /* Corpus */
    [P_SRC_SCOPE]     = {"src_scope",    "Scope",        T_ENUM,   0,   1, 1,  ENV_SRC_SCOPE, 2, 1},   /* Folder | File (default File) */
    [P_POOL_MB]       = {"pool_mb",      "Pool Cap",     T_INT,   16, 512, 1,  NULL, 0, 512, "MB"},
    [P_RESCAN]        = {"rescan",       "Rescan",       T_INT,    0, 127, 1,  NULL, 0, 0},
    [P_SLICE_MODE]    = {"slice_mode",   "Slice Mode",   T_ENUM,   0,   1, 1,  ENV_SLICE_MODE, 2, 0},
    [P_MIN_SLICE]     = {"min_slice_ms", "Min Slice",    T_FLOAT, 20, 500, 5,  NULL, 0, 20,  "ms"},
    [P_MAX_SLICE]     = {"max_slice_ms", "Max Slice",    T_FLOAT,100,2000,10,  NULL, 0, 2000,"ms"},
    [P_ANALYZE]       = {"analyze",      "Analyze",      T_INT,    0, 100, 1,  NULL, 0, 0},
    [P_SILENCE_GATE]  = {"silence_gate", "Silence Gate", T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.02f},
    /* Map */
    [P_MAP_MODE]      = {"map_mode",     "Map Mode",     T_ENUM,   0,   1, 1,  ENV_MAP_MODE, 2, 0},   /* Raw axes / Learned PCA */
    [P_X_DESC]        = {"x_desc",       "X Descriptor", T_ENUM,   0,   7, 1,  ENV_DESC, 8, 0},
    [P_Y_DESC]        = {"y_desc",       "Y Descriptor", T_ENUM,   0,   7, 1,  ENV_DESC, 8, 1},
    [P_X_POS]         = {"x_pos",        "X Position",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.5f},
    [P_Y_POS]         = {"y_pos",        "Y Position",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.5f},
    [P_RADIUS]        = {"radius",       "Radius",       T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.15f},
    [P_K_NEAREST]     = {"k_nearest",    "K Nearest",    T_INT,    1,  16, 1,  NULL, 0, 4},
    [P_FOLLOW]        = {"follow",       "Follow",       T_ENUM,   0,   2, 1,  ENV_FOLLOW, 3, 0},
    [P_REPEAT_AVOID]  = {"repeat_avoid", "Repeat Avoid", T_INT,    0,  16, 1,  NULL, 0, 4},
    [P_SMOOTH]        = {"smooth",       "Smooth",       T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.35f},   /* concat-cost continuity weight */
    [P_LFO_RATE]      = {"lfo_rate",     "LFO Rate",     T_FLOAT, 0.01f, 10, 0.01f, NULL, 0, 0.2f, "Hz", "{\"group\":\"lfo\",\"role\":\"rate\"}"},
    [P_LFO_SHAPE]     = {"lfo_shape",    "LFO Shape",    T_ENUM,   0,   3, 1,  ENV_LFO_SHAPE, 4, 0, NULL, "{\"group\":\"lfo\",\"role\":\"shape\"}"},
    /* Grain */
    [P_DENSITY]       = {"density",      "Density",      T_FLOAT,  1, 100, 1,  NULL, 0, 20,  "g/s"},
    [P_GRAIN_SIZE]    = {"grain_size",   "Grain Size",   T_FLOAT, 10,2000, 1,  NULL, 0, 120, "ms"},
    [P_GRAIN_ENV]     = {"grain_env",    "Grain Env",    T_ENUM,   0,   7, 1,  ENV_GRAIN, 8, 0},
    [P_PITCH]         = {"pitch",        "Pitch",        T_FLOAT,-24,  24, 1,  NULL, 0, 0,   "st"},
    [P_PITCH_JITTER]  = {"pitch_jitter", "Pitch Jitter", T_FLOAT,  0,   1, 0.01f, NULL, 0, 0},
    [P_PAN_SPREAD]    = {"pan_spread",   "Pan Spread",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.3f},
    [P_REVERSE]       = {"reverse",      "Reverse",      T_FLOAT,  0,   1, 0.01f, NULL, 0, 0},
    [P_GRAIN_RAND]    = {"grain_rand",   "Grain Random", T_FLOAT,  0,   1, 0.01f, NULL, 0, 0},
    /* Play / Out */
    [P_PLAY_MODE]     = {"play_mode",    "Play Mode",    T_ENUM,   0,   2, 1,  ENV_PLAY_MODE, 3, 1},   /* default Notes */
    [P_NOTE_MATCH]    = {"note_match",   "Note Match",   T_ENUM,   0,   2, 1,  ENV_NOTE_MATCH, 3, 2},   /* default Zone */
    [P_DRY_MIX]       = {"dry_mix",      "Dry Mix",      T_FLOAT,  0,   1, 0.01f, NULL, 0, 0},
    [P_VOLUME]        = {"volume",       "Volume",       T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.8f},
    [P_WIDTH]         = {"width",        "Width",        T_FLOAT,  0,   1, 0.01f, NULL, 0, 1.0f},
    [P_SAT]           = {"sat",          "Saturation",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0},
    /* Out */
    /* DJ filter: one bipolar knob (center = bypass, left = LP down, right = HP up) */
    /* DJ filter is bipolar (center bypass, L=LP down, R=HP up) — the auto-detected
     * LP filter graphic is misleading, so kill the graphic and leave a plain dial. */
    [P_CUTOFF]        = {"cutoff",       "Filter",       T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.5f, NULL, "false"},
    [P_RESO]          = {"resonance",    "Resonance",    T_FLOAT,  0,   1, 0.01f, NULL, 0, 0,    NULL, "false"},
    /* Expression */
    [P_PRESSURE]      = {"pressure",     "Pressure",     T_ENUM,   0,   7, 1,  ENV_PRESSURE, 8, 2},   /* default Density */
    [P_PRESS_AMT]     = {"press_amt",    "Pressure Amt", T_FLOAT,  0,   1, 0.01f, NULL, 0, 1.0f},
    [P_VEL_AMT]       = {"vel_amt",      "Velocity Amt", T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.8f},
    [P_POLY]          = {"poly",         "Polyphony",    T_INT,    1, 128, 1,  NULL, 0, 48},
    /* Shape — amp + filter envelopes (0..1 time knobs mapped to seconds; sus/amt are levels) */
    [P_AMP_ATK]       = {"amp_atk",      "Amp Attack",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.01f, NULL, "{\"group\":\"amp\",\"role\":\"attack\"}"},
    [P_AMP_DEC]       = {"amp_dec",      "Amp Decay",    T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.2f,  NULL, "{\"group\":\"amp\",\"role\":\"decay\"}"},
    [P_AMP_SUS]       = {"amp_sus",      "Amp Sustain",  T_FLOAT,  0,   1, 0.01f, NULL, 0, 1.0f,  NULL, "{\"group\":\"amp\",\"role\":\"sustain\"}"},
    [P_AMP_REL]       = {"amp_rel",      "Amp Release",  T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.2f,  NULL, "{\"group\":\"amp\",\"role\":\"release\"}"},
    [P_FILT_AMT]      = {"filt_amt",     "Filter Env",   T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.45f},
    [P_FILT_ATK]      = {"filt_atk",     "Filter Attack",T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.01f},
    [P_FILT_DEC]      = {"filt_dec",     "Filter Decay", T_FLOAT,  0,   1, 0.01f, NULL, 0, 0.3f},
    [P_FILT_SUS]      = {"filt_sus",     "Filter Sustain",T_FLOAT, 0,   1, 0.01f, NULL, 0, 0},
};

/* ui_hierarchy = the knob grid. Root is the landing page (Corpus/setup) with the
 * filepath **Source** cell (`corpus_file`) first; Map / Grain / Play are further
 * grid pages reached by paging. A `filepath` param IS a grid cell (breakbeat's
 * A_sample_path pattern) — jog-click it to open the file browser. Knob turns route
 * as set_param(key)/get_param(key) with metadata from chain_params, so there is no
 * legacy knob_N_* overlay. Ships in module.json too — keep the two in sync. */
static const char UI_HIERARCHY[] =
"{\"modes\":null,\"levels\":{"
"\"root\":{\"name\":\"Concat\",\"knobs\":[\"x_pos\",\"y_pos\",\"density\",\"grain_size\",\"amp_atk\",\"amp_rel\",\"poly\",\"cutoff\"],\"params\":[\"x_pos\",\"y_pos\",\"density\",\"grain_size\",\"amp_atk\",\"amp_rel\",\"poly\",\"cutoff\",{\"level\":\"view\",\"label\":\"View\"},{\"level\":\"corpus\",\"label\":\"Corpus\"},{\"level\":\"map\",\"label\":\"Map\"},{\"level\":\"mod_set\",\"label\":\"Mod & Set\"},{\"level\":\"grain\",\"label\":\"Grain\"},{\"level\":\"shape\",\"label\":\"Shape\"},{\"level\":\"play\",\"label\":\"Play\"},{\"level\":\"out\",\"label\":\"Out\"}]},"
/* View carries its own NAVIGATION knobs (radius / k-nearest / smooth alongside
 * x/y). The planner builds both a knob-grid page and the as_page canvas page
 * from the same knobs[] (the canvas encoders are that list; it cannot declare
 * its own), and the only grid-suppression rule needs the set to duplicate an
 * earlier-walked level (root). Sharing root's set would lose the nav knobs, so
 * View deliberately keeps two pages: "View" (grid) then "View - 2" (the map). */
"\"view\":{\"name\":\"View\",\"list_param\":\"preset_index\",\"count_param\":\"preset_count\",\"name_param\":\"preset_name\",\"knobs\":[\"x_pos\",\"y_pos\",\"radius\",\"density\",\"grain_size\",\"k_nearest\",\"smooth\",\"cutoff\"],\"params\":[\"map_view\",\"x_pos\",\"y_pos\",\"radius\",\"density\",\"grain_size\",\"k_nearest\",\"smooth\",\"cutoff\"]},"
"\"corpus\":{\"name\":\"Corpus\",\"knobs\":[\"corpus_file\",\"pool_mb\",\"rescan\",\"slice_mode\",\"min_slice_ms\",\"max_slice_ms\",\"analyze\",\"silence_gate\"],\"params\":[\"corpus_file\",\"pool_mb\",\"rescan\",\"slice_mode\",\"min_slice_ms\",\"max_slice_ms\",\"analyze\",\"silence_gate\"]},"
"\"mod_set\":{\"name\":\"Mod & Set\",\"knobs\":[\"follow\",\"lfo_rate\",\"lfo_shape\",\"smooth\",\"src_scope\"],\"params\":[\"follow\",\"lfo_rate\",\"lfo_shape\",\"smooth\",\"src_scope\"]},"
"\"map\":{\"name\":\"Map\",\"knobs\":[\"x_desc\",\"y_desc\",\"x_pos\",\"y_pos\",\"map_mode\",\"radius\",\"k_nearest\",\"repeat_avoid\"],\"params\":[\"x_desc\",\"y_desc\",\"x_pos\",\"y_pos\",\"map_mode\",\"radius\",\"k_nearest\",\"repeat_avoid\"]},"
"\"grain\":{\"name\":\"Grain\",\"knobs\":[\"density\",\"grain_size\",\"grain_env\",\"pitch\",\"pan_spread\",\"grain_rand\",\"reverse\",\"pitch_jitter\"],\"params\":[\"density\",\"grain_size\",\"grain_env\",\"pitch\",\"pan_spread\",\"grain_rand\",\"reverse\",\"pitch_jitter\"]},"
"\"shape\":{\"name\":\"Shape\",\"knobs\":[\"amp_atk\",\"amp_dec\",\"amp_sus\",\"amp_rel\",\"filt_amt\",\"filt_atk\",\"filt_dec\",\"filt_sus\"],\"params\":[\"amp_atk\",\"amp_dec\",\"amp_sus\",\"amp_rel\",\"filt_amt\",\"filt_atk\",\"filt_dec\",\"filt_sus\"]},"
"\"play\":{\"name\":\"Play\",\"knobs\":[\"play_mode\",\"note_match\",\"pressure\",\"press_amt\",\"vel_amt\",\"poly\"],\"params\":[\"play_mode\",\"note_match\",\"pressure\",\"press_amt\",\"vel_amt\",\"poly\"]},"
"\"out\":{\"name\":\"Out\",\"knobs\":[\"cutoff\",\"resonance\",\"sat\",\"dry_mix\",\"volume\",\"width\"],\"params\":[\"cutoff\",\"resonance\",\"sat\",\"dry_mix\",\"volume\",\"width\"]}"
"}}";

/* ── Instance state ─────────────────────────────────────────────────────────── */

#define MAX_NOTES 16
typedef struct {
    int    active;          /* in use (held or releasing) */
    int    held;            /* note currently down */
    int    note;
    float  vel;
    int    amp_stage;       /* per-note amp ADSR (Shape amp params) */
    float  amp_level;
    double accum;           /* per-note grain scheduler */
    int    gen;             /* bumped each note-on; grains of an old gen are muted on slot reuse */
} note_voice_t;

typedef struct {
    float pv[NPARAMS];      /* live param values (enums/ints stored as float) */

    /* MIDI (Notes mode) */
    int   midi_note;
    int   note_active;
    float velocity;
    float pitch_bend;       /* semitones */
    float mod_wheel;
    float aftertouch;       /* pad pressure 0..1 (poly AT / channel pressure) */

    char  corpus_file[512]; /* audio file picked via the filepath browser */
    char  corpus_path[512]; /* its parent dir — the folder analysed as the corpus */

    /* Engine */
    grain_engine_t   ge;                 /* render-thread grain cloud */
    corpus_index_t  *index;              /* current corpus (atomic acquire/release) */
    corpus_index_t  *ge_ix;              /* index the live grains' audio points into (flush on change) */
    pthread_t        analyst;            /* single worker; builds the index off-thread */
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    int              arm;                /* a (re)analysis is requested */
    int              auto_pick;          /* no saved corpus: discover a default on the analyst */
    int              preset_idx;         /* current factory recipe (label; 0 = Init) */
    int              shutdown;
    int              analyst_started;
    char             pending_path[512];
    int              a_slice_mode, a_min_len, a_max_len, a_pool_samples;  /* snapshot at arm */
    int              a_force;             /* 1 = rescan (ignore disk cache) */
    float            a_sil;
    volatile int     progress;           /* 0..100, served by get_param("analyze") */

    /* Mosaic mode: live-input descriptor extraction (render thread) */
    const concat_host_t *host;
    analysis_t          *mono_analysis;
    float                in_frame[ANALYSIS_N];
    int                  in_fill;
    float                mosaic_t[NDESC], mosaic_w[NDESC];
    int                  mosaic_ready;

    /* Output filter (TPT state-variable, per channel) */
    float                svf_ic1[2], svf_ic2[2];

    /* Global envelopes (Map/Mosaic amp; filter env is always global) */
    int                  amp_stage, filt_stage, last_gate;
    float                amp_level, amp_prev, filt_level;

    /* Map LFO (Follow=LFO): self-animates X/Y around their knob positions */
    float                lfo_phase, lfo_shx, lfo_shy;
    unsigned             lfo_rng;

    /* MIDI clock / tempo sync (Follow=Sequence locks the LFO to the beat) */
    uint64_t             global_samples, last_beat_sample;
    float                samples_per_beat;   /* from incoming 24-PPQN clock; default = 120 BPM */
    int                  clock_tick;

    /* Polyphonic Notes mode: one voice per held pad, each with its own amp env */
    note_voice_t         notes[MAX_NOTES];

    /* Map mode pad clusters: holding a pad jumps the query target to a k-means cluster */
    int                  last_pad_note, pads_held;
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

/* ── Factory presets ("recipes") ─────────────────────────────────────────────
 * Served to the host's preset browser through the ui_hierarchy triple on the
 * View level (list_param=preset_index, count_param=preset_count,
 * name_param=preset_name). Selecting one in the browser LOADS it. A recipe sets
 * PLAY-scope params only ("k=v;k=v", enums by option name) and never touches the
 * Corpus page: scope/pool/slicing are corpus-specific and would re-trigger
 * analysis, and a baked file path would not exist on another Move. "Init"
 * (empty kv) resets the play-scope params to their defaults.
 * Starter set — tune freely; the names are what the browser shows. */
typedef struct { const char *name; const char *kv; } recipe_t;
static const recipe_t RECIPES[] = {
    {"Init",     ""},
    {"Cloud",    "play_mode=Map;follow=LFO;lfo_shape=Circle;lfo_rate=0.05;density=60;grain_size=400;grain_env=Hann;"
                 "pan_spread=0.8;radius=0.4;smooth=0.6;k_nearest=6;amp_atk=0.3;amp_rel=0.6;width=1"},
    {"Stutter",  "play_mode=Notes;note_match=Zone;density=30;grain_size=40;grain_env=Tukey;radius=0.05;k_nearest=2;"
                 "smooth=0.1;repeat_avoid=0;amp_atk=0;amp_rel=0.05;pan_spread=0.2"},
    {"Drone",    "play_mode=Map;follow=LFO;lfo_shape=Circle;lfo_rate=0.02;density=12;grain_size=1500;grain_env=Gauss;"
                 "radius=0.3;smooth=0.8;amp_atk=0.6;amp_sus=1;amp_rel=0.8;width=1;pan_spread=0.6"},
    {"Scatter",  "play_mode=Map;follow=LFO;lfo_shape=S&H;lfo_rate=2;density=40;grain_size=120;grain_rand=0.7;"
                 "pitch_jitter=0.4;pan_spread=1;reverse=0.5;radius=0.9;k_nearest=16;smooth=0;repeat_avoid=16"},
    {"Pitched",  "play_mode=Notes;note_match=Pitch;density=25;grain_size=150;pitch_jitter=0;radius=0.1;smooth=0.5;"
                 "vel_amt=0.9;amp_atk=0.02;amp_rel=0.3;grain_env=Hann"},
    {"Mosaic",   "play_mode=Mosaic;density=30;grain_size=80;radius=0.15;k_nearest=4;smooth=0.4;dry_mix=0.3;"
                 "amp_atk=0.01;amp_rel=0.1;grain_env=Tukey"},
    {"Sequence", "play_mode=Map;follow=Sequence;lfo_shape=Circle;lfo_rate=0.25;density=20;grain_size=200;"
                 "radius=0.25;smooth=0.5;amp_rel=0.4;grain_env=Hann"},
};
#define NRECIPES ((int)(sizeof(RECIPES) / sizeof(RECIPES[0])))

/* The Corpus page is the first 8 entries of the table; a recipe never writes them. */
static int is_corpus_param(int i) { return i <= P_SILENCE_GATE; }

static void apply_recipe(concat_t *inst, int idx) {
    if (idx < 0) idx = 0;
    if (idx >= NRECIPES) idx = NRECIPES - 1;
    /* A recipe is absolute, not layered: reset play-scope params to defaults first. */
    for (int i = 0; i < NPARAMS; i++)
        if (!is_corpus_param(i)) inst->pv[i] = PARAMS[i].def;
    /* "k=v;k=v" — short, bounded, no alloc (this runs on the SPI callback). */
    const char *p = RECIPES[idx].kv;
    while (p && *p) {
        const char *end = strchr(p, ';'); if (!end) end = p + strlen(p);
        const char *eq  = (const char *)memchr(p, '=', end - p);
        if (eq && eq > p) {
            char k[32], v[32];
            int kl = (int)(eq - p), vl = (int)(end - eq - 1);
            if (kl > 0 && kl < (int)sizeof k && vl >= 0 && vl < (int)sizeof v) {
                memcpy(k, p, kl); k[kl] = 0; memcpy(v, eq + 1, vl); v[vl] = 0;
                for (int i = 0; i < NPARAMS; i++)
                    if (!is_corpus_param(i) && strcmp(k, PARAMS[i].key) == 0) { apply_value(inst, i, v); break; }
            }
        }
        if (!*end) break;
        p = end + 1;
    }
    inst->preset_idx = idx;
    inst->lfo_shx = inst->pv[P_X_POS]; inst->lfo_shy = inst->pv[P_Y_POS];   /* re-centre the map LFO */
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

/* Derive the analysed path from the picked file: the file itself when Scope=File
 * (that one sample becomes the whole corpus), or its parent folder when Scope=Folder. */
static void set_corpus_path(concat_t *inst) {
    strncpy(inst->corpus_path, inst->corpus_file, sizeof(inst->corpus_path) - 1);
    inst->corpus_path[sizeof(inst->corpus_path) - 1] = 0;
    if ((int)inst->pv[P_SRC_SCOPE] == 0) {              /* Folder → dirname */
        char *slash = strrchr(inst->corpus_path, '/');
        if (slash && slash != inst->corpus_path) *slash = 0;
    }
}

/* Default corpus when none is saved: the first audio file of the first sub-folder
 * of the Samples root, both taken in name order. Worker-thread only (opendir /
 * stat / malloc). Writes an absolute path to `out`; returns 1 on a hit. */
#define CONCAT_SAMPLES_ROOT "/data/UserData/UserLibrary/Samples"
static int auto_pick_corpus(char *out, int out_sz) {
    DIR *d = opendir(CONCAT_SAMPLES_ROOT);
    if (!d) return 0;
    char first_dir[256] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;                 /* skip ., .., hidden */
        char full[INGEST_PATH];
        snprintf(full, sizeof full, "%s/%s", CONCAT_SAMPLES_ROOT, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (first_dir[0] == 0 || strcmp(e->d_name, first_dir) < 0) {
            strncpy(first_dir, e->d_name, sizeof first_dir - 1);
            first_dir[sizeof first_dir - 1] = 0;
        }
    }
    closedir(d);
    if (first_dir[0] == 0) return 0;

    char dir[INGEST_PATH];
    snprintf(dir, sizeof dir, "%s/%s", CONCAT_SAMPLES_ROOT, first_dir);
    char (*paths)[INGEST_PATH] = malloc(sizeof(*paths) * 256);
    if (!paths) return 0;
    int n = ingest_list_audio(dir, paths, 256);
    if (n <= 0) { free(paths); return 0; }
    int best = 0;                                          /* first file in name order */
    for (int i = 1; i < n; i++) if (strcmp(paths[i], paths[best]) < 0) best = i;
    strncpy(out, paths[best], out_sz - 1); out[out_sz - 1] = 0;
    free(paths);
    return 1;
}

/* Snapshot the corpus path + analysis params and wake the analyst.
 * force = 1 (rescan) rebuilds from scratch, ignoring the on-disk cache. */
static void arm_analyst(concat_t *inst, int force) {
    if (!inst->analyst_started || inst->corpus_path[0] == 0) return;
    pthread_mutex_lock(&inst->lock);
    inst->a_force = force;
    strncpy(inst->pending_path, inst->corpus_path, sizeof(inst->pending_path) - 1);
    inst->pending_path[sizeof(inst->pending_path) - 1] = 0;
    inst->a_slice_mode = (int)inst->pv[P_SLICE_MODE];
    inst->a_min_len = (int)(inst->pv[P_MIN_SLICE] * 44.1f);
    inst->a_max_len = (int)(inst->pv[P_MAX_SLICE] * 44.1f);
    if (inst->a_min_len > inst->a_max_len) inst->a_min_len = inst->a_max_len;   /* guard degenerate slicing */
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
        while (!inst->arm && !inst->auto_pick && !inst->shutdown) pthread_cond_wait(&inst->cond, &inst->lock);
        if (inst->shutdown) { pthread_mutex_unlock(&inst->lock); break; }

        /* No saved corpus: discover a default (first file of the first Samples
         * sub-folder) off the SPI path, then arm a normal build on it. */
        if (inst->auto_pick) {
            inst->auto_pick = 0;
            pthread_mutex_unlock(&inst->lock);
            char f[512];
            if (inst->corpus_file[0] == 0 && auto_pick_corpus(f, sizeof f)) {
                strncpy(inst->corpus_file, f, sizeof(inst->corpus_file) - 1);
                inst->corpus_file[sizeof(inst->corpus_file) - 1] = 0;
                set_corpus_path(inst);
                arm_analyst(inst, 0);
            }
            continue;
        }
        inst->arm = 0;
        char path[512];
        strncpy(path, inst->pending_path, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
        int sm = inst->a_slice_mode, mn = inst->a_min_len, mx = inst->a_max_len, ps = inst->a_pool_samples;
        int force = inst->a_force;
        float sg = inst->a_sil;
        pthread_mutex_unlock(&inst->lock);

        if (path[0]) {
            inst->progress = 0;
            corpus_index_t *ni = corpus_index_build(path, 44100, sm, mn, mx, sg, ps,
                                                    CONCAT_CACHE_DIR, force, &inst->progress);
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
    inst->lfo_rng = 0x1234567u;
    inst->lfo_shx = inst->pv[P_X_POS]; inst->lfo_shy = inst->pv[P_Y_POS];
    inst->samples_per_beat = 22050.0f;   /* 120 BPM fallback until MIDI clock arrives */
    grain_engine_init(&inst->ge);
    inst->host = g_host;
    inst->mono_analysis = analysis_create(44100, ANALYSIS_N, 40, 13);   /* Mosaic target extraction */
    pthread_mutex_init(&inst->lock, NULL);
    pthread_cond_init(&inst->cond, NULL);
    if (pthread_create(&inst->analyst, NULL, analyst_thread, inst) == 0) inst->analyst_started = 1;
    if (json_defaults && *json_defaults)
        set_param(inst, "state", json_defaults);        /* restore params; may arm the analyst */
    if (inst->corpus_file[0] == 0 && inst->analyst_started) {
        /* Fresh instance with no saved corpus: let the analyst discover a default
         * (scanning is disk I/O — never do it here, module load blocks SPI). */
        pthread_mutex_lock(&inst->lock);
        inst->auto_pick = 1;
        pthread_cond_signal(&inst->cond);
        pthread_mutex_unlock(&inst->lock);
    }
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
    analysis_destroy(inst->mono_analysis);
    corpus_index_free(inst->index);
    free(inst);
}

/* ── MIDI ───────────────────────────────────────────────────────────────────── */

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    concat_t *inst = (concat_t *)instance;
    if (!inst || len < 1) return;

    uint8_t status = msg[0] & 0xF0;

    if (len == 1) {                          /* system realtime (1 byte) — MIDI clock/transport */
        if (msg[0] == 0xF8) {                /* timing clock: 24 per quarter note */
            if (++inst->clock_tick >= 24) {  /* one beat elapsed → measure its length */
                inst->clock_tick = 0;
                if (inst->last_beat_sample > 0) {
                    float spb = (float)(inst->global_samples - inst->last_beat_sample);
                    if (spb > 2000.0f && spb < 300000.0f)   /* ~9..1300 BPM sanity */
                        inst->samples_per_beat = 0.5f * inst->samples_per_beat + 0.5f * spb;
                }
                inst->last_beat_sample = inst->global_samples;
            }
        } else if (msg[0] == 0xFA) {         /* start → align the downbeat */
            inst->clock_tick = 0; inst->lfo_phase = 0.0f; inst->last_beat_sample = inst->global_samples;
        }
        return;
    }
    if (status == 0xD0 && len >= 2) { inst->aftertouch = msg[1] / 127.0f; return; }  /* channel pressure */
    if (len < 3) return;
    uint8_t d1 = msg[1], d2 = msg[2];

    if (status == 0x90 && d2 > 0) {          /* Note On — allocate a polyphonic voice */
        int slot = -1;
        for (int i = 0; i < MAX_NOTES; i++) if (inst->notes[i].active && inst->notes[i].note == d1) { slot = i; break; }
        if (slot < 0) for (int i = 0; i < MAX_NOTES; i++) if (!inst->notes[i].active) { slot = i; break; }
        if (slot < 0) slot = 0;              /* steal voice 0 */
        note_voice_t *nv = &inst->notes[slot];
        nv->active = 1; nv->held = 1; nv->note = d1; nv->vel = d2 / 127.0f;
        nv->amp_stage = 1; nv->amp_level = 0.0f; nv->accum = 0.0; nv->gen++;   /* supersede any old grains on this slot */
        inst->note_active = 1; inst->midi_note = d1; inst->velocity = d2 / 127.0f;
        inst->last_pad_note = d1; inst->pads_held++;      /* Map-mode cluster jump */
    } else if (status == 0x80 || (status == 0x90 && d2 == 0)) {  /* Note Off → release the voice */
        for (int i = 0; i < MAX_NOTES; i++)
            if (inst->notes[i].active && inst->notes[i].held && inst->notes[i].note == d1) {
                inst->notes[i].held = 0; inst->notes[i].amp_stage = 4;
            }
        int any = 0; for (int i = 0; i < MAX_NOTES; i++) if (inst->notes[i].held) any = 1;
        inst->note_active = any; inst->aftertouch = 0.0f;
        if (--inst->pads_held < 0) inst->pads_held = 0;
    } else if (status == 0xA0) {             /* Poly aftertouch = pad pressure */
        inst->aftertouch = d2 / 127.0f;
    } else if (status == 0xE0) {             /* Pitch bend → ± transpose */
        int bend = (d2 << 7) | d1;
        inst->pitch_bend = (bend - 8192) / 8192.0f * 2.0f;
    } else if (status == 0xB0 && d1 == 1) {  /* Mod wheel (TODO) */
        inst->mod_wheel = d2 / 127.0f;
    } else if (status == 0xB0 && d1 == 123) {/* All notes off */
        for (int i = 0; i < MAX_NOTES; i++) if (inst->notes[i].active) { inst->notes[i].held = 0; inst->notes[i].amp_stage = 4; }
        inst->note_active = 0; inst->aftertouch = 0.0f; inst->pads_held = 0;
    }
}

/* ── set_param ──────────────────────────────────────────────────────────────── */

static void set_param(void *instance, const char *key, const char *val) {
    concat_t *inst = (concat_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "corpus_file") == 0) {            /* filepath browser → an audio file */
        strncpy(inst->corpus_file, val, sizeof(inst->corpus_file) - 1);
        inst->corpus_file[sizeof(inst->corpus_file) - 1] = 0;
        set_corpus_path(inst);                            /* file or its folder, per Scope */
        arm_analyst(inst, 0);                             /* analyse it (use cache if valid) */
        return;
    }

    if (strcmp(key, "src_scope") == 0) {              /* Folder|File → re-read the same pick */
        apply_value(inst, P_SRC_SCOPE, val);
        if (inst->corpus_file[0]) { set_corpus_path(inst); arm_analyst(inst, 0); }
        return;
    }

    if (strcmp(key, "rescan") == 0) {                     /* Pattern-3 action → re-analyse */
        int v = atoi(val);
        inst->pv[P_RESCAN] = (float)(v < 0 ? 0 : (v > 127 ? 127 : v));
        if (v != 0) arm_analyst(inst, 1);                 /* rescan → force rebuild, ignore cache */
        return;
    }

    /* Recipe browser: the host writes the index on each jog detent → LOAD it. */
    if (strcmp(key, "preset_index") == 0) { apply_recipe(inst, atoi(val)); return; }
    /* State restore of the LABEL only — never re-applies, or a reload would
     * clobber the saved (tweaked) values with the recipe's. */
    if (strcmp(key, "preset_idx") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v >= NRECIPES) v = NRECIPES - 1;
        inst->preset_idx = v;
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

/* Project slice `idx` onto the normalized 0..1 map plane the scatter uses:
 * PCA plane 1/2 when learned, else centroid (x) vs loudness (y). */
static void map_project(const corpus_index_t *ix, int idx, float *px, float *py) {
    double p1, p2;
    if (ix->pca_ok) {
        p1 = 0; p2 = 0;
        for (int j = 0; j < NDESC; j++) {
            double dq = ix->qn[idx * NDESC + j] - ix->pca_mean[j];
            p1 += dq * ix->pca_axis[0][j]; p2 += dq * ix->pca_axis[1][j];
        }
        float rx = ix->pca_max[0] - ix->pca_min[0], ry = ix->pca_max[1] - ix->pca_min[1];
        if (rx <= 0) rx = 1;
        if (ry <= 0) ry = 1;
        p1 = (p1 - ix->pca_min[0]) / rx; p2 = (p2 - ix->pca_min[1]) / ry;
    } else {
        p1 = ix->qn[idx * NDESC + D_CENTROID_MEAN];
        p2 = ix->qn[idx * NDESC + D_LOUD_MEAN];
    }
    if (p1 < 0) p1 = 0; else if (p1 > 1) p1 = 1;
    if (p2 < 0) p2 = 0; else if (p2 > 1) p2 = 1;
    *px = (float)p1; *py = (float)p2;
}

/* ── get_param ──────────────────────────────────────────────────────────────── */

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    concat_t *inst = (concat_t *)instance;
    if (!inst || !key) return -1;

    /* chain_params MUST be DSP-served: the module.json (-1) fallback does NOT render the
     * filepath Source picker on this device (verified 2026-09-02 — it shows a bare float
     * even after a power-cycle). Schwung >= 1.3.0 raised SHADOW_PARAM_VALUE_LEN to 64 KB
     * (was ~4 KB), so full labels + step are back and there is ample headroom. Requires the
     * Move to be on Schwung 1.3.0+. corpus_file is the filepath cell; keep module.json in sync. */
    if (strcmp(key, "chain_params") == 0) {
        int n = 0;
#define CP_EMIT(...) do { if (n < buf_len) n += snprintf(buf + n, buf_len - n, __VA_ARGS__); } while (0)
        CP_EMIT("[{\"key\":\"corpus_file\",\"name\":\"Source\",\"type\":\"filepath\","
                "\"root\":\"/data/UserData/UserLibrary\",\"filter\":[\".wav\",\".flac\",\".mp3\"]}");
        /* Corpus map as a 1.3.0 as_page: a normal knob page carrying a drawer, so
         * the encoders drive the level's knobs and pads reach the DSP. extra_keys
         * puts map_live + map_pts in the host's per-frame read rotation → drawPage
         * gets fresh values with no read on the draw path. */
        CP_EMIT(",{\"key\":\"map_view\",\"name\":\"View\",\"type\":\"canvas\",\"canvas_script\":\"canvas.js\","
                "\"as_page\":true,\"preset_browser\":true,\"show_value\":false,\"extra_keys\":[\"map_live\",\"map_pts\"]}");
        /* Recipe browser triple, named by the View level's list/count/name_param. The
         * selector (preset_index) is a browser control, never a knob cell. */
        CP_EMIT(",{\"key\":\"preset_index\",\"name\":\"Recipe\",\"type\":\"int\",\"min\":0,\"max\":%d}", NRECIPES - 1);
        CP_EMIT(",{\"key\":\"preset_count\",\"name\":\"Recipes\",\"type\":\"int\",\"min\":0,\"max\":%d,\"access\":\"read\"}", NRECIPES);
        CP_EMIT(",{\"key\":\"preset_name\",\"name\":\"Recipe\",\"type\":\"string\",\"access\":\"read\"}");
        CP_EMIT(",{\"key\":\"map_live\",\"name\":\"Live\",\"type\":\"string\",\"access\":\"read\"}");
        CP_EMIT(",{\"key\":\"map_pts\",\"name\":\"Pts\",\"type\":\"string\",\"access\":\"read\"}");
        for (int i = 0; i < NPARAMS; i++) {
            const pdef_t *p = &PARAMS[i];
            CP_EMIT(",{\"key\":\"%s\",\"name\":\"%s\",", p->key, p->label);
            if (p->type == T_ENUM) {
                CP_EMIT("\"type\":\"enum\",\"options\":[");
                for (int j = 0; j < p->nopts; j++) CP_EMIT("%s\"%s\"", j ? "," : "", p->opts[j]);
                CP_EMIT("]");
            } else if (p->type == T_INT) {
                CP_EMIT("\"type\":\"int\",\"min\":%d,\"max\":%d", (int)p->min, (int)p->max);
                if (p->unit) CP_EMIT(",\"unit\":\"%s\"", p->unit);
            } else {
                CP_EMIT("\"type\":\"float\",\"min\":%g,\"max\":%g,\"step\":%g",
                        (double)p->min, (double)p->max, (double)p->step);
                if (p->unit) CP_EMIT(",\"unit\":\"%s\"", p->unit);
            }
            if (p->viz) CP_EMIT(",\"viz\":%s", p->viz);   /* raw body: false | {group,role} */
            CP_EMIT("}");
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

    /* Canvas view feeds: cheap reads served every repaint on the SPI callback. */
    if (strcmp(key, "map_scatter") == 0) {
        corpus_index_t *ix = __atomic_load_n(&inst->index, __ATOMIC_ACQUIRE);
        return snprintf(buf, buf_len, "%s", (ix && ix->scatter_bmp[0]) ? ix->scatter_bmp : "");
    }
    if (strcmp(key, "map_live") == 0) {   /* normalized 0..1: qx,qy,qr + last N played-slice x,y pairs */
        corpus_index_t *ix = __atomic_load_n(&inst->index, __ATOMIC_ACQUIRE);
        if (!ix || ix->nslices <= 0) return snprintf(buf, buf_len, "0,0,0");
        float xp = inst->pv[P_X_POS], yp = inst->pv[P_Y_POS], r = inst->pv[P_RADIUS];
        int n = snprintf(buf, buf_len, "%.3f,%.3f,%.3f", (double)xp, (double)yp, (double)r);
        /* Trail: the last few played slices, projected onto the map plane. The
         * canvas (as_page drawPage) scales these to its band, so emit 0..1. */
        int rc = ix->query.ring_count, rh = ix->query.ring_head, cap = ix->query.ring_cap;
        for (int t = 0; t < rc && t < 8; t++) {
            int idx = ix->query.ring[(rh - 1 - t + cap * 2) % (cap ? cap : 1)];
            if (idx < 0 || idx >= ix->nslices) continue;
            float tx, ty; map_project(ix, idx, &tx, &ty);
            if (n < buf_len - 16) n += snprintf(buf + n, buf_len - n, ",%.3f,%.3f", (double)tx, (double)ty);
        }
        return n;
    }
    if (strcmp(key, "map_pts") == 0) {   /* normalized 0..1 scatter, one point per slice (strided to a cap) */
        corpus_index_t *ix = __atomic_load_n(&inst->index, __ATOMIC_ACQUIRE);
        if (!ix || ix->nslices <= 0) { if (buf_len > 0) buf[0] = 0; return 0; }
        const int CAP = 360;
        int step = (ix->nslices + CAP - 1) / CAP;
        if (step < 1) step = 1;
        int n = 0;
        for (int i = 0; i < ix->nslices; i += step) {
            if (n >= buf_len - 16) break;
            float px, py; map_project(ix, i, &px, &py);
            n += snprintf(buf + n, buf_len - n, "%s%.3f,%.3f", n ? ";" : "", (double)px, (double)py);
        }
        return n;
    }
    if (strcmp(key, "map_view") == 0) return snprintf(buf, buf_len, "map");   /* canvas cell needs a value */
    /* Recipe browser triple (list / count / name) — served every tick while browsing. */
    if (strcmp(key, "preset_index") == 0) return snprintf(buf, buf_len, "%d", inst->preset_idx);
    if (strcmp(key, "preset_count") == 0) return snprintf(buf, buf_len, "%d", NRECIPES);
    if (strcmp(key, "preset_name")  == 0) return snprintf(buf, buf_len, "%s", RECIPES[inst->preset_idx].name);
    if (strcmp(key, "_dbg") == 0) {   /* debug introspection */
        corpus_index_t *ix = __atomic_load_n(&inst->index, __ATOMIC_ACQUIRE);
        return snprintf(buf, buf_len, "started=%d arm=%d prog=%d idx=%d nslices=%d pool=%d kclusters=%d pca=%d spb=%d path=[%s]",
                        inst->analyst_started, inst->arm, inst->progress,
                        ix != NULL, ix ? ix->nslices : -1, ix ? ix->pool_len : -1,
                        ix ? ix->kclusters : -1, ix ? ix->pca_ok : -1, (int)inst->samples_per_beat, inst->corpus_path);
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
        if (n < buf_len)   /* label only: restored via preset_idx, which does not re-apply */
            n += snprintf(buf + n, buf_len - n, "preset_idx=%d\n", inst->preset_idx);
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

/* Mosaic: reduce the accumulated live-input frame to a query target in the corpus
 * descriptor space (means only; std/pitch left 0). Render-thread, no allocation. */
static void compute_mosaic_target(concat_t *inst, corpus_index_t *ix) {
    analysis_t *a = inst->mono_analysis;
    if (!a) return;
    float mag[ANALYSIS_N / 2 + 1];
    float mfcc[13];
    analysis_magnitude(a, inst->in_frame, mag);
    analysis_mfcc(a, mag, mfcc);
    float raw[NDESC];
    memset(raw, 0, sizeof(raw));
    for (int i = 0; i < 13; i++) raw[D_MFCC0_MEAN + i] = mfcc[i];
    raw[D_CENTROID_MEAN] = analysis_centroid(a, mag);
    raw[D_FLATNESS_MEAN] = analysis_flatness(a, mag);
    raw[D_ZCR_MEAN]      = analysis_zcr(inst->in_frame, ANALYSIS_N);
    raw[D_LOUD_MEAN]     = analysis_rms(inst->in_frame, ANALYSIS_N);
    concat_robust_apply(raw, ix->median, ix->iqr, inst->mosaic_t);
    for (int i = 0; i < NDESC; i++) inst->mosaic_w[i] = 0.0f;
    for (int i = 0; i < 13; i++) inst->mosaic_w[D_MFCC0_MEAN + i] = 1.0f;   /* MFCC-weighted timbre match */
    inst->mosaic_w[D_CENTROID_MEAN] = 1.0f;
    inst->mosaic_w[D_FLATNESS_MEAN] = 1.0f;
    inst->mosaic_w[D_ZCR_MEAN]      = 1.0f;
    inst->mosaic_w[D_LOUD_MEAN]     = 1.0f;
    inst->mosaic_ready = 1;
}

/* Advance an ADSR one block. stage: 0 idle, 1 atk, 2 dec, 3 sus, 4 rel. */
static void adsr_advance(int *stage, float *lvl, float atk, float dec, float sus, float rel, int frames, int sr) {
    float dt = (float)frames / sr;
    switch (*stage) {
        case 1: *lvl += dt / atk;                if (*lvl >= 1.0f) { *lvl = 1.0f; *stage = 2; } break;
        case 2: *lvl -= dt * (1.0f - sus) / dec; if (*lvl <= sus)  { *lvl = sus;  *stage = 3; } break;
        case 3: *lvl = sus; break;
        case 4: *lvl *= expf(-3.0f * dt / rel);  if (*lvl < 5e-4f) { *lvl = 0.0f; *stage = 0; } break;
        default: *lvl = 0.0f; break;
    }
}

/* Learned map: reconstruct a descriptor-space target from the 2 PCA axes at (xp,yp). */
static void pca_target(const corpus_index_t *ix, float xp, float yp, float *t, float *w) {
    float px = ix->pca_min[0] + xp * (ix->pca_max[0] - ix->pca_min[0]);
    float py = ix->pca_min[1] + yp * (ix->pca_max[1] - ix->pca_min[1]);
    for (int j = 0; j < NDESC; j++) { t[j] = ix->pca_mean[j] + px * ix->pca_axis[0][j] + py * ix->pca_axis[1][j]; w[j] = 1.0f; }
}

/* Build the query target at nav position (xp,yp): learned PCA plane or raw X/Y descriptors. */
static void nav_target(concat_t *inst, corpus_index_t *ix, int learned, float xp, float yp, float *t, float *w) {
    if (learned) pca_target(ix, xp, yp, t, w);
    else query_target_map(&ix->query, desc_index((int)inst->pv[P_X_DESC]), desc_index((int)inst->pv[P_Y_DESC]), xp, yp, t, w);
}

static void render_block(void *instance, int16_t *out_lr, int frames) {
    concat_t *inst = (concat_t *)instance;
    inst->global_samples += (uint64_t)frames;      /* drives MIDI-clock beat timing */
    corpus_index_t *ix = __atomic_load_n(&inst->index, __ATOMIC_ACQUIRE);
    if (!ix || ix->nslices == 0) { memset(out_lr, 0, sizeof(int16_t) * 2 * frames); return; }

    /* Corpus hot-swapped → flush grains before their pool pointers dangle (the analyst
     * frees the old pool ~60 ms after the swap, but grains can live up to grain_len). */
    if (ix != inst->ge_ix) { grain_flush(&inst->ge); inst->ge_ix = ix; }

    /* Repeat-avoid depth can change live. */
    query_set_repeat_avoid(&ix->query, (int)inst->pv[P_REPEAT_AVOID]);

    grain_params_t gp;
    gp.k_nearest    = (int)inst->pv[P_K_NEAREST];
    gp.radius       = inst->pv[P_RADIUS];
    gp.smooth       = inst->pv[P_SMOOTH];
    gp.density      = inst->pv[P_DENSITY];
    gp.grain_size_ms = inst->pv[P_GRAIN_SIZE];
    gp.grain_env    = (int)inst->pv[P_GRAIN_ENV];
    gp.pitch_semi   = inst->pv[P_PITCH];
    gp.pitch_jitter = inst->pv[P_PITCH_JITTER];
    gp.pan_spread   = inst->pv[P_PAN_SPREAD];
    gp.reverse_prob = inst->pv[P_REVERSE];
    gp.volume       = inst->pv[P_VOLUME];
    gp.width        = inst->pv[P_WIDTH];
    gp.target_hz    = 0.0f;
    gp.grain_rand   = inst->pv[P_GRAIN_RAND];
    gp.max_voices   = (int)inst->pv[P_POLY];

    /* Pad pressure (aftertouch) → chosen destination.
     * Grain-domain destinations here; Bright/Sat applied in the post chain below. */
    float at   = inst->aftertouch * inst->pv[P_PRESS_AMT];
    int   pdst = (int)inst->pv[P_PRESSURE];
    if (inst->aftertouch > 0.0f) switch (pdst) {
        case 1: gp.radius       = clampf(gp.radius + at, 0.0f, 1.0f); break;             /* Proximity */
        case 2: gp.density      = clampf(gp.density + at * 80.0f, 1.0f, 100.0f); break;  /* Density */
        case 5: gp.pitch_semi  += at * 12.0f; break;                                     /* Pitch (+1 oct) */
        case 6: gp.grain_size_ms = clampf(gp.grain_size_ms + at * 1000.0f, 10.0f, 2000.0f); break; /* Grain */
        case 7: gp.pitch_jitter = clampf(gp.pitch_jitter + at, 0.0f, 1.0f); break;       /* Jitter */
        default: break;
    }

    float t[NDESC], w[NDESC];
    int pm = (int)inst->pv[P_PLAY_MODE];        /* 0 Map, 1 Notes, 2 Mosaic */
    int nmatch = (int)inst->pv[P_NOTE_MATCH];   /* 0 Off, 1 Pitch, 2 Zone */
    int learned = (int)inst->pv[P_MAP_MODE] == 1 && ix->pca_ok;   /* Learned PCA nav axes */
    int any_held = 0;
    int16_t *dry_in = NULL;                     /* live input for Dry Mix (Mosaic only) */

    /* Map LFO — Follow=LFO (free, Hz) or Follow=Sequence (tempo-locked: LFO Rate =
     * cycles per beat, driven by the incoming MIDI clock / 120 BPM fallback). */
    float xpos = inst->pv[P_X_POS], ypos = inst->pv[P_Y_POS];
    int followm = (int)inst->pv[P_FOLLOW];
    if (followm == 1 || followm == 2) {
        float spb = inst->samples_per_beat > 100.0f ? inst->samples_per_beat : 22050.0f;
        float adv = (followm == 2) ? inst->pv[P_LFO_RATE] * (float)frames / spb          /* cycles/beat */
                                   : inst->pv[P_LFO_RATE] * (float)frames / 44100.0f;     /* Hz */
        inst->lfo_phase += adv;
        if (inst->lfo_phase >= 1.0f) {
            inst->lfo_phase -= (float)(int)inst->lfo_phase;
            inst->lfo_rng = inst->lfo_rng * 1664525u + 1013904223u;   /* new Sample&Hold point each cycle */
            inst->lfo_shx = (inst->lfo_rng >> 8) * (1.0f / 16777216.0f);
            inst->lfo_rng = inst->lfo_rng * 1664525u + 1013904223u;
            inst->lfo_shy = (inst->lfo_rng >> 8) * (1.0f / 16777216.0f);
        }
        float ph = inst->lfo_phase * 6.2831853f;
        const float depth = 0.45f;
        switch ((int)inst->pv[P_LFO_SHAPE]) {
            case 0: xpos += depth * cosf(ph); ypos += depth * sinf(ph); break;   /* Circle */
            case 1: xpos += depth * sinf(ph); break;                             /* Sine X */
            case 2: ypos += depth * sinf(ph); break;                             /* Sine Y */
            default: xpos = inst->lfo_shx; ypos = inst->lfo_shy; break;          /* Sample & Hold */
        }
        xpos = clampf(xpos, 0.0f, 1.0f); ypos = clampf(ypos, 0.0f, 1.0f);
    }

#define ENVT(v) (0.002f + (v) * (v) * 5.0f)     /* 0..1 knob → 2 ms .. 5 s */

    if (pm == 1) {                              /* Notes: polyphonic — one voice per held pad */
        for (int i = 0; i < MAX_NOTES; i++) {
            note_voice_t *nv = &inst->notes[i];
            if (!nv->active) continue;
            if (nv->held) any_held = 1;
            adsr_advance(&nv->amp_stage, &nv->amp_level,
                         ENVT(inst->pv[P_AMP_ATK]), ENVT(inst->pv[P_AMP_DEC]),
                         inst->pv[P_AMP_SUS], ENVT(inst->pv[P_AMP_REL]), frames, 44100);
            if (!nv->held && nv->amp_stage == 0) { nv->active = 0; continue; }  /* released + silent → free */

            float note_hz = 440.0f * powf(2.0f, (nv->note - 69) / 12.0f)
                                   * powf(2.0f, inst->pitch_bend / 12.0f);
            float target_hz = 0.0f;
            memset(t, 0, sizeof(float) * NDESC);
            memset(w, 0, sizeof(float) * NDESC);
            if (nmatch == 1) {                   /* Pitch: nearest-pitch slice + retune to the note */
                if (ix->iqr[D_PITCH] > 1e-6f) {
                    t[D_PITCH] = (note_hz - ix->median[D_PITCH]) / ix->iqr[D_PITCH];
                    w[D_PITCH] = 1.0f;
                }
                target_hz = note_hz;
            } else if (nmatch == 2) {            /* Zone: note → map X, natural pitch (pads play different parts) */
                float zx = clampf((nv->note - 36) / 60.0f, 0.0f, 1.0f);
                nav_target(inst, ix, learned, zx, ypos, t, w);
            } else {                             /* Off: current map point, retuned chromatically */
                nav_target(inst, ix, learned, xpos, ypos, t, w);
                target_hz = note_hz;
            }
            /* Velocity Amount: 0 = velocity ignored (always full), 1 = velocity fully scales gain.
             * The amp envelope is passed LIVE (&nv->amp_level) so this note's grains fade together
             * on release — even long grains — instead of playing out at their spawn gain. */
            float ev = 1.0f - inst->pv[P_VEL_AMT] * (1.0f - nv->vel);
            grain_amp_t ga = { &nv->amp_level, &nv->gen, nv->gen };
            grain_schedule(&inst->ge, ix, t, w, &gp, ev, &ga, target_hz, &nv->accum, frames, 44100);
        }
    } else if (pm == 2 && inst->host && inst->mono_analysis) {   /* Mosaic: target from live line/mic input */
        int16_t *ain = (int16_t *)(inst->host->mapped_memory + inst->host->audio_in_offset);
        dry_in = ain;                                            /* expose to Dry Mix */
        for (int i = 0; i < frames; i++) {
            float mono = (ain[i * 2] + ain[i * 2 + 1]) * (0.5f / 32768.0f);
            if (inst->in_fill < ANALYSIS_N) inst->in_frame[inst->in_fill++] = mono;
            if (inst->in_fill >= ANALYSIS_N) { compute_mosaic_target(inst, ix); inst->in_fill = 0; }
        }
        if (inst->mosaic_ready) { memcpy(t, inst->mosaic_t, sizeof(float) * NDESC); memcpy(w, inst->mosaic_w, sizeof(float) * NDESC); }
        else nav_target(inst, ix, learned, xpos, ypos, t, w);
        grain_schedule(&inst->ge, ix, t, w, &gp, 1.0f, NULL, 0.0f, &inst->ge.sched_accum, frames, 44100);
    } else {                                     /* Map: X/Y navigation, or pad → k-means cluster jump */
        if (inst->pads_held > 0 && ix->kclusters > 0) {
            int c = ((inst->last_pad_note % ix->kclusters) + ix->kclusters) % ix->kclusters;
            const float *cen = &ix->centroids[c * NDESC];
            for (int j = 0; j < NDESC; j++) { t[j] = cen[j]; w[j] = 1.0f; }   /* full-space nearest to cluster centre */
        } else {
            nav_target(inst, ix, learned, xpos, ypos, t, w);
        }
        grain_schedule(&inst->ge, ix, t, w, &gp, 1.0f, NULL, 0.0f, &inst->ge.sched_accum, frames, 44100);
    }

    grain_mix(&inst->ge, &gp, out_lr, frames, 44100);   /* per-note gains already baked into grains */

    /* ── Filter envelope (Shape) — gated by held notes in Notes, else held open ── */
    int fgate = (pm == 1) ? any_held : 1;
    if (fgate && !inst->last_gate) inst->filt_stage = 1;
    if (!fgate && inst->last_gate) inst->filt_stage = 4;
    inst->last_gate = fgate;
    adsr_advance(&inst->filt_stage, &inst->filt_level, ENVT(inst->pv[P_FILT_ATK]), ENVT(inst->pv[P_FILT_DEC]),
                 inst->pv[P_FILT_SUS], ENVT(inst->pv[P_AMP_REL]), frames, 44100);
#undef ENVT

    /* ── DJ filter: one bipolar knob (center bypass, left LP down, right HP up) ── */
    float cutpos = inst->pv[P_CUTOFF];
    cutpos += inst->filt_level * (inst->pv[P_FILT_AMT] * 2.0f - 1.0f);   /* filter envelope (bipolar) */
    if (pdst == 3) cutpos += at;                                        /* pressure → Bright */
    cutpos = clampf(cutpos, 0.0f, 1.0f);
    int fmode; float fc = 0.0f;
    if (cutpos < 0.48f)      { fmode = 1; fc = 20000.0f * powf(0.005f, (0.5f - cutpos) / 0.5f); }  /* LP 20k→100 */
    else if (cutpos > 0.52f) { fmode = 2; fc = 20.0f    * powf(400.0f, (cutpos - 0.5f) / 0.5f); }  /* HP 20→8k */
    else                       fmode = 0;                                                          /* bypass */

    float sat = inst->pv[P_SAT];
    if (pdst == 4) sat = clampf(sat + at, 0.0f, 1.0f);

    float g  = fmode ? tanf((float)M_PI * fc / SAMPLE_RATE) : 0.0f;
    float kf = 2.0f - 1.9f * inst->pv[P_RESO];
    float a1 = 1.0f / (1.0f + g * (g + kf)), a2 = g * a1, a3 = g * a2;
    float drive = 1.0f + sat * 3.0f, makeup = 1.0f / drive;
    float dry = dry_in ? inst->pv[P_DRY_MIX] : 0.0f;   /* Dry Mix: blend live input (Mosaic) */
    for (int i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            float x = out_lr[i * 2 + ch] * (1.0f / 32768.0f);
            if (fmode != 0) {
                float v3 = x - inst->svf_ic2[ch];
                float v1 = a1 * inst->svf_ic1[ch] + a2 * v3;
                float v2 = inst->svf_ic2[ch] + a2 * inst->svf_ic1[ch] + a3 * v3;
                inst->svf_ic1[ch] = 2.0f * v1 - inst->svf_ic1[ch] + 1e-20f;    /* denormal guard */
                inst->svf_ic2[ch] = 2.0f * v2 - inst->svf_ic2[ch] + 1e-20f;
                x = (fmode == 1) ? v2 : (x - kf * v1 - v2);    /* LP : HP */
            }
            if (sat > 0.001f) x = tanhf(x * drive) * makeup;
            if (dry > 0.0f) x = x * (1.0f - dry) + dry_in[i * 2 + ch] * (1.0f / 32768.0f) * dry;
            out_lr[i * 2 + ch] = (int16_t)(clampf(x, -1.0f, 1.0f) * 32767.0f);
        }
    }
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
    g_host = (const concat_host_t *)host;   /* for Mosaic live-input reads */
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
