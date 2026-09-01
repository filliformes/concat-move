/* Concat — correctness tests for the descriptor kernels (analysis.c).
 * Cross-compiled for aarch64 and run on the Move via scripts/test.sh. */
#include "analysis.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define N 1024
#define SR 44100

static int npass = 0, nfail = 0;
static void check(const char *name, int cond, const char *fmt, ...) {
    char detail[128] = {0};
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(detail, sizeof(detail), fmt, ap); va_end(ap); }
    printf("  %s %s%s%s\n", cond ? "PASS" : "FAIL", name, detail[0] ? "  -> " : "", detail);
    if (cond) npass++; else nfail++;
}

static void gen_sine(float *x, int n, double hz) {
    for (int i = 0; i < n; i++) x[i] = (float)sin(2.0 * M_PI * hz * i / SR);
}
static void gen_noise(float *x, int n, unsigned seed) {
    unsigned s = seed ? seed : 1;
    for (int i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; x[i] = (float)((int)s) / 2147483648.0f; }
}
static int all_finite(const float *v, int n) {
    for (int i = 0; i < n; i++) if (!isfinite(v[i])) return 0;
    return 1;
}

int main(void) {
    analysis_t *a = analysis_create(SR, N, 40, 13);
    check("analysis_create", a != NULL, NULL);
    if (!a) return 1;

    float sine440[N], sine220[N], noise[N];
    gen_sine(sine440, N, 440.0);
    gen_sine(sine220, N, 220.0);
    gen_noise(noise, N, 12345);

    /* Pitch */
    float c1, c2, cn;
    float p440 = analysis_yin(a, sine440, 50.0f, 2000.0f, &c1);
    float p220 = analysis_yin(a, sine220, 50.0f, 2000.0f, &c2);
    float pn   = analysis_yin(a, noise,   50.0f, 2000.0f, &cn);
    check("YIN 440Hz sine", fabsf(p440 - 440.0f) < 6.0f, "%.1f Hz (conf %.2f)", p440, c1);
    check("YIN 220Hz sine", fabsf(p220 - 220.0f) < 4.0f, "%.1f Hz (conf %.2f)", p220, c2);
    check("YIN noise = unvoiced/low-conf", (pn == 0.0f || cn < 0.8f), "%.1f Hz (conf %.2f)", pn, cn);
    check("YIN confidence high on tone", c1 > 0.8f, "conf %.2f", c1);

    /* Magnitude + spectral shape */
    float mag440[N / 2 + 1], magN[N / 2 + 1];
    analysis_magnitude(a, sine440, mag440);
    analysis_magnitude(a, noise, magN);
    float cent = analysis_centroid(a, mag440);
    float flat_t = analysis_flatness(a, mag440);
    float flat_n = analysis_flatness(a, magN);
    check("centroid 440 sine near 440Hz", cent > 350.0f && cent < 650.0f, "%.0f Hz", cent);
    check("flatness tone low", flat_t < 0.15f, "%.3f", flat_t);
    check("flatness noise > tone", flat_n > flat_t && flat_n > 0.12f, "noise %.3f > tone %.3f", flat_n, flat_t);

    /* MFCC finite */
    float mfcc[13];
    analysis_mfcc(a, mag440, mfcc);
    check("MFCC finite", all_finite(mfcc, 13), "c0=%.2f c1=%.2f", mfcc[0], mfcc[1]);

    /* Flux */
    float f_same = analysis_flux(a, mag440, mag440);
    float f_diff = analysis_flux(a, magN, mag440);
    check("flux(same)=0", f_same == 0.0f, "%.4f", f_same);
    check("flux(diff)>0", f_diff > 0.0f, "%.4f", f_diff);

    /* ZCR + RMS */
    float z440 = analysis_zcr(sine440, N);
    float z220 = analysis_zcr(sine220, N);
    check("ZCR 440 > ZCR 220", z440 > z220, "%.4f vs %.4f", z440, z220);
    float rms = analysis_rms(sine440, N);
    check("RMS sine ~0.707", fabsf(rms - 0.7071f) < 0.03f, "%.4f", rms);

    /* Welford stats: {2,4,4,4,5,5,7,9} -> mean 5, pop-std 2 */
    rstat_t s; rstat_init(&s);
    float data[] = {2, 4, 4, 4, 5, 5, 7, 9};
    for (int i = 0; i < 8; i++) rstat_add(&s, data[i]);
    check("rstat mean=5", fabsf(rstat_mean(&s) - 5.0f) < 1e-4f, "%.4f", rstat_mean(&s));
    check("rstat std=2", fabsf(rstat_std(&s) - 2.0f) < 1e-4f, "%.4f", rstat_std(&s));

    analysis_destroy(a);
    printf("\n%d passed, %d failed\n", npass, nfail);
    return nfail ? 1 : 0;
}
