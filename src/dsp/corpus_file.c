/**
 * Concat — corpus sources (clean-room, first-party MIT). See corpus_file.h.
 */
#define _GNU_SOURCE     /* FNM_CASEFOLD */
#include "corpus_file.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fnmatch.h>
#include <sys/stat.h>

#define CF_MAX_BYTES (1 << 20)          /* a .concat file is a few KB; refuse silly sizes */

int corpus_is_concat(const char *path) {
    const char *e = path ? strrchr(path, '.') : NULL;
    return e && !strcasecmp(e, ".concat");
}

static void set_pair(float *mask, int mean_idx, int std_idx) {
    mask[mean_idx] = 1.0f;
    if (std_idx >= 0) mask[std_idx] = 1.0f;
}

int corpus_desc_group(const char *name, float *mask) {
    if (!name) return 0;
    if (!strcasecmp(name, "loud"))     { set_pair(mask, D_LOUD_MEAN, D_LOUD_PEAK); return 1; }
    if (!strcasecmp(name, "centroid")) { set_pair(mask, D_CENTROID_MEAN, D_CENTROID_STD); return 1; }
    if (!strcasecmp(name, "flatness")) { set_pair(mask, D_FLATNESS_MEAN, D_FLATNESS_STD); return 1; }
    if (!strcasecmp(name, "zcr"))      { set_pair(mask, D_ZCR_MEAN, D_ZCR_STD); return 1; }
    if (!strcasecmp(name, "pitch"))    { set_pair(mask, D_PITCH, D_PITCH_CONF); return 1; }
    if (!strncasecmp(name, "mfcc", 4)) {
        int a = 1, b = 13;
        const char *r = name + 4;
        if (*r) {
            char *end;
            a = (int)strtol(r, &end, 10);
            if (end == r) return 0;
            b = a;
            if (*end == '-') { const char *r2 = end + 1; b = (int)strtol(r2, &end, 10); if (end == r2) return 0; }
            if (*end) return 0;
        }
        if (a < 1 || b > 13 || a > b) return 0;
        for (int k = a; k <= b; k++) set_pair(mask, D_MFCC0_MEAN + k - 1, D_MFCC0_STD + k - 1);
        return 1;
    }
    return 0;
}

static void add_watch(corpus_spec_t *spec, const char *path) {
    if (spec->nwatch >= CF_MAX_WATCH) return;
    snprintf(spec->watch[spec->nwatch++], INGEST_PATH, "%s", path);
}

unsigned long long corpus_watch_stamp(const corpus_spec_t *spec) {
    unsigned long long h = 0xcbf29ce484222325ULL;
    int any = 0;
    for (int i = 0; i < spec->nwatch; i++) {
        struct stat st;
        long long v[3] = { -1, -1, -1 };
        if (stat(spec->watch[i], &st) == 0) {            /* ns mtime: two saves in one second differ */
            v[0] = (long long)st.st_mtim.tv_sec; v[1] = (long long)st.st_mtim.tv_nsec; v[2] = (long long)st.st_size;
            any = 1;
        }
        const unsigned char *b = (const unsigned char *)v;
        for (size_t k = 0; k < sizeof(v); k++) { h ^= b[k]; h *= 0x100000001b3ULL; }
    }
    return any ? (h ? h : 1) : 0;
}

/* Glob list (array of strings) vs a file name. */
static int any_match(const jv_t *globs, const char *name) {
    if (!globs || globs->type != JV_ARR) return 0;
    for (const jv_t *g = globs->child; g; g = g->next)
        if (g->type == JV_STR && fnmatch(g->str, name, FNM_CASEFOLD) == 0) return 1;
    return 0;
}

static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static char *read_text(const char *path, char *err, int errlen) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, errlen, "can't open corpus file"); return NULL; }
    char *buf = (char *)malloc(CF_MAX_BYTES + 1);
    if (!buf) { fclose(fp); snprintf(err, errlen, "out of memory"); return NULL; }
    size_t n = fread(buf, 1, CF_MAX_BYTES + 1, fp);
    fclose(fp);
    if (n > CF_MAX_BYTES) { free(buf); snprintf(err, errlen, "corpus file too big"); return NULL; }
    buf[n] = 0;
    return buf;
}

static int resolve_concat(const char *path, corpus_spec_t *spec, corpus_item_t *items, int max) {
    spec->is_concat = 1;
    add_watch(spec, path);
    char *text = read_text(path, spec->err, sizeof(spec->err));
    if (!text) return -1;
    char jerr[64];
    jv_t *root = json_parse(text, jerr, sizeof(jerr));
    free(text);
    if (!root) { snprintf(spec->err, sizeof(spec->err), "bad JSON: %s", jerr); return -1; }
    if (root->type != JV_OBJ) { json_free(root); snprintf(spec->err, sizeof(spec->err), "not a corpus file"); return -1; }
    int ver = (int)json_num(json_get(root, "concat"), 1);
    if (ver != 1) { json_free(root); snprintf(spec->err, sizeof(spec->err), "corpus file version %d", ver); return -1; }

    snprintf(spec->name, sizeof(spec->name), "%s", json_str(json_get(root, "name"), base_name(path)));

    const jv_t *sl = json_get(root, "slicing");
    if (sl && sl->type == JV_OBJ) {
        spec->has_slicing = 1;
        const char *m = json_str(json_get(sl, "mode"), "onset");
        spec->slice_mode = (!strcasecmp(m, "fixed") || !strcasecmp(m, "grain")) ? 1 : 0;
        spec->min_ms = (int)json_num(json_get(sl, "min_ms"), 60);
        spec->max_ms = (int)json_num(json_get(sl, "max_ms"), 400);
        if (spec->min_ms < 5) spec->min_ms = 5;
        if (spec->max_ms < spec->min_ms) spec->max_ms = spec->min_ms;
        if (spec->max_ms > 2000) spec->max_ms = 2000;
        const jv_t *g = json_get(sl, "gate");
        if (g && g->type == JV_NUM) {
            spec->has_gate = 1;
            spec->sil_gate = (float)g->num;
            if (spec->sil_gate < 0.0f) spec->sil_gate = 0.0f;
            if (spec->sil_gate > 1.0f) spec->sil_gate = 1.0f;
        }
    }

    const jv_t *use = json_get(json_get(root, "descriptors"), "use");
    if (use && use->type == JV_ARR) {
        float m[NDESC] = {0};
        int any = 0;
        for (const jv_t *u = use->child; u; u = u->next) {
            if (u->type == JV_STR && corpus_desc_group(u->str, m)) any = 1;
            else spec->unknown_desc++;
        }
        if (any) { spec->has_mask = 1; memcpy(spec->mask, m, sizeof(m)); }
    }

    const jv_t *gexcl = json_get(root, "exclude");
    const jv_t *srcs = json_get(root, "sources");
    int n = 0, si = 0;
    char (*tmp)[INGEST_PATH] = NULL;
    for (const jv_t *s = srcs && srcs->type == JV_ARR ? srcs->child : NULL; s && n < max; s = s->next, si++) {
        spec->sources_total++;
        const char *rel = s->type == JV_STR ? s->str : json_str(json_get(s, "path"), NULL);
        if (!rel || !rel[0]) { spec->sources_missing++; continue; }
        char full[INGEST_PATH];
        if (rel[0] == '/') snprintf(full, sizeof(full), "%s", rel);
        else               snprintf(full, sizeof(full), "%s/%s", CF_LIBRARY_ROOT, rel);
        struct stat st;
        if (stat(full, &st) != 0) { spec->sources_missing++; continue; }
        if (S_ISDIR(st.st_mode)) {
            add_watch(spec, full);
            if (!tmp && !(tmp = (char (*)[INGEST_PATH])malloc(sizeof(char[INGEST_PATH]) * (size_t)max))) break;
            int recurse = json_bool(json_get(s, "recurse"), 1);
            int nf = ingest_list_audio_ex(full, tmp, max, recurse);
            const jv_t *incl = json_get(s, "include"), *excl = json_get(s, "exclude");
            int has_incl = incl && incl->type == JV_ARR && incl->child;
            for (int f = 0; f < nf && n < max; f++) {
                const char *bn = base_name(tmp[f]);
                if (has_incl && !any_match(incl, bn)) continue;
                if (any_match(excl, bn) || any_match(gexcl, bn)) continue;
                snprintf(items[n].path, INGEST_PATH, "%s", tmp[f]);
                items[n].start_s = 0; items[n].end_s = 0; items[n].src = si;
                n++;
            }
        } else if (ingest_is_audio(full)) {
            double a = json_num(json_get(s, "start"), 0.0), b = json_num(json_get(s, "end"), 0.0);
            if (a < 0) a = 0;
            snprintf(items[n].path, INGEST_PATH, "%s", full);
            items[n].start_s = a; items[n].end_s = b > a ? b : 0.0; items[n].src = si;
            n++;
        } else {
            spec->sources_missing++;                    /* not audio we can read */
        }
    }
    free(tmp);
    json_free(root);
    return n;
}

int corpus_resolve(const char *path, corpus_spec_t *spec, corpus_item_t *items, int max) {
    memset(spec, 0, sizeof(*spec));
    for (int d = 0; d < NDESC; d++) spec->mask[d] = 1.0f;
    if (!path || !path[0] || max <= 0) return 0;
    if (corpus_is_concat(path)) return resolve_concat(path, spec, items, max);

    char (*paths)[INGEST_PATH] = (char (*)[INGEST_PATH])malloc(sizeof(char[INGEST_PATH]) * (size_t)max);
    if (!paths) return 0;
    int n = ingest_list_audio(path, paths, max);
    for (int i = 0; i < n; i++) {
        memcpy(items[i].path, paths[i], INGEST_PATH);
        items[i].start_s = 0; items[i].end_s = 0; items[i].src = 0;
    }
    free(paths);
    snprintf(spec->name, sizeof(spec->name), "%s", base_name(path));
    return n;
}
