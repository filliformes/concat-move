/* Concat — tests for .concat corpus files: JSON reader, descriptor groups, source
 * resolution, and builds from a corpus file (ranges, slicing override, descriptor
 * mask, cache signature). Writes its own WAV fixture, so it runs anywhere. */
#include "json.h"
#include "corpus_file.h"
#include "corpus_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

static int npass = 0, nfail = 0;
static void check(const char *name, int cond, const char *fmt, ...) {
    char d[200] = {0};
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof(d), fmt, ap); va_end(ap); }
    printf("  %s %s%s%s\n", cond ? "PASS" : "FAIL", name, d[0] ? "  -> " : "", d);
    if (cond) npass++; else nfail++;
}

#define FIX "/data/UserData/schwung/concat-cf-test"

/* Mono 16-bit WAV of decaying tone/noise events (enough onsets to slice). */
static void write_wav(const char *path, int sr, double secs, unsigned seed) {
    int n = (int)(sr * secs);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    uint32_t data = (uint32_t)n * 2, riff = 36 + data, fmt = 16, rate = (uint32_t)sr, br = rate * 2;
    uint16_t pcm = 1, ch = 1, ba = 2, bits = 16;
    fwrite("RIFF", 1, 4, fp); fwrite(&riff, 4, 1, fp); fwrite("WAVEfmt ", 1, 8, fp);
    fwrite(&fmt, 4, 1, fp); fwrite(&pcm, 2, 1, fp); fwrite(&ch, 2, 1, fp); fwrite(&rate, 4, 1, fp);
    fwrite(&br, 4, 1, fp); fwrite(&ba, 2, 1, fp); fwrite(&bits, 2, 1, fp);
    fwrite("data", 1, 4, fp); fwrite(&data, 4, 1, fp);
    unsigned r = seed;
    double env = 0, f0 = 200, ph = 0, noise = 0;
    int next = 0;
    for (int i = 0; i < n; i++) {
        if (i >= next) {
            r = r * 1664525u + 1013904223u; next = i + sr / 10 + (int)(r % (unsigned)(sr / 4));
            r = r * 1664525u + 1013904223u; f0 = 100 + r % 2500;
            r = r * 1664525u + 1013904223u; noise = (r % 100) / 100.0;
            env = 1.0;
        }
        env *= 0.9996;
        ph += 2 * M_PI * f0 / sr;
        r = r * 1664525u + 1013904223u;
        double nz = ((r >> 8) / 8388608.0) - 1.0;
        double v = env * ((1 - noise) * sin(ph) + noise * nz) * 0.6;
        int16_t s = (int16_t)(v * 32767);
        fwrite(&s, 2, 1, fp);
    }
    fclose(fp);
}

static void write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "w");
    if (fp) { fputs(text, fp); fclose(fp); }
}

static int has_item(const corpus_item_t *it, int n, const char *suffix) {
    size_t ls = strlen(suffix);
    for (int i = 0; i < n; i++) {
        size_t l = strlen(it[i].path);
        if (l >= ls && !strcmp(it[i].path + l - ls, suffix)) return 1;
    }
    return 0;
}

int main(void) {
    /* ── JSON ── */
    char err[64];
    jv_t *v = json_parse("{\"a\": [1, 2.5, -3e2], \"s\": \"caf\\u00e9 \\\"q\\\"\", \"t\": true, \"n\": null, \"o\": {}}", err, sizeof err);
    check("json: parses a mixed document", v != NULL, "%s", err);
    if (v) {
        const jv_t *a = json_get(v, "a");
        check("json: array of numbers", a && a->type == JV_ARR && a->child && a->child->next &&
              json_num(a->child->next->next, 0) == -300.0, NULL);
        check("json: \\u escape -> UTF-8, quotes", !strcmp(json_str(json_get(v, "s"), ""), "caf\xc3\xa9 \"q\""),
              "[%s]", json_str(json_get(v, "s"), ""));
        check("json: bool / null / empty object", json_bool(json_get(v, "t"), 0) == 1 &&
              json_get(v, "n")->type == JV_NULL && json_get(v, "o")->type == JV_OBJ, NULL);
        check("json: missing key falls back", json_num(json_get(v, "zz"), 7) == 7 && json_get(v, "zz") == NULL, NULL);
        json_free(v);
    }
    const char *bad[] = { "[1,]", "{\"a\" 1}", "\"open", "[1] x", "{\"a\":tru}", "" };
    int all_bad = 1;
    for (int i = 0; i < 6; i++) { jv_t *b = json_parse(bad[i], err, sizeof err); if (b || !err[0]) all_bad = 0; json_free(b); }
    check("json: malformed input rejected with a reason", all_bad, NULL);
    char deep[200]; memset(deep, '[', 100); memset(deep + 100, ']', 99); deep[199] = 0; deep[100 + 98] = ']';
    jv_t *dv = json_parse(deep, err, sizeof err);
    check("json: nesting depth bounded", dv == NULL, "%s", err);

    /* ── descriptor groups ── */
    float m[NDESC]; memset(m, 0, sizeof m);
    int ok = corpus_desc_group("mfcc3-5", m);
    int cnt = 0; for (int d = 0; d < NDESC; d++) cnt += m[d] != 0;
    check("desc: mfcc3-5 = 3 coefficients x mean/std", ok && cnt == 6 && m[D_MFCC0_MEAN + 2] && m[D_MFCC0_STD + 4], "%d bits", cnt);
    memset(m, 0, sizeof m);
    check("desc: pitch = pitch + confidence", corpus_desc_group("pitch", m) && m[D_PITCH] && m[D_PITCH_CONF], NULL);
    check("desc: unknown names rejected", !corpus_desc_group("mfcc14", m) && !corpus_desc_group("bogus", m) &&
          !corpus_desc_group("mfcc5-3", m), NULL);

    /* ── fixture ── */
    if (system("rm -rf " FIX " && mkdir -p " FIX "/lib/sub " FIX "/cache") != 0) { printf("  FAIL fixture\n"); return 1; }
    write_wav(FIX "/lib/a_tone.wav", 44100, 4.0, 1);
    write_wav(FIX "/lib/b_tone.wav", 48000, 3.0, 2);
    write_wav(FIX "/lib/click_x.wav", 44100, 2.0, 3);
    write_wav(FIX "/lib/sub/kick1.wav", 44100, 2.0, 4);
    write_wav(FIX "/lib/sub/snare1.wav", 44100, 2.0, 5);
    write_wav(FIX "/long.wav", 44100, 6.0, 6);

    static corpus_item_t items[64];
    static corpus_spec_t spec;

    write_text(FIX "/c1.concat",
        "{\"concat\":1,\"name\":\"Test One\",\"sources\":["
        "{\"path\":\"" FIX "/lib\",\"recurse\":true},"
        "{\"path\":\"" FIX "/lib/sub\",\"include\":[\"*KICK*\"],\"recurse\":false},"
        "{\"path\":\"" FIX "/long.wav\",\"start\":1.0,\"end\":3.0},"
        "{\"path\":\"" FIX "/nope\"},"
        "\"" FIX "/lib/a_tone.wav\"],"
        "\"exclude\":[\"click*\"]}");
    int n = corpus_resolve(FIX "/c1.concat", &spec, items, 64);
    check("resolve: .concat parsed", n > 0 && spec.is_concat && !strcmp(spec.name, "Test One"), "n=%d err=%s", n, spec.err);
    check("resolve: recursive folder + excluded click", has_item(items, n, "/lib/sub/snare1.wav") && !has_item(items, n, "click_x.wav"), NULL);
    int kicks = 0; for (int i = 0; i < n; i++) if (strstr(items[i].path, "kick1")) kicks++;
    check("resolve: include glob is case-insensitive", kicks == 2, "%d kick entries (folder + include)", kicks);
    int rng = -1; for (int i = 0; i < n; i++) if (strstr(items[i].path, "long.wav")) rng = i;
    check("resolve: file range kept", rng >= 0 && items[rng].start_s == 1.0 && items[rng].end_s == 3.0 && items[rng].src == 2, NULL);
    check("resolve: bare-string source accepted", items[n - 1].src == 4 && strstr(items[n - 1].path, "a_tone"), NULL);
    check("resolve: missing source counted, not fatal", spec.sources_total == 5 && spec.sources_missing == 1, "%d/%d", spec.sources_missing, spec.sources_total);
    check("resolve: watches the file and its folders", spec.nwatch == 3, "nwatch=%d", spec.nwatch);
    check("resolve: no mask unless asked", !spec.has_mask && spec.mask[0] == 1.0f, NULL);

    write_text(FIX "/bad.concat", "{\"concat\":1, \"sources\": [ }");
    n = corpus_resolve(FIX "/bad.concat", &spec, items, 64);
    check("resolve: bad JSON -> -1 with reason", n == -1 && strstr(spec.err, "JSON"), "[%s]", spec.err);
    write_text(FIX "/v2.concat", "{\"concat\":2, \"sources\": []}");
    n = corpus_resolve(FIX "/v2.concat", &spec, items, 64);
    check("resolve: unknown version refused", n == -1 && strstr(spec.err, "version"), "[%s]", spec.err);
    n = corpus_resolve(FIX "/lib", &spec, items, 64);
    check("resolve: plain folder still works", n == 5 && !spec.is_concat, "n=%d", n);

    /* ── builds from corpus files ── */
    corpus_build_info_t bi; memset(&bi, 0, sizeof bi);
    write_text(FIX "/range.concat",
        "{\"concat\":1,\"sources\":[{\"path\":\"" FIX "/long.wav\",\"start\":1.0,\"end\":3.0}],"
        "\"slicing\":{\"mode\":\"fixed\",\"min_ms\":50,\"max_ms\":100},"
        "\"descriptors\":{\"use\":[\"loud\",\"centroid\",\"mfcc1-4\",\"nonsense\"]}}");
    bi.spec = &spec;
    corpus_index_t *ix = corpus_index_build(FIX "/range.concat", 44100, 0, 2646, 17640, 0.0f, 1000000, 0, NULL, 0, &bi);
    check("build: .concat builds", ix && ix->nslices > 0, "err=%d", bi.err);
    if (ix) {
        int maxlen = 0; for (int i = 0; i < ix->nslices; i++) if (ix->slices[i].len > maxlen) maxlen = ix->slices[i].len;
        check("build: slicing override (fixed 100 ms grains)", maxlen <= 4410 + 1, "max slice %d samples", maxlen);
        check("build: range limits the audio (2 s, 50% overlap grains)", ix->pool_len <= 2 * 2 * 44100 + 4410 && ix->pool_len > 44100,
              "pool %d samples", ix->pool_len);
        int zero_ok = 1, used_ok = 0;
        for (int i = 0; i < ix->nslices; i++) {
            const float *q = &ix->qn[(size_t)i * NDESC];
            if (q[D_PITCH] != 0.0f || q[D_FLATNESS_MEAN] != 0.0f || q[D_MFCC0_MEAN + 6] != 0.0f) zero_ok = 0;
            if (q[D_CENTROID_MEAN] != 0.0f) used_ok = 1;
        }
        check("build: unused descriptors zeroed in query space", zero_ok, NULL);
        check("build: used descriptors kept", used_ok, NULL);
        check("build: unknown descriptor name counted", spec.unknown_desc == 1, "%d", spec.unknown_desc);
        corpus_index_free(ix);
    }

    /* The range must start on the exact sample: compare its first grain with a direct decode. */
    write_text(FIX "/r2.concat", "{\"concat\":1,\"sources\":[{\"path\":\"" FIX "/long.wav\",\"start\":2.0,\"end\":4.0}],"
                                 "\"slicing\":{\"mode\":\"fixed\",\"min_ms\":100,\"max_ms\":100}}");
    corpus_index_t *part = corpus_index_build(FIX "/r2.concat", 44100, 0, 4410, 4410, 0.0f, 1000000, 0, NULL, 0, &bi);
    int len = 0; float *dec = ingest_decode(FIX "/long.wav", 44100, &len);
    int same = part && dec && len > 88200 + 4410 && part->nslices > 0 && part->slices[0].len >= 2000;
    for (int i = 0; same && i < 2000; i++) {
        int16_t want = (int16_t)(fmaxf(-1.0f, fminf(1.0f, dec[88200 + i])) * 32767.0f);
        if (abs(part->pool[part->slices[0].pool_off + i] - want) > 1) same = 0;
    }
    check("build: range starts at the exact sample", same, NULL);
    free(dec); corpus_index_free(part);

    /* Cache: unchanged -> hit; edited range -> rebuilt; mask -> different signature. */
    corpus_index_t *c1 = corpus_index_build(FIX "/c1.concat", 44100, 0, 2646, 17640, 0.02f, 1000000, 0, FIX "/cache", 1, &bi);
    corpus_index_t *c2 = corpus_index_build(FIX "/c1.concat", 44100, 0, 2646, 17640, 0.02f, 1000000, 0, FIX "/cache", 0, &bi);
    check("cache: unchanged corpus file hits", c1 && c2 && bi.from_cache == 1 && bi.sources_missing == 1, NULL);
    corpus_index_free(c1); corpus_index_free(c2);
    sleep(1);   /* mtime granularity */
    write_text(FIX "/c1.concat",
        "{\"concat\":1,\"name\":\"Test One\",\"sources\":["
        "{\"path\":\"" FIX "/lib\",\"recurse\":true},"
        "{\"path\":\"" FIX "/lib/sub\",\"include\":[\"*KICK*\"],\"recurse\":false},"
        "{\"path\":\"" FIX "/long.wav\",\"start\":1.5,\"end\":3.0},"
        "{\"path\":\"" FIX "/nope\"},"
        "\"" FIX "/lib/a_tone.wav\"],"
        "\"exclude\":[\"click*\"]}");
    corpus_index_t *c3 = corpus_index_build(FIX "/c1.concat", 44100, 0, 2646, 17640, 0.02f, 1000000, 0, FIX "/cache", 0, &bi);
    check("cache: edited range rebuilds", c3 && bi.from_cache == 0, NULL);
    corpus_index_free(c3);

    corpus_index_t *cb = corpus_index_build(FIX "/bad.concat", 44100, 0, 2646, 17640, 0.02f, 1000000, 0, NULL, 0, &bi);
    check("build: bad corpus file -> CB_ERR_SPEC", cb == NULL && bi.err == CB_ERR_SPEC && strstr(spec.err, "JSON"), NULL);

    /* Per-file cache: add a file to a source folder. The rebuild reuses every other
     * file's slices + descriptors, and the result equals a from-scratch analysis. */
    corpus_index_t *w1 = corpus_index_build(FIX "/c1.concat", 44100, 0, 2646, 17640, 0.02f, 1000000, 0, FIX "/cache", 1, &bi);
    int nf1 = bi.files_total;
    corpus_index_free(w1);
    write_wav(FIX "/lib/added.wav", 44100, 2.0, 11);
    corpus_index_t *w2 = corpus_index_build(FIX "/c1.concat", 44100, 0, 2646, 17640, 0.02f, 1000000, 0, FIX "/cache", 0, &bi);
    int cached = bi.files_cached, nf2 = bi.files_total, hit2 = bi.from_cache;
    corpus_index_t *w3 = corpus_index_build(FIX "/c1.concat", 44100, 0, 2646, 17640, 0.02f, 1000000, 0, NULL, 0, &bi);
    check("pfc: index cache misses, unchanged files reuse the per-file cache",
          !hit2 && nf2 == nf1 + 1 && cached == nf1, "cached %d of %d", cached, nf2);
    same = w2 && w3 && w2->nslices == w3->nslices && w2->pool_len == w3->pool_len;
    for (int i = 0; same && i < w2->nslices * NDESC; i++) if (fabsf(w2->qn[i] - w3->qn[i]) > 1e-5f) same = 0;
    for (int i = 0; same && i < w2->pool_len; i++) if (w2->pool[i] != w3->pool[i]) same = 0;
    check("pfc: cached rebuild == full analysis (slices, audio, descriptors)", same,
          "n %d/%d", w2 ? w2->nslices : -1, w3 ? w3->nslices : -1);
    corpus_index_free(w2); corpus_index_free(w3);

    /* A memory-capped build leaves descriptors missing for decimated slices; a full
     * build later fills them in and still matches. */
    corpus_index_t *d1 = corpus_index_build(FIX "/lib", 44100, 0, 2646, 17640, 0.02f, 1000000, 60000, FIX "/cache", 1, &bi);
    corpus_index_free(d1);
    corpus_index_t *d2 = corpus_index_build(FIX "/lib", 44100, 0, 2646, 17640, 0.02f, 1000000, 0, FIX "/cache", 0, &bi);
    int dcached = bi.files_cached;
    corpus_index_t *d3 = corpus_index_build(FIX "/lib", 44100, 0, 2646, 17640, 0.02f, 1000000, 0, NULL, 0, &bi);
    same = d2 && d3 && d2->nslices == d3->nslices;
    for (int i = 0; same && i < d2->nslices * NDESC; i++) if (fabsf(d2->qn[i] - d3->qn[i]) > 1e-5f) same = 0;
    check("pfc: partial cache from a capped build completes correctly", same && dcached > 0,
          "cached %d, n %d/%d", dcached, d2 ? d2->nslices : -1, d3 ? d3->nslices : -1);
    corpus_index_free(d2); corpus_index_free(d3);

    /* watch stamp follows edits */
    corpus_resolve(FIX "/c1.concat", &spec, items, 64);
    unsigned long long s1 = corpus_watch_stamp(&spec);
    sleep(1);
    write_wav(FIX "/lib/new_one.wav", 44100, 1.0, 9);           /* folder content changes */
    unsigned long long s2 = corpus_watch_stamp(&spec);
    check("watch: adding a file to a source folder changes the stamp", s1 && s2 && s1 != s2, NULL);

    if (system("rm -rf " FIX) != 0) printf("  (fixture not removed)\n");
    printf("\n%d passed, %d failed\n", npass, nfail);
    return nfail ? 1 : 0;
}
