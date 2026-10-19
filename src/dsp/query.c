/**
 * Concat — descriptor-space query (clean-room, first-party MIT). See query.h.
 * Render-callable: no allocation, no locks.
 */
#include "query.h"
#include <stdlib.h>
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
    q->qn = qn; q->nslices = nslices; q->rng = 0x1234567u; q->last_sel = -1;
    q->qt = nslices > 0 ? (float *)malloc(sizeof(float) * NDESC * (size_t)nslices) : NULL;
    if (q->qt)
        for (int i = 0; i < nslices; i++)
            for (int d = 0; d < NDESC; d++) q->qt[(size_t)d * nslices + i] = qn[(size_t)i * NDESC + d];
    for (int d = 0; d < NDESC; d++) { q->nav_min[d] = 1e30f; q->nav_max[d] = -1e30f; }
    for (int i = 0; i < nslices; i++)
        for (int d = 0; d < NDESC; d++) {
            float v = qn[i * NDESC + d];
            if (v < q->nav_min[d]) q->nav_min[d] = v;
            if (v > q->nav_max[d]) q->nav_max[d] = v;
        }
}

void query_free(query_t *q) { free(q->qt); q->qt = NULL; }

void query_set_repeat_avoid(query_t *q, int n) {
    if (n < 0) n = 0;
    if (n > QUERY_RING_MAX) n = QUERY_RING_MAX;
    if (n > q->nslices - 1) n = q->nslices - 1;   /* keep ≥1 slice selectable (avoid forbidding all) */
    if (n < 0) n = 0;
    if (n == q->ring_cap) return;                 /* unchanged — keep the ring (render calls this per block) */
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

/* ── Candidate search ────────────────────────────────────────────────────────
 * Distances only run over the dims a target actually weights (a raw X/Y Map target
 * weights 2 of 36, a Notes pitch target 1), Learned-map targets search the 2-D PCA
 * plane, and the K-nearest list is kept until the target moves, so a held pad or a
 * parked X/Y scans the corpus once instead of once per grain. */

static int active_dims(const float *w, int *dims) {
    int n = 0;
    for (int i = 0; i < NDESC; i++) if (w[i] != 0.0f) dims[n++] = i;
    return n;
}

static int is_plane(const query_t *q, const float *w) { return q->proj && w[0] == QUERY_W_PLANE; }

static int cost_per_slice(const query_cands_t *c) {
    if (c->plane) return 2;
    int dims[NDESC], nd = active_dims(c->w, dims);
    return nd > 0 ? nd : 1;
}

static int same_target(const query_cands_t *c, int plane, const float *t, const float *w) {
    if (!c->valid || c->plane != plane) return 0;
    if (plane) return c->t[0] == t[0] && c->t[1] == t[1];
    return memcmp(c->t, t, sizeof(float) * NDESC) == 0 && memcmp(c->w, w, sizeof(float) * NDESC) == 0;
}

static void cands_begin(query_cands_t *c, int plane, const float *t, const float *w, int m) {
    c->valid = 0; c->plane = plane; c->m = m; c->n = 0;
    memcpy(c->t, t, sizeof(float) * NDESC);
    memcpy(c->w, w, sizeof(float) * NDESC);
}

static inline void cands_insert(query_cands_t *c, float d, int i) {
    int p;
    if (c->n < c->m) p = c->n++;
    else if (d < c->d[c->m - 1]) p = c->m - 1;
    else return;
    while (p > 0 && c->d[p - 1] > d) { c->d[p] = c->d[p - 1]; c->idx[p] = c->idx[p - 1]; p--; }
    c->d[p] = d; c->idx[p] = i;
}

/* Fold slices [from, to) into the list. */
static void cands_scan(const query_t *q, query_cands_t *c, int from, int to) {
    if (c->plane) {
        const float *p = q->proj;
        const float tx = c->t[0], ty = c->t[1];
        for (int i = from; i < to; i++) {
            float dx = tx - p[2 * i], dy = ty - p[2 * i + 1];
            cands_insert(c, dx * dx + dy * dy, i);
        }
        return;
    }
    int dims[NDESC], nd = active_dims(c->w, dims);
    if (q->qt && nd <= 8) {                  /* few dims: stream their columns */
        const float *col[8]; float tv[8], wv[8];
        for (int j = 0; j < nd; j++) { col[j] = &q->qt[(size_t)dims[j] * q->nslices]; tv[j] = c->t[dims[j]]; wv[j] = c->w[dims[j]]; }
        for (int i = from; i < to; i++) {
            float d = 0.0f;
            for (int j = 0; j < nd; j++) { float df = tv[j] - col[j][i]; d += wv[j] * df * df; }
            if (c->n < c->m || d < c->d[c->m - 1]) cands_insert(c, d, i);
        }
        return;
    }
    for (int i = from; i < to; i++) {        /* many dims: rows are read whole anyway */
        const float *v = &q->qn[(size_t)i * NDESC];
        float d = 0.0f;
        for (int j = 0; j < nd; j++) { int k = dims[j]; float df = c->t[k] - v[k]; d += c->w[k] * df * df; }
        cands_insert(c, d, i);
    }
}

void query_tick(query_t *q) {
    q->blk_cost = 0;
    if (!q->scan_active) return;
    int step = QUERY_SCAN_BUDGET / cost_per_slice(&q->scan);
    if (step < 1) step = 1;
    int to = q->scan_pos + step;
    if (to > q->nslices) to = q->nslices;
    cands_scan(q, &q->scan, q->scan_pos, to);
    q->scan_pos = to;
    if (to >= q->nslices) {
        q->scan.valid = 1;
        int slot = q->cache_next; q->cache_next = (q->cache_next + 1) % QUERY_SLOTS;
        q->cache[slot] = q->scan; q->last_slot = slot;
        q->scan_active = 0;
    }
}

/* Find or make a candidate list for target (t,w) with at least m entries: reuse a slot,
 * search now if that is cheap, or queue a budgeted search and serve the most recent
 * list meanwhile. Returns the slot to pick from (may be invalid: nothing yet). */
static const query_cands_t *refresh_cands(query_t *q, const float *t, const float *w, int m) {
    int plane = is_plane(q, w);
    for (int s = 0; s < QUERY_SLOTS; s++)
        if (same_target(&q->cache[s], plane, t, w) && q->cache[s].m >= m) { q->last_slot = s; return &q->cache[s]; }
    query_cands_t probe; probe.plane = plane; memcpy(probe.w, w, sizeof(float) * NDESC);
    long cost = (long)q->nslices * cost_per_slice(&probe);
    if (cost <= QUERY_SCAN_BUDGET) {
        if (q->blk_cost > 0 && q->blk_cost + cost > QUERY_SCAN_BUDGET) return NULL;   /* next block */
        q->blk_cost += cost;
        int s = q->cache_next; q->cache_next = (q->cache_next + 1) % QUERY_SLOTS;
        query_cands_t *c = &q->cache[s];
        cands_begin(c, plane, t, w, m);
        cands_scan(q, c, 0, q->nslices);
        c->valid = 1; q->last_slot = s;
        return c;
    }
    /* A running search finishes its own target first (restarting on every move would
     * never finish under a moving Mosaic target); the next call queues the latest. */
    if (!q->scan_active) {
        cands_begin(&q->scan, plane, t, w, m);
        q->scan_active = 1; q->scan_pos = 0;
        query_tick(q);                       /* make a start now */
        for (int s = 0; s < QUERY_SLOTS; s++)   /* it may have completed right away */
            if (same_target(&q->cache[s], plane, t, w) && q->cache[s].m >= m) { q->last_slot = s; return &q->cache[s]; }
    }
    return &q->cache[q->last_slot];
}

/* Full-space squared distance between two slices (concatenation cost). */
static float slice_dist2(const query_t *q, int a, int b) {
    const float *va = &q->qn[a * NDESC], *vb = &q->qn[b * NDESC];
    float d = 0.0f;
    for (int i = 0; i < NDESC; i++) { float diff = va[i] - vb[i]; d += diff * diff; }
    return d;
}

/* CataRT-style concatenation cost weight (blend of target cost and continuity with the
 * last-played slice): smooths grain-to-grain jumps while `radius` still governs variety.
 * Now dialable via the Smooth knob (0 = pure target nearest, 1 = maximum continuity);
 * default 0.35. */
int query_select(query_t *q, const float *t, const float *w, int k, float radius, float smooth) {
    if (q->nslices <= 0) return -1;
    if (k < 1) k = 1;
    if (k > QUERY_RING_MAX) k = QUERY_RING_MAX;
    if (smooth < 0.0f) smooth = 0.0f;
    if (smooth > 1.0f) smooth = 1.0f;

    int m = k + q->ring_cap;                 /* enough that k survive the ring filter */
    if (m > QUERY_CAND_MAX) m = QUERY_CAND_MAX;
    if (m > q->nslices) m = q->nslices;
    const query_cands_t *c = refresh_cands(q, t, w, m);
    if (!c || !c->valid || c->n == 0) return -1;   /* first big search still running: no grain yet */

    float bd[QUERY_RING_MAX]; int bi[QUERY_RING_MAX]; int nb = 0;  /* K-best, ascending */
    for (int j = 0; j < c->n && nb < k; j++) {
        if (in_ring(q, c->idx[j])) continue;
        bd[nb] = c->d[j]; bi[nb] = c->idx[j]; nb++;
    }

    if (nb == 0) {                          /* every candidate is ring-forbidden: nearest overall */
        int best = -1;
        for (int j = 0; j < c->n; j++)
            if (c->idx[j] != q->last_sel) { best = c->idx[j]; break; }   /* avoid an immediate repeat */
        if (best < 0) best = q->last_sel;   /* only last_sel is available (tiny corpus) */
        if (best >= 0) { push_ring(q, best); q->last_sel = best; }
        return best;
    }

    if (radius < 0.0f) radius = 0.0f;
    if (radius > 1.0f) radius = 1.0f;
    int pool = (int)ceilf(1.0f + radius * (nb - 1));   /* radius widens pool 1..nb */
    if (pool < 1) pool = 1;
    if (pool > nb) pool = nb;

    int idx;
    if (pool == 1 || q->last_sel < 0 || q->last_sel >= q->nslices) {
        idx = bi[(pool == 1) ? 0 : (int)(q_rand(q) % (unsigned)pool)];
    } else {
        /* Concatenation cost: weighted-random among the radius pool, biased toward low
         * (target + continuity) cost. Smooths grain-to-grain jumps while `radius` still
         * governs spread and every candidate stays reachable (exploration preserved). */
        float cd[QUERY_RING_MAX];
        float tmin = bd[0], tmax = bd[0], cmin = 1e30f, cmax = -1e30f;
        for (int j = 0; j < pool; j++) {
            cd[j] = slice_dist2(q, bi[j], q->last_sel);
            if (bd[j] < tmin) tmin = bd[j];
            if (bd[j] > tmax) tmax = bd[j];
            if (cd[j] < cmin) cmin = cd[j];
            if (cd[j] > cmax) cmax = cd[j];
        }
        float tr = tmax > tmin ? tmax - tmin : 1.0f, cr = cmax > cmin ? cmax - cmin : 1.0f;
        float wt[QUERY_RING_MAX], wsum = 0.0f;
        for (int j = 0; j < pool; j++) {
            if (bi[j] == q->last_sel) { wt[j] = 0.0f; continue; }   /* never re-pick the same slice */
            float comb = (1.0f - smooth) * (bd[j] - tmin) / tr + smooth * (cd[j] - cmin) / cr;
            wt[j] = 1.0f - comb; if (wt[j] < 0.05f) wt[j] = 0.05f;   /* floor keeps all reachable */
            wsum += wt[j];
        }
        float r = ((q_rand(q) >> 8) * (1.0f / 16777216.0f)) * wsum, acc = 0.0f;
        idx = bi[0];
        for (int j = 0; j < pool; j++) { if (wt[j] <= 0.0f) continue; acc += wt[j]; if (r <= acc) { idx = bi[j]; break; } }
    }
    push_ring(q, idx);
    q->last_sel = idx;
    return idx;
}
