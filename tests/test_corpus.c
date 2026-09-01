/* Concat — tests for the corpus descriptor layer (corpus.c). aarch64 on device. */
#include "corpus.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define SR 44100

static int npass = 0, nfail = 0;
static void check(const char *name, int cond, const char *fmt, ...) {
    char d[160] = {0};
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof(d), fmt, ap); va_end(ap); }
    printf("  %s %s%s%s\n", cond ? "PASS" : "FAIL", name, d[0] ? "  -> " : "", d);
    if (cond) npass++; else nfail++;
}
static void fill_sine(float *x, int off, int n, double hz, float amp) {
    for (int i = 0; i < n; i++) x[off + i] = amp * (float)sin(2.0 * M_PI * hz * i / SR);
}

int main(void) {
    analysis_t *a = analysis_create(SR, 1024, 40, 13);
    if (!a) { printf("FAIL analysis_create\n"); return 1; }

    /* --- per-slice descriptor of a stationary 440 Hz tone --- */
    int L = 4410;
    float *tone = (float *)calloc(L, sizeof(float));
    fill_sine(tone, 0, L, 440.0, 0.8f);
    float dv[NDESC];
    concat_slice_descriptor(a, tone, L, dv);
    check("slice pitch ~440", fabsf(dv[D_PITCH] - 440.0f) < 6.0f, "%.1f Hz", dv[D_PITCH]);
    check("slice centroid ~440", dv[D_CENTROID_MEAN] > 350.0f && dv[D_CENTROID_MEAN] < 650.0f, "%.0f Hz", dv[D_CENTROID_MEAN]);
    check("slice loud>0", dv[D_LOUD_MEAN] > 0.1f, "%.3f", dv[D_LOUD_MEAN]);
    check("slice peak ~0.8", fabsf(dv[D_LOUD_PEAK] - 0.8f) < 0.05f, "%.3f", dv[D_LOUD_PEAK]);
    check("slice mfcc0 finite", isfinite(dv[D_MFCC0_MEAN]), "%.2f", dv[D_MFCC0_MEAN]);
    check("stationary tone → small centroid std", dv[D_CENTROID_STD] < 40.0f, "%.2f", dv[D_CENTROID_STD]);
    free(tone);

    /* --- onset slicing: silence | tone | silence | tone | silence --- */
    int seg_sil1 = 5000, seg_t1 = 8820, seg_sil2 = 8000, seg_t2 = 8820, seg_sil3 = 3000;
    int total = seg_sil1 + seg_t1 + seg_sil2 + seg_t2 + seg_sil3;
    float *buf = (float *)calloc(total, sizeof(float));
    fill_sine(buf, seg_sil1, seg_t1, 330.0, 0.7f);
    fill_sine(buf, seg_sil1 + seg_t1 + seg_sil2, seg_t2, 660.0, 0.7f);
    slice_range_t sr[32];
    int ns = concat_onset_slice(a, buf, total, 2646, 17640, 0.03f, sr, 32);
    printf("    onset slices: %d ->", ns);
    for (int i = 0; i < ns; i++) printf(" [%d,+%d]", sr[i].start, sr[i].len);
    printf("\n");
    check("onset: >=2 slices", ns >= 2 && ns <= 8, "%d", ns);
    int has_t1 = 0, has_t2 = 0, all_loud = 1;
    for (int i = 0; i < ns; i++) {
        if (sr[i].start >= 3000 && sr[i].start < seg_sil1 + seg_t1) has_t1 = 1;
        if (sr[i].start >= seg_sil1 + seg_t1 + seg_sil2 - 2000) has_t2 = 1;
        if (analysis_rms(&buf[sr[i].start], sr[i].len) < 0.03f) all_loud = 0;
    }
    check("onset: first tone found", has_t1, NULL);
    check("onset: second tone found", has_t2, NULL);
    check("onset: silence dropped (all slices loud)", all_loud, NULL);
    check("onset: no slice starts at 0 (leading silence gated)", ns > 0 && sr[0].start > 2000, "start=%d", ns > 0 ? sr[0].start : -1);
    free(buf);

    /* --- fixed slicing on a constant tone --- */
    int FL = 44100;
    float *cont = (float *)calloc(FL, sizeof(float));
    fill_sine(cont, 0, FL, 220.0, 0.6f);
    int nf = concat_fixed_slice(a, cont, FL, 8820, 0.03f, sr, 32);
    check("fixed slices count", nf == 9, "%d", nf);
    free(cont);

    /* --- RobustScaling: dim0 = {1,2,3,4,5} → median 3, iqr 2 --- */
    int NS = 5;
    float *desc = (float *)calloc(NS * NDESC, sizeof(float));
    for (int i = 0; i < NS; i++) desc[i * NDESC + 0] = (float)(i + 1);
    float med[NDESC], iqr[NDESC];
    concat_robust_fit(desc, NS, med, iqr);
    check("robust median dim0 = 3", fabsf(med[0] - 3.0f) < 1e-4f, "%.3f", med[0]);
    check("robust iqr dim0 = 2", fabsf(iqr[0] - 2.0f) < 1e-4f, "%.3f", iqr[0]);
    float vec[NDESC]; memset(vec, 0, sizeof(vec)); vec[0] = 5.0f;
    float outv[NDESC];
    concat_robust_apply(vec, med, iqr, outv);
    check("robust apply (5-3)/2 = 1", fabsf(outv[0] - 1.0f) < 1e-4f, "%.3f", outv[0]);
    float mn[NDESC], mx[NDESC];
    concat_minmax_fit(desc, NS, mn, mx);
    check("minmax dim0 = [1,5]", fabsf(mn[0] - 1.0f) < 1e-4f && fabsf(mx[0] - 5.0f) < 1e-4f, "[%.1f,%.1f]", mn[0], mx[0]);
    free(desc);

    analysis_destroy(a);
    printf("\n%d passed, %d failed\n", npass, nfail);
    return nfail ? 1 : 0;
}
