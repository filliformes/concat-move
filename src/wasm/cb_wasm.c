/**
 * Concat — Corpus Builder analysis core, compiled to WebAssembly (clean-room, MIT).
 *
 * The browser runs the SAME decode (dr_libs + Concat's cubic resampler), slicing and
 * descriptor code as the Move, so the page's statistics and map show what Concat
 * will compute. The per-item loop mirrors corpus_index_build: seek to the range
 * start, read CONCAT_WIN_SECONDS windows, slice each window, describe each slice.
 *
 * Usage from JS (one file at a time):
 *   p = cb_alloc(size); HEAPU8.set(bytes, p); cb_load(p, size, name)   // takes ownership
 *   n = cb_decode()      -> mono 44.1k frames of the whole file, at cb_audio()
 *   k = cb_analyse(start_s, end_s, mode, min_ms, max_ms, gate)
 *       -> slices at cb_slices() [start,len pairs, frames from the range start]
 *          descriptors at cb_desc() [k * cb_ndesc() floats, raw (unscaled)]
 *   cb_unload()
 */
#include <emscripten/emscripten.h>
#include <stdlib.h>
#include <string.h>
#include "ingest.h"
#include "corpus.h"
#include "corpus_engine.h"   /* CONCAT_WIN_SECONDS */
#include "analysis.h"

#define SR 44100
#define MAX_SLICES_WIN 4096  /* = MAX_SLICES_FILE in corpus_engine.c */

static uint8_t *g_data;  static size_t g_size;  static char g_name[256];
static float   *g_audio; static int g_audio_n;
static int     *g_slices; static float *g_desc; static int g_n, g_cap;
static analysis_t *g_an;

EMSCRIPTEN_KEEPALIVE void *cb_alloc(int size) { return malloc((size_t)size); }
EMSCRIPTEN_KEEPALIVE void  cb_free(void *p)   { free(p); }
EMSCRIPTEN_KEEPALIVE int   cb_ndesc(void)     { return NDESC; }
EMSCRIPTEN_KEEPALIVE int   cb_win_seconds(void) { return CONCAT_WIN_SECONDS; }

EMSCRIPTEN_KEEPALIVE void cb_unload(void) {
    free(g_data); g_data = NULL; g_size = 0;
    free(g_audio); g_audio = NULL; g_audio_n = 0;
}

/* Take ownership of a buffer from cb_alloc holding the file's bytes. */
EMSCRIPTEN_KEEPALIVE int cb_load(uint8_t *data, int size, const char *name) {
    cb_unload();
    g_data = data; g_size = (size_t)size;
    strncpy(g_name, name ? name : "", sizeof(g_name) - 1); g_name[sizeof(g_name) - 1] = 0;
    ingest_reader_t *r = ingest_open_memory(g_data, g_size, g_name, SR);
    if (!r) return 0;
    ingest_close(r);
    return 1;
}

/* Whole file as mono 44.1k (waveform + audition). Returns frames, or -1. */
EMSCRIPTEN_KEEPALIVE int cb_decode(void) {
    free(g_audio); g_audio = NULL; g_audio_n = 0;
    ingest_reader_t *r = ingest_open_memory(g_data, g_size, g_name, SR);
    if (!r) return -1;
    int cap = 1 << 16, n = 0;
    float *buf = (float *)malloc(sizeof(float) * (size_t)cap);
    while (buf) {
        if (cap - n < 8192) {
            float *nb = (float *)realloc(buf, sizeof(float) * (size_t)cap * 2);
            if (!nb) break;
            buf = nb; cap *= 2;
        }
        int got = ingest_read(r, buf + n, cap - n);
        if (got <= 0) break;
        n += got;
    }
    ingest_close(r);
    if (!buf) return -1;
    g_audio = buf; g_audio_n = n;
    return n;
}
EMSCRIPTEN_KEEPALIVE float *cb_audio(void) { return g_audio; }

static int push_slice(int start, int len, const float *src, int slen) {
    if (g_n >= g_cap) {
        int nc = g_cap ? g_cap * 2 : 1024;
        int *ns = (int *)realloc(g_slices, sizeof(int) * 2 * (size_t)nc);
        if (!ns) return 0;
        g_slices = ns;
        float *nd = (float *)realloc(g_desc, sizeof(float) * NDESC * (size_t)nc);
        if (!nd) return 0;
        g_desc = nd;
        g_cap = nc;
    }
    g_slices[2 * g_n] = start; g_slices[2 * g_n + 1] = len;
    concat_slice_descriptor(g_an, src, slen, &g_desc[(size_t)g_n * NDESC]);
    g_n++;
    return 1;
}

/* Slice + describe a range exactly as the Move's build does. Returns the slice count,
 * or -1 if the file cannot be decoded. end_s <= start_s = to the end. */
EMSCRIPTEN_KEEPALIVE int cb_analyse(double start_s, double end_s, int slice_mode, int min_ms, int max_ms, float gate) {
    g_n = 0;
    if (!g_an) g_an = analysis_create(SR, 1024, 40, 13);
    ingest_reader_t *r = ingest_open_memory(g_data, g_size, g_name, SR);
    if (!r || !g_an) { if (r) ingest_close(r); return -1; }
    int min_len = (int)((long)min_ms * SR / 1000), max_len = (int)((long)max_ms * SR / 1000);
    int win_cap = SR * CONCAT_WIN_SECONDS;
    float *win = (float *)malloc(sizeof(float) * (size_t)win_cap);
    slice_range_t *ranges = (slice_range_t *)malloc(sizeof(slice_range_t) * MAX_SLICES_WIN);
    if (!win || !ranges) { free(win); free(ranges); ingest_close(r); return -1; }

    if (start_s > 0.0) {
        long long st = (long long)(start_s * SR);
        if (!ingest_seek(r, st))
            while (st > 0) {
                int got = ingest_read(r, win, st < win_cap ? (int)st : win_cap);
                if (got <= 0) break;
                st -= got;
            }
    }
    long long remain = end_s > start_s ? (long long)((end_s - start_s) * SR) : -1;
    long long base = 0;
    for (;;) {
        int want = win_cap;
        if (remain >= 0 && remain < want) want = (int)remain;
        if (want <= 0) break;
        int len = ingest_read(r, win, want);
        if (len <= 0) break;
        if (remain >= 0) remain -= len;
        int nr = (slice_mode == 1)
            ? concat_fixed_slice(g_an, win, len, max_len, gate, ranges, MAX_SLICES_WIN)
            : concat_onset_slice(g_an, win, len, min_len, max_len, gate, ranges, MAX_SLICES_WIN);
        for (int s = 0; s < nr; s++)
            if (!push_slice((int)(base + ranges[s].start), ranges[s].len, &win[ranges[s].start], ranges[s].len)) break;
        base += len;
    }
    free(win); free(ranges);
    ingest_close(r);
    return g_n;
}
EMSCRIPTEN_KEEPALIVE int   *cb_slices(void) { return g_slices; }
EMSCRIPTEN_KEEPALIVE float *cb_desc(void)   { return g_desc; }
