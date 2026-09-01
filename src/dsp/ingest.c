/**
 * Concat — audio ingest (clean-room MIT). See ingest.h. Offline only.
 */
#include "ingest.h"
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

static int is_audio(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) return 0;
    return !strcasecmp(e, ".wav") || !strcasecmp(e, ".flac") || !strcasecmp(e, ".mp3");
}

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

int ingest_list_audio(const char *dir, char (*paths)[INGEST_PATH], int max) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.' || !is_audio(e->d_name)) continue;
        snprintf(paths[n], INGEST_PATH, "%s/%s", dir, e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

/* Cubic Hermite resample of a mono buffer. */
static float *resample_mono(const float *in, int in_len, int in_sr, int out_sr, int *out_len) {
    if (in_len <= 0) { *out_len = 0; return NULL; }
    if (in_sr == out_sr) {
        float *o = (float *)malloc((size_t)in_len * sizeof(float));
        if (o) memcpy(o, in, (size_t)in_len * sizeof(float));
        *out_len = o ? in_len : 0; return o;
    }
    double ratio = (double)out_sr / in_sr;
    int n = (int)(in_len * ratio);
    if (n < 1) n = 1;
    float *o = (float *)malloc((size_t)n * sizeof(float));
    if (!o) { *out_len = 0; return NULL; }
    for (int i = 0; i < n; i++) {
        double sp = i / ratio;
        int idx = (int)sp; double fr = sp - idx;
        float y0 = in[idx > 0 ? idx - 1 : 0];
        float y1 = in[idx < in_len ? idx : in_len - 1];
        float y2 = in[idx + 1 < in_len ? idx + 1 : in_len - 1];
        float y3 = in[idx + 2 < in_len ? idx + 2 : in_len - 1];
        float c0 = y1;
        float c1 = 0.5f * (y2 - y0);
        float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        o[i] = ((c3 * (float)fr + c2) * (float)fr + c1) * (float)fr + c0;
    }
    *out_len = n; return o;
}

float *ingest_decode(const char *path, int target_sr, int *out_len) {
    *out_len = 0;
    const char *ext = strrchr(path, '.');
    unsigned int ch = 0, sr = 0;
    drwav_uint64 frames = 0;
    float *pcm = NULL;

    if (ext && !strcasecmp(ext, ".flac")) {
        pcm = drflac_open_file_and_read_pcm_frames_f32(path, &ch, &sr, &frames, NULL);
    } else if (ext && !strcasecmp(ext, ".mp3")) {
        drmp3_config cfg;
        pcm = drmp3_open_file_and_read_pcm_frames_f32(path, &cfg, &frames, NULL);
        if (pcm) { ch = cfg.channels; sr = cfg.sampleRate; }
    } else {
        pcm = drwav_open_file_and_read_pcm_frames_f32(path, &ch, &sr, &frames, NULL);
    }
    if (!pcm) {   /* fallback: try WAV regardless of extension */
        pcm = drwav_open_file_and_read_pcm_frames_f32(path, &ch, &sr, &frames, NULL);
    }
    if (!pcm || frames == 0 || ch == 0 || sr == 0) { free(pcm); return NULL; }

    int nf = (int)frames;
    float *mono = (float *)malloc((size_t)nf * sizeof(float));
    if (!mono) { free(pcm); return NULL; }
    for (int i = 0; i < nf; i++) {
        float s = 0.0f;
        for (unsigned int c = 0; c < ch; c++) s += pcm[(size_t)i * ch + c];
        mono[i] = s / (float)ch;
    }
    free(pcm);

    float *out = resample_mono(mono, nf, (int)sr, target_sr, out_len);
    free(mono);
    return out;
}
