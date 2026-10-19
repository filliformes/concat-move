/**
 * Concat — audio ingest (clean-room MIT). See ingest.h. Offline only.
 */
#include "ingest.h"
#include "corpus.h"     /* concat_cancelled */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

/* dr_libs implementations (public domain / MIT-0). Quiet their warnings under -Wextra. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#define DR_WAV_IMPLEMENTATION
#include "vendor/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "vendor/dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_mp3.h"
#pragma GCC diagnostic pop

static const char *ROOTS[] = {
    "/data/UserData/UserLibrary/Samples",
    "/data/UserData/UserLibrary/Recordings",
    "/data/UserData/breakbeat-samples",
    "/data/UserData/UserLibrary/Magneto Recs",
    "/data/UserData/schwung/corpora",
    "/data/UserData/schwung/recordings",
};
#define NROOTS ((int)(sizeof(ROOTS) / sizeof(ROOTS[0])))

int ingest_is_audio(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) return 0;
    return !strcasecmp(e, ".wav") || !strcasecmp(e, ".flac") || !strcasecmp(e, ".mp3") ||
           !strcasecmp(e, ".aif") || !strcasecmp(e, ".aiff");          /* AIFF via dr_wav */
}
#define is_audio ingest_is_audio

static int is_dir_entry(const char *parent, struct dirent *e) {
#ifdef DT_DIR
    if (e->d_type == DT_DIR) return 1;
    if (e->d_type == DT_REG) return 0;
    /* DT_LNK / DT_UNKNOWN → stat() (follows symlinks, e.g. breakbeat-samples/Built-in) */
#endif
    char p[INGEST_PATH]; struct stat st;
    snprintf(p, sizeof(p), "%s/%s", parent, e->d_name);
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int count_audio(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int c = 0; struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.' && is_audio(e->d_name)) c++;
    closedir(d);
    return c;
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void add_folder(corpus_folder_t *out, int *n, int max, const char *name, const char *path, int nfiles) {
    if (*n >= max) return;
    strncpy(out[*n].name, name, sizeof(out[*n].name) - 1); out[*n].name[sizeof(out[*n].name) - 1] = 0;
    strncpy(out[*n].path, path, sizeof(out[*n].path) - 1); out[*n].path[sizeof(out[*n].path) - 1] = 0;
    out[*n].nfiles = nfiles;
    (*n)++;
}

int ingest_scan_folders(corpus_folder_t *out, int max) {
    int n = 0;
    for (int r = 0; r < NROOTS && n < max; r++) {
        int rc = count_audio(ROOTS[r]);
        if (rc > 0) add_folder(out, &n, max, basename_of(ROOTS[r]), ROOTS[r], rc);
        DIR *d = opendir(ROOTS[r]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && n < max) {
            if (e->d_name[0] == '.') continue;
            if (!is_dir_entry(ROOTS[r], e)) continue;
            char child[INGEST_PATH];
            snprintf(child, sizeof(child), "%s/%s", ROOTS[r], e->d_name);
            int cc = count_audio(child);
            if (cc > 0) add_folder(out, &n, max, e->d_name, child, cc);
        }
        closedir(d);
    }
    return n;
}

/* Recurse a folder (bounded depth) collecting audio files into paths[]. Depth cap
 * bounds symlink loops; the file cap (`max`) bounds total. Subfolders first, then
 * files, so nested sample libraries are picked up (a whole tree as one corpus). */
static void scan_audio(const char *dir, char (*paths)[INGEST_PATH], int max, int *n, int depth) {
    if (*n >= max || depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *n < max) {
        if (e->d_name[0] == '.') continue;
        char full[INGEST_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (is_dir_entry(dir, e))       scan_audio(full, paths, max, n, depth + 1);
        else if (is_audio(e->d_name)) { snprintf(paths[*n], INGEST_PATH, "%s", full); (*n)++; }
    }
    closedir(d);
}

int ingest_list_audio(const char *dir, char (*paths)[INGEST_PATH], int max) {
    struct stat st;
    if (max > 0 && stat(dir, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(paths[0], INGEST_PATH, "%s", dir);   /* a single FILE is the whole corpus */
        return 1;
    }
    int n = 0;
    scan_audio(dir, paths, max, &n, 0);
    return n;
}



/* ── Streaming reader ─────────────────────────────────────────────────────────
 * Decodes CHUNK source frames at a time, downmixes to mono, and resamples to the
 * target rate with a stateful cubic Hermite (same kernel as the old whole-buffer
 * resampler, so descriptors and cache signatures are unchanged in practice).
 * Memory is O(chunk), independent of file length. */

#define RD_CHUNK 4096

enum { RD_WAV = 0, RD_FLAC = 1, RD_MP3 = 2 };

struct ingest_reader {
    int        kind;
    drwav      wav;
    drflac    *flac;
    drmp3      mp3;
    unsigned   ch, sr;
    unsigned long long total;    /* source frames, 0 = unknown */
    int        target_sr;
    double     step;             /* source frames per output frame */
    float     *ibuf;             /* interleaved decode chunk [RD_CHUNK * ch] */
    float     *mono;             /* mono source window */
    int        mono_cap, mono_n;
    long long  mono_base;        /* absolute source index of mono[0] */
    long long  src_read;         /* source frames decoded so far */
    double     opos;             /* absolute source position of the next output frame */
    int        eof;
};

static unsigned long long rd_decode(ingest_reader_t *r, float *buf, unsigned long long n) {
    switch (r->kind) {
        case RD_FLAC: return drflac_read_pcm_frames_f32(r->flac, n, buf);
        case RD_MP3:  return drmp3_read_pcm_frames_f32(&r->mp3, n, buf);
        default:      return drwav_read_pcm_frames_f32(&r->wav, n, buf);
    }
}

/* Drop source samples before `keep_from` (absolute), then decode one chunk. */
static void rd_refill(ingest_reader_t *r, long long keep_from) {
    if (keep_from > r->mono_base) {
        long long drop = keep_from - r->mono_base;
        if (drop > r->mono_n) drop = r->mono_n;
        memmove(r->mono, r->mono + drop, sizeof(float) * (size_t)(r->mono_n - drop));
        r->mono_n -= (int)drop;
        r->mono_base += drop;
    }
    int room = r->mono_cap - r->mono_n;
    if (room > RD_CHUNK) room = RD_CHUNK;
    if (room <= 0 || r->eof) return;
    unsigned long long got = rd_decode(r, r->ibuf, (unsigned long long)room);
    if (got == 0) { r->eof = 1; return; }
    float inv = 1.0f / (float)r->ch;
    for (unsigned long long i = 0; i < got; i++) {
        float s = 0.0f;
        for (unsigned c = 0; c < r->ch; c++) s += r->ibuf[i * r->ch + c];
        r->mono[r->mono_n + (int)i] = s * inv;
    }
    r->mono_n += (int)got;
    r->src_read += (long long)got;
}

ingest_reader_t *ingest_open(const char *path, int target_sr) {
    if (!path || target_sr <= 0) return NULL;
    ingest_reader_t *r = (ingest_reader_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    const char *ext = strrchr(path, '.');
    int ok = 0;
    if (ext && !strcasecmp(ext, ".flac")) {
        r->flac = drflac_open_file(path, NULL);
        if (r->flac) { ok = 1; r->kind = RD_FLAC; r->ch = r->flac->channels; r->sr = r->flac->sampleRate;
                       r->total = r->flac->totalPCMFrameCount; }
    } else if (ext && !strcasecmp(ext, ".mp3")) {
        if (drmp3_init_file(&r->mp3, path, NULL)) {
            ok = 1; r->kind = RD_MP3; r->ch = r->mp3.channels; r->sr = r->mp3.sampleRate; r->total = 0;
        }
    }
    if (!ok && drwav_init_file(&r->wav, path, NULL)) {      /* WAV, AIFF, RF64, W64; fallback for odd extensions */
        ok = 1; r->kind = RD_WAV; r->ch = r->wav.channels; r->sr = r->wav.sampleRate;
        r->total = r->wav.totalPCMFrameCount;
    }
    if (!ok || r->ch == 0 || r->ch > 32 || r->sr == 0) { if (ok) ingest_close(r); else free(r); return NULL; }

    r->target_sr = target_sr;
    r->step = (double)r->sr / (double)target_sr;
    r->mono_cap = RD_CHUNK + 8;
    r->ibuf = (float *)malloc(sizeof(float) * (size_t)RD_CHUNK * r->ch);
    r->mono = (float *)malloc(sizeof(float) * (size_t)r->mono_cap);
    if (!r->ibuf || !r->mono) { ingest_close(r); return NULL; }
    return r;
}

int ingest_read(ingest_reader_t *r, float *out, int max_frames) {
    if (!r || max_frames <= 0) return 0;
    int w = 0;
    if (r->sr == (unsigned)r->target_sr) {                  /* same rate: plain copy-through */
        while (w < max_frames) {
            if (r->mono_n == 0) { if (r->eof || concat_cancelled()) break; rd_refill(r, r->mono_base); continue; }
            int n = r->mono_n < max_frames - w ? r->mono_n : max_frames - w;
            memcpy(out + w, r->mono, sizeof(float) * (size_t)n);
            w += n;
            rd_refill(r, r->mono_base + n);                  /* drops consumed; refills only if empty-ish */
        }
        return w;
    }
    while (w < max_frames) {
        long long idx = (long long)r->opos;
        if (!r->eof && idx + 2 >= r->mono_base + r->mono_n) {
            if (concat_cancelled()) break;                     /* per 4096-frame refill */
            rd_refill(r, idx - 1); continue;
        }
        if (r->eof && r->opos >= (double)r->src_read) break;   /* n_out = floor(n_in * ratio) */
        long long last = r->src_read - 1;
        if (last < 0) break;
        long long i0 = idx - 1, i1 = idx, i2 = idx + 1, i3 = idx + 2;
        if (i0 < 0) i0 = 0;
        if (i1 > last) i1 = last;
        if (i2 > last) i2 = last;
        if (i3 > last) i3 = last;
        if (i0 > last) i0 = last;
        float y0 = r->mono[i0 - r->mono_base], y1 = r->mono[i1 - r->mono_base];
        float y2 = r->mono[i2 - r->mono_base], y3 = r->mono[i3 - r->mono_base];
        float fr = (float)(r->opos - (double)idx);
        float c1 = 0.5f * (y2 - y0);
        float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        out[w++] = ((c3 * fr + c2) * fr + c1) * fr + y1;
        r->opos += r->step;
    }
    return w;
}

double ingest_duration_s(const ingest_reader_t *r) {
    return (r && r->sr && r->total) ? (double)r->total / (double)r->sr : 0.0;
}

void ingest_close(ingest_reader_t *r) {
    if (!r) return;
    switch (r->kind) {
        case RD_FLAC: if (r->flac) drflac_close(r->flac); break;
        case RD_MP3:  drmp3_uninit(&r->mp3); break;
        default:      drwav_uninit(&r->wav); break;
    }
    free(r->ibuf); free(r->mono); free(r);
}

float *ingest_decode(const char *path, int target_sr, int *out_len) {
    *out_len = 0;
    ingest_reader_t *r = ingest_open(path, target_sr);
    if (!r) return NULL;
    int cap = 1 << 16, n = 0;
    float *buf = (float *)malloc(sizeof(float) * (size_t)cap);
    while (buf) {
        if (cap - n < 4096) {
            if (cap > (1 << 29)) break;                       /* ~12 min @ 44.1k per GB of floats: stop growing */
            float *nb = (float *)realloc(buf, sizeof(float) * (size_t)cap * 2);
            if (!nb) break;
            buf = nb; cap *= 2;
        }
        int got = ingest_read(r, buf + n, cap - n);
        if (got <= 0) break;
        n += got;
    }
    ingest_close(r);
    if (!buf || n == 0) { free(buf); return NULL; }
    *out_len = n;
    return buf;
}
