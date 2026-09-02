/* Concat — corpus engine test: build a real index+pool from a device folder. */
#include "corpus_engine.h"
#include "grain.h"
#include <stdio.h>
#include <math.h>
#include <time.h>

static int npass = 0, nfail = 0;
static void check(const char *name, int cond, const char *extra) {
    printf("  %s %s%s%s\n", cond ? "PASS" : "FAIL", name, extra ? "  -> " : "", extra ? extra : "");
    if (cond) npass++; else nfail++;
}

int main(void) {
    const char *folder = "/data/UserData/UserLibrary/Samples/MAITRISE";
    int prog = 0;
    /* ~1M-sample pool (~23s) so analysis stops after a few files → fast test. */
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    corpus_index_t *ix = corpus_index_build(folder, 44100, 0 /*onset*/, 2646, 17640, 0.02f, 1000000, &prog);
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
    int sel = query_select(&ix->query, t, w, 4, 0.3f);
    snprintf(buf, sizeof(buf), "slice %d (off %d, len %d)", sel, sel >= 0 ? ix->slices[sel].pool_off : -1, sel >= 0 ? ix->slices[sel].len : -1);
    check("query returns a valid slice", sel >= 0 && sel < ix->nslices, buf);

    /* Different map corners give (usually) different slices. */
    query_target_map(&ix->query, D_CENTROID_MEAN, D_PITCH, 0.0f, 0.0f, t, w);
    int s0 = query_select(&ix->query, t, w, 1, 0.0f);
    query_target_map(&ix->query, D_CENTROID_MEAN, D_PITCH, 1.0f, 1.0f, t, w);
    int s1 = query_select(&ix->query, t, w, 1, 0.0f);
    snprintf(buf, sizeof(buf), "corner0=%d corner1=%d", s0, s1);
    check("map corners resolve to slices", s0 >= 0 && s1 >= 0, buf);

    /* Grain engine: render blocks from the corpus and confirm we get audio. */
    grain_engine_t ge; grain_engine_init(&ge);
    grain_params_t gp = {0};
    gp.k_nearest = 4; gp.radius = 0.3f; gp.density = 30.0f; gp.grain_size_ms = 120.0f;
    gp.grain_env = 0; gp.pitch_semi = 0.0f; gp.pitch_jitter = 0.0f;
    gp.pan_spread = 0.3f; gp.reverse_prob = 0.0f; gp.volume = 0.8f; gp.width = 1.0f;
    query_target_map(&ix->query, D_CENTROID_MEAN, D_PITCH, 0.5f, 0.5f, t, w);
    int16_t out[128 * 2];
    int peak = 0; long nz = 0;
    for (int b = 0; b < 200; b++) {
        grain_render(&ge, ix, t, w, &gp, out, 128, 44100);
        for (int s = 0; s < 256; s++) { int v = out[s] < 0 ? -out[s] : out[s]; if (v > peak) peak = v; if (v) nz++; }
    }
    snprintf(buf, sizeof(buf), "peak=%d, %ld/%d nonzero samples", peak, nz, 200 * 256);
    check("grain engine produces audio", peak > 100, buf);
    check("grain output within int16", peak <= 32767, NULL);

    corpus_index_free(ix);
    printf("\n%d passed, %d failed\n", npass, nfail);
    return nfail ? 1 : 0;
}
