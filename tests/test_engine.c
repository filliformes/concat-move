/* Concat — corpus engine test: build a real index+pool from a device folder. */
#include "corpus_engine.h"
#include "grain.h"
#include "ingest.h"
#include <stdio.h>
#include <math.h>
#include <time.h>

static int npass = 0, nfail = 0;
static void check(const char *name, int cond, const char *extra) {
    printf("  %s %s%s%s\n", cond ? "PASS" : "FAIL", name, extra ? "  -> " : "", extra ? extra : "");
    if (cond) npass++; else nfail++;
}
static double qn_range(corpus_index_t *x, int d) {   /* spread of one descriptor column */
    double lo = 1e30, hi = -1e30;
    for (int i = 0; i < x->nslices; i++) { double v = x->qn[i * NDESC + d]; if (v < lo) lo = v; if (v > hi) hi = v; }
    return hi - lo;
}

int main(void) {
    /* Discover a corpus folder instead of hardcoding one: the user's sample library
     * gets reorganised, and a stale path made this whole suite fail spuriously. */
    static corpus_folder_t folders[INGEST_MAX_FOLDERS];
    int nfound = ingest_scan_folders(folders, INGEST_MAX_FOLDERS), pick = -1;
    for (int i = 0; i < nfound; i++)
        if (folders[i].nfiles >= 5 && (pick < 0 || folders[i].nfiles > folders[pick].nfiles)) pick = i;
    if (pick < 0) { printf("  FAIL no corpus folder with >=5 audio files found\n\n0 passed, 1 failed\n"); return 1; }
    const char *folder = folders[pick].path;
    printf("    corpus: %s (%d files)\n", folder, folders[pick].nfiles);
    int prog = 0;
    /* ~1M-sample pool (~23s) so analysis stops after a few files → fast test. */
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    corpus_index_t *ix = corpus_index_build(folder, 44100, 0 /*onset*/, 2646, 17640, 0.02f, 1000000, NULL, 0, &prog);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    check("index built", ix != NULL, folder);
    if (!ix) { printf("\n%d passed, %d failed\n", npass, nfail); return 1; }

    char buf[160];
    snprintf(buf, sizeof(buf), "%d slices, pool %.1fs, %.1fs to analyze", ix->nslices, ix->pool_len / 44100.0, secs);
    check("nslices > 0", ix->nslices > 0, buf);
    check("pool used within cap", ix->pool_len > 0 && ix->pool_len <= ix->pool_cap, NULL);
    check("progress reached 100", prog == 100, NULL);

    int off_ok = 1;
    for (int i = 0; i < ix->nslices; i++)
        if (ix->slices[i].pool_off < 0 || ix->slices[i].pool_off + ix->slices[i].len > ix->pool_len || ix->slices[i].len <= 0)
            off_ok = 0;
    check("all slice offsets valid", off_ok, NULL);

    int qn_ok = isfinite(ix->qn[0]) && isfinite(ix->qn[(ix->nslices - 1) * NDESC + NDESC - 1]);
    check("descriptors (qn) finite", qn_ok, NULL);

    /* Query: navigate the map, get a slice. */
    float t[NDESC], w[NDESC];
    query_target_map(&ix->query, D_CENTROID_MEAN, D_PITCH, 0.5f, 0.5f, t, w);
    int sel = query_select(&ix->query, t, w, 4, 0.3f, 0.35f);
    snprintf(buf, sizeof(buf), "slice %d (off %d, len %d)", sel, sel >= 0 ? ix->slices[sel].pool_off : -1, sel >= 0 ? ix->slices[sel].len : -1);
    check("query returns a valid slice", sel >= 0 && sel < ix->nslices, buf);

    /* Different map corners give (usually) different slices. */
    query_target_map(&ix->query, D_CENTROID_MEAN, D_PITCH, 0.0f, 0.0f, t, w);
    int s0 = query_select(&ix->query, t, w, 1, 0.0f, 0.35f);
    query_target_map(&ix->query, D_CENTROID_MEAN, D_PITCH, 1.0f, 1.0f, t, w);
    int s1 = query_select(&ix->query, t, w, 1, 0.0f, 0.35f);
    snprintf(buf, sizeof(buf), "corner0=%d corner1=%d", s0, s1);
    check("map corners resolve to slices", s0 >= 0 && s1 >= 0, buf);

    /* Grain engine: render blocks from the corpus and confirm we get audio. */
    grain_engine_t ge; grain_engine_init(&ge);
    grain_params_t gp = {0};
    gp.k_nearest = 4; gp.radius = 0.3f; gp.density = 30.0f; gp.grain_size_ms = 120.0f;
    gp.grain_env = 0; gp.pitch_semi = 0.0f; gp.pitch_jitter = 0.0f;
    gp.pan_spread = 0.3f; gp.reverse_prob = 0.0f; gp.volume = 0.8f; gp.width = 1.0f;
    gp.grain_rand = 0.0f; gp.max_voices = 24;
    query_target_map(&ix->query, D_CENTROID_MEAN, D_PITCH, 0.5f, 0.5f, t, w);
    int16_t out[128 * 2];
    int peak = 0; long nz = 0;
    for (int b = 0; b < 200; b++) {
        grain_schedule(&ge, ix, t, w, &gp, 1.0f, NULL, 0.0f, &ge.sched_accum, 128, 44100);
        grain_mix(&ge, &gp, out, 128, 44100);
        for (int s = 0; s < 256; s++) { int v = out[s] < 0 ? -out[s] : out[s]; if (v > peak) peak = v; if (v) nz++; }
    }
    snprintf(buf, sizeof(buf), "peak=%d, %ld/%d nonzero samples", peak, nz, 200 * 256);
    check("grain engine produces audio", peak > 100, buf);
    check("grain output within int16", peak <= 32767, NULL);

    /* Regression (#1): extreme transpose + reverse must not read past a slice.
     * Pre-fix this walked the read index out of bounds; completing without a
     * segfault (and staying in range) is the guard. */
    gp.pitch_semi = 48.0f; gp.reverse_prob = 1.0f; grain_engine_init(&ge);
    int peak2 = 0;
    for (int b = 0; b < 300; b++) {
        grain_schedule(&ge, ix, t, w, &gp, 1.0f, NULL, 0.0f, &ge.sched_accum, 128, 44100);
        grain_mix(&ge, &gp, out, 128, 44100);
        for (int s = 0; s < 256; s++) { int v = out[s] < 0 ? -out[s] : out[s]; if (v > peak2) peak2 = v; }
    }
    check("high-transpose grains stay in bounds", peak2 <= 32767, NULL);

    { char kb[64]; snprintf(kb, sizeof(kb), "kclusters=%d", ix->kclusters);
      check("k-means clusters built", ix->kclusters > 0 && ix->centroids != NULL, kb); }

    /* PCA learned map: top-2 axes should be unit-length, ~orthogonal, and span a range. */
    check("pca: learned map built", ix->pca_ok == 1, NULL);
    if (ix->pca_ok) {
        double n1 = 0, n2 = 0, dot = 0;
        for (int j = 0; j < NDESC; j++) {
            n1 += (double)ix->pca_axis[0][j] * ix->pca_axis[0][j];
            n2 += (double)ix->pca_axis[1][j] * ix->pca_axis[1][j];
            dot += (double)ix->pca_axis[0][j] * ix->pca_axis[1][j];
        }
        char pb[128]; snprintf(pb, sizeof(pb), "|v1|=%.3f |v2|=%.3f v1.v2=%.3f range x=[%.2f,%.2f]",
                               sqrt(n1), sqrt(n2), dot, ix->pca_min[0], ix->pca_max[0]);
        check("pca: axes orthonormal", fabs(sqrt(n1) - 1) < 0.02 && fabs(sqrt(n2) - 1) < 0.02 && fabs(dot) < 0.05, pb);
        check("pca: axes span a range", ix->pca_max[0] > ix->pca_min[0] && ix->pca_max[1] > ix->pca_min[1], NULL);
    }

    /* OOM eviction: a small (overflowing) pool must build, respect the cap, and
     * still cover the corpus (descriptor range not collapsed to the first files). */
    int pq = 0;
    corpus_index_t *small = corpus_index_build(folder, 44100, 0, 2646, 17640, 0.02f, 250000, NULL, 0, &pq);
    check("oom: overflowing pool still builds", small && small->nslices > 0, NULL);
    if (small) {
        check("oom: respects pool cap", small->pool_len <= 250000, NULL);
        double rb = qn_range(ix, D_CENTROID_MEAN), rs = qn_range(small, D_CENTROID_MEAN);
        char ob[128]; snprintf(ob, sizeof(ob), "centroid range small/big=%.2f  (n=%d vs %d)",
                               rb > 0 ? rs / rb : 0.0, small->nslices, ix->nslices);
        check("oom: coverage spreads (range not collapsed)", rb <= 0.0 || rs > 0.4 * rb, ob);
        corpus_index_free(small);
    }

    corpus_index_free(ix);

    /* Index disk cache: force-build (writes cache) then plain build (must hit + match).
     * Use the /data user partition — the device rootfs (/tmp) can be full. */
    const char *cd = "/data/UserData/schwung/concat-cache-test";
    struct timespec cb0, cb1, cl0, cl1;
    clock_gettime(CLOCK_MONOTONIC, &cb0);
    corpus_index_t *c1 = corpus_index_build(folder, 44100, 0, 2646, 17640, 0.02f, 1000000, cd, 1, &prog);
    clock_gettime(CLOCK_MONOTONIC, &cb1);
    clock_gettime(CLOCK_MONOTONIC, &cl0);
    corpus_index_t *c2 = corpus_index_build(folder, 44100, 0, 2646, 17640, 0.02f, 1000000, cd, 0, &prog);
    clock_gettime(CLOCK_MONOTONIC, &cl1);
    double t_build = (cb1.tv_sec - cb0.tv_sec) + (cb1.tv_nsec - cb0.tv_nsec) / 1e9;
    double t_load  = (cl1.tv_sec - cl0.tv_sec) + (cl1.tv_nsec - cl0.tv_nsec) / 1e9;
    check("cache: both builds succeeded", c1 && c2, NULL);
    if (c1 && c2) {
        char cbuf[160];
        snprintf(cbuf, sizeof(cbuf), "nslices %d==%d, pool %d==%d | build %.3fs -> load %.3fs",
                 c1->nslices, c2->nslices, c1->pool_len, c2->pool_len, t_build, t_load);
        check("cache hit matches nslices + pool_len", c1->nslices == c2->nslices && c1->pool_len == c2->pool_len, cbuf);
        int same = 1;
        for (int i = 0; i < c2->pool_len; i++) if (c1->pool[i] != c2->pool[i]) { same = 0; break; }
        check("cache pool bytes identical", same, NULL);
        check("cache load faster than analysis", t_load < t_build, NULL);
        check("cache preserves k-means clusters", c2->kclusters == c1->kclusters && c1->kclusters > 0, NULL);
        check("cache preserves PCA map", c2->pca_ok == c1->pca_ok && (!c1->pca_ok || fabs(c1->pca_axis[0][0] - c2->pca_axis[0][0]) < 1e-6), NULL);
    }
    corpus_index_free(c1); corpus_index_free(c2);
    printf("\n%d passed, %d failed\n", npass, nfail);
    return nfail ? 1 : 0;
}
