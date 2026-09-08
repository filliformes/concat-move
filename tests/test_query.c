/* Concat — tests for the descriptor-space query (query.c). aarch64 on device. */
#include "query.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

static int npass = 0, nfail = 0;
static void check(const char *name, int cond, const char *fmt, ...) {
    char d[160] = {0};
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof(d), fmt, ap); va_end(ap); }
    printf("  %s %s%s%s\n", cond ? "PASS" : "FAIL", name, d[0] ? "  -> " : "", d);
    if (cond) npass++; else nfail++;
}

int main(void) {
    const int NS = 5;
    static float qn[5 * NDESC];
    memset(qn, 0, sizeof(qn));
    /* two informative dims, parallel: centroid & pitch = {100,200,300,400,500} */
    for (int i = 0; i < NS; i++) {
        qn[i * NDESC + D_CENTROID_MEAN] = 100.0f * (i + 1);
        qn[i * NDESC + D_PITCH]         = 100.0f * (i + 1);
    }

    query_t q;
    query_init(&q, qn, NS);
    check("nav_min centroid=100", q.nav_min[D_CENTROID_MEAN] == 100.0f, "%.0f", q.nav_min[D_CENTROID_MEAN]);
    check("nav_max centroid=500", q.nav_max[D_CENTROID_MEAN] == 500.0f, "%.0f", q.nav_max[D_CENTROID_MEAN]);

    float t[NDESC], w[NDESC];
    /* Map navigation: xpos/ypos across centroid×pitch, k=1 radius=0 → exact nearest */
    query_target_map(&q, D_CENTROID_MEAN, D_PITCH, 0.0f, 0.0f, t, w);
    check("map xpos=0 → slice 0", query_select(&q, t, w, 1, 0.0f, 0.35f) == 0, NULL);
    query_target_map(&q, D_CENTROID_MEAN, D_PITCH, 0.5f, 0.5f, t, w);
    check("map xpos=0.5 → slice 2", query_select(&q, t, w, 1, 0.0f, 0.35f) == 2, NULL);
    query_target_map(&q, D_CENTROID_MEAN, D_PITCH, 1.0f, 1.0f, t, w);
    check("map xpos=1 → slice 4", query_select(&q, t, w, 1, 0.0f, 0.35f) == 4, NULL);
    query_target_map(&q, D_CENTROID_MEAN, D_PITCH, 0.30f, 0.30f, t, w);   /* ~220 → nearest 200 */
    check("map xpos=0.3 → slice 1", query_select(&q, t, w, 1, 0.0f, 0.35f) == 1, NULL);

    /* Repetition avoidance: k=5, radius=1 (uniform among all), ring=3 */
    query_set_repeat_avoid(&q, 3);
    query_target_map(&q, D_CENTROID_MEAN, D_PITCH, 0.5f, 0.5f, t, w);
    int seq[24]; int in_range = 1, no_repeat = 1;
    for (int i = 0; i < 24; i++) {
        seq[i] = query_select(&q, t, w, 5, 1.0f, 0.35f);
        if (seq[i] < 0 || seq[i] >= NS) in_range = 0;
        for (int b = 1; b <= 3 && i - b >= 0; b++) if (seq[i] == seq[i - b]) no_repeat = 0;
    }
    printf("    seq:");
    for (int i = 0; i < 24; i++) printf(" %d", seq[i]);
    printf("\n");
    check("repeat-avoid: all picks valid", in_range, NULL);
    check("repeat-avoid: no repeat within ring(3)", no_repeat, NULL);

    /* Variety: with radius=1 and no ring, selection should not be constant */
    query_set_repeat_avoid(&q, 0);
    int seen[5] = {0};
    for (int i = 0; i < 200; i++) { int s = query_select(&q, t, w, 5, 1.0f, 0.35f); if (s >= 0 && s < 5) seen[s] = 1; }
    int nseen = 0; for (int i = 0; i < 5; i++) nseen += seen[i];
    check("radius=1 explores multiple slices", nseen >= 4, "%d/5 seen", nseen);

    /* radius=0 is deterministic nearest even with k=5 */
    int a0 = query_select(&q, t, w, 5, 0.0f, 0.35f);
    int a1 = query_select(&q, t, w, 5, 0.0f, 0.35f);
    check("radius=0 deterministic nearest (slice 2)", a0 == 2 && a1 == 2, "%d,%d", a0, a1);

    /* Mosaic: full target = copy of slice 3's descriptor → returns slice 3 */
    query_target_full(&qn[3 * NDESC], t, w);
    check("mosaic full-vector target → slice 3", query_select(&q, t, w, 1, 0.0f, 0.35f) == 3, NULL);

    /* Concatenation cost: consecutive picks should stay near each other (smooth),
     * i.e. mostly ±1 slice on the 100-spaced line, vs ~200 for random jumps. */
    query_init(&q, qn, NS);                 /* reset last_sel */
    query_set_repeat_avoid(&q, 0);
    query_target_map(&q, D_CENTROID_MEAN, D_PITCH, 0.5f, 0.5f, t, w);   /* centre target */
    int prev = query_select(&q, t, w, 5, 1.0f, 0.35f);
    double stepsum = 0.0; int steps = 0, sseen[5] = {0};
    for (int i = 0; i < 80; i++) {
        int s = query_select(&q, t, w, 5, 1.0f, 0.35f);
        stepsum += fabs(qn[s * NDESC + D_CENTROID_MEAN] - qn[prev * NDESC + D_CENTROID_MEAN]);
        steps++; if (s >= 0 && s < 5) sseen[s] = 1; prev = s;
    }
    double avg_step = stepsum / steps;
    int nvisit = 0; for (int i = 0; i < 5; i++) nvisit += sseen[i];
    check("concat: consecutive picks are neighbours (smooth)", avg_step < 160.0, "avg step %.0f (adjacent=100)", avg_step);
    check("concat: still wanders the corpus (not stuck)", nvisit >= 3, "%d/5 visited", nvisit);

    printf("\n%d passed, %d failed\n", npass, nfail);
    return nfail ? 1 : 0;
}
