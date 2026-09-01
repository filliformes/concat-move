/**
 * Concat — descriptor-space query (clean-room, first-party MIT). See query.h.
 * Render-callable: no allocation, no locks.
 */
#include "query.h"
#include <string.h>
#include <math.h>

static unsigned q_rand(query_t *q) {
    unsigned x = q->rng ? q->rng : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    q->rng = x; return x;
}

static void push_ring(query_t *q, int idx) {
    if (q->ring_cap <= 0) return;
    q->ring[q->ring_head] = idx;
    q->ring_head = (q->ring_head + 1) % q->ring_cap;
    if (q->ring_count < q->ring_cap) q->ring_count++;
}

static int in_ring(const query_t *q, int idx) {
    for (int r = 0; r < q->ring_count; r++) if (q->ring[r] == idx) return 1;
    return 0;
}

void query_init(query_t *q, const float *qn, int nslices) {
    memset(q, 0, sizeof(*q));
    q->qn = qn; q->nslices = nslices; q->rng = 0x1234567u;
    for (int d = 0; d < NDESC; d++) { q->nav_min[d] = 1e30f; q->nav_max[d] = -1e30f; }
    for (int i = 0; i < nslices; i++)
        for (int d = 0; d < NDESC; d++) {
            float v = qn[i * NDESC + d];
            if (v < q->nav_min[d]) q->nav_min[d] = v;
            if (v > q->nav_max[d]) q->nav_max[d] = v;
        }
}

void query_set_repeat_avoid(query_t *q, int n) {
    if (n < 0) n = 0;
    if (n > QUERY_RING_MAX) n = QUERY_RING_MAX;
    q->ring_cap = n; q->ring_count = 0; q->ring_head = 0;
}

void query_target_map(const query_t *q, int dx, int dy, float xpos, float ypos,
                      float *t, float *w) {
    memset(t, 0, sizeof(float) * NDESC);
    memset(w, 0, sizeof(float) * NDESC);
    if (dx >= 0 && dx < NDESC) { t[dx] = q->nav_min[dx] + xpos * (q->nav_max[dx] - q->nav_min[dx]); w[dx] = 1.0f; }
    if (dy >= 0 && dy < NDESC) { t[dy] = q->nav_min[dy] + ypos * (q->nav_max[dy] - q->nav_min[dy]); w[dy] = 1.0f; }
}

void query_target_full(const float *robust_vec, float *t, float *w) {
    for (int d = 0; d < NDESC; d++) { t[d] = robust_vec[d]; w[d] = 1.0f; }
}

static float wdist2(const float *t, const float *w, const float *v) {
    float d = 0.0f;
    for (int i = 0; i < NDESC; i++) { float diff = t[i] - v[i]; d += w[i] * diff * diff; }
    return d;
}

int query_select(query_t *q, const float *t, const float *w, int k, float radius) {
    if (q->nslices <= 0) return -1;
    if (k < 1) k = 1;
    if (k > QUERY_RING_MAX) k = QUERY_RING_MAX;

    float bd[QUERY_RING_MAX]; int bi[QUERY_RING_MAX]; int nb = 0;  /* K-best, ascending */
    for (int i = 0; i < q->nslices; i++) {
        if (in_ring(q, i)) continue;
        float d = wdist2(t, w, &q->qn[i * NDESC]);
        if (nb < k) {
            int p = nb++;
            while (p > 0 && bd[p - 1] > d) { bd[p] = bd[p - 1]; bi[p] = bi[p - 1]; p--; }
            bd[p] = d; bi[p] = i;
        } else if (d < bd[k - 1]) {
            int p = k - 1;
            while (p > 0 && bd[p - 1] > d) { bd[p] = bd[p - 1]; bi[p] = bi[p - 1]; p--; }
            bd[p] = d; bi[p] = i;
        }
    }

    if (nb == 0) {                          /* every candidate is ring-forbidden: nearest overall */
        int best = -1; float bestd = 1e30f;
        for (int i = 0; i < q->nslices; i++) {
            float d = wdist2(t, w, &q->qn[i * NDESC]);
            if (d < bestd) { bestd = d; best = i; }
        }
        if (best >= 0) push_ring(q, best);
        return best;
    }

    if (radius < 0.0f) radius = 0.0f;
    if (radius > 1.0f) radius = 1.0f;
    int pool = (int)ceilf(1.0f + radius * (nb - 1));   /* radius widens pool 1..nb */
    if (pool < 1) pool = 1;
    if (pool > nb) pool = nb;
    int pick = (pool == 1) ? 0 : (int)(q_rand(q) % (unsigned)pool);
    int idx = bi[pick];
    push_ring(q, idx);
    return idx;
}
