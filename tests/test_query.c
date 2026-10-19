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

    /* Big corpus, full 36-dim target: over the budget, so the search is spread over
     * query_tick calls. No grain until the first search completes, then the exact
     * brute-force nearest; ticks per search stay bounded. */
    {
        const int NB = 20000;
        float *big = malloc(sizeof(float) * NDESC * NB);
        unsigned r = 12345u;
        for (int i = 0; i < NB * NDESC; i++) { r = r * 1664525u + 1013904223u; big[i] = (r >> 8) * (1.0f / 16777216.0f) * 4.0f - 2.0f; }
        query_t qb; query_init(&qb, big, NB); query_set_repeat_avoid(&qb, 0);
        float tb[NDESC], wb[NDESC];
        query_target_full(&big[777 * NDESC], tb, wb);
        tb[0] += 0.01f;                                    /* near 777, not exactly on it */
        int first = query_select(&qb, tb, wb, 1, 0.0f, 0.0f);
        int ticks = 0;
        while (qb.scan_active && ticks < 100) { query_tick(&qb); ticks++; }
        int got = query_select(&qb, tb, wb, 1, 0.0f, 0.0f);
        int best = -1; float bestd = 1e30f;
        for (int i = 0; i < NB; i++) {
            float d = 0; for (int j = 0; j < NDESC; j++) { float df = tb[j] - big[i * NDESC + j]; d += df * df; }
            if (d < bestd) { bestd = d; best = i; }
        }
        check("budget: no grain before the first big search completes", first == -1, "first=%d", first);
        check("budget: search completes in a few blocks", ticks > 0 && ticks <= 4, "%d ticks", ticks);
        check("budget: result is the brute-force nearest", got == best, "got %d want %d", got, best);
        int again = query_select(&qb, tb, wb, 1, 0.0f, 0.0f);
        check("cache: unchanged target reuses the list (no new search)", again == best && !qb.scan_active, NULL);
        /* Moving target: previous candidates serve while the new search runs. */
        query_target_full(&big[4242 * NDESC], tb, wb);
        int stale = query_select(&qb, tb, wb, 1, 0.0f, 0.0f);
        check("budget: stale list serves during a new search", stale == best && qb.scan_active, "stale=%d", stale);
        while (qb.scan_active) query_tick(&qb);
        check("budget: new search lands on the new target", query_select(&qb, tb, wb, 1, 0.0f, 0.0f) == 4242, NULL);
        free(big);
    }

    /* Learned plane: w[0] = QUERY_W_PLANE searches the bound 2-D projection. */
    {
        static float proj[5 * 2] = { 0,0,  1,0,  0,1,  1,1,  0.5f,0.5f };
        query_t qp; query_init(&qp, qn, NS); query_set_repeat_avoid(&qp, 0);
        qp.proj = proj;
        float tp[NDESC] = {0}, wp[NDESC] = {0};
        tp[0] = 0.9f; tp[1] = 0.1f; wp[0] = QUERY_W_PLANE;
        check("plane: nearest in the PCA plane", query_select(&qp, tp, wp, 1, 0.0f, 0.0f) == 1, NULL);
        tp[0] = 0.45f; tp[1] = 0.55f;
        check("plane: moves with the target", query_select(&qp, tp, wp, 1, 0.0f, 0.0f) == 4, NULL);
    }

    /* Ring filter on cached candidates: repeat-avoid 2, k=1 never repeats within 3 picks. */
    {
        query_t qr; query_init(&qr, qn, NS); query_set_repeat_avoid(&qr, 2);
        query_target_map(&qr, D_CENTROID_MEAN, D_PITCH, 0.0f, 0.0f, t, w);
        int a = query_select(&qr, t, w, 1, 0.0f, 0.0f), b = query_select(&qr, t, w, 1, 0.0f, 0.0f), c = query_select(&qr, t, w, 1, 0.0f, 0.0f);
        check("ring: cached list still honours repeat-avoid", a == 0 && b == 1 && c == 2, "%d %d %d", a, b, c);
    }

    printf("\n%d passed, %d failed\n", npass, nfail);
    return nfail ? 1 : 0;
}
