/**
 * Concat — descriptor-space query (clean-room, first-party MIT).
 *
 * kNN selection in the (RobustScaled) descriptor space with random-among-K
 * within a radius and CataRT-style repetition avoidance. Render-callable:
 * NO allocation, NO locks — a linear scan over the resident descriptor array.
 *
 * The descriptor array `qn` (nslices × NDESC) is owned by the corpus/pool and
 * must stay valid for the query's lifetime. Query operates purely on it.
 */
#ifndef CONCAT_QUERY_H
#define CONCAT_QUERY_H

#include "corpus.h"   /* NDESC */

#define QUERY_RING_MAX 16
#define QUERY_CAND_MAX (2 * QUERY_RING_MAX)   /* k nearest + room for ring-forbidden ones */

/* w[0] == QUERY_W_PLANE selects the learned 2-D PCA plane instead of descriptor space:
 * t[0], t[1] are plane coordinates (pca_min/max units), the rest of t/w is ignored.
 * Only valid when the query has a projection bound (q->proj != NULL). */
#define QUERY_W_PLANE (-1.0f)

/* Search cost budget, in multiply-adds (slices x active dims). A search under it runs
 * in full inside query_select. A bigger one (36-dim Mosaic/pad targets on a large
 * corpus) is spread over render blocks by query_tick, serving the previous
 * candidates until it completes, so no block pays for a whole scan. */
#define QUERY_SCAN_BUDGET (1 << 18)

/* Candidate lists kept at once: one per held note (Notes polyphony), so each voice
 * reuses its own list while its target holds still. */
#define QUERY_SLOTS 16

/* The nearest candidates for one target, ring-forbidden slices included (filtered at
 * pick time), sorted by distance. */
typedef struct {
    int   valid, plane, m, n;           /* m = list size asked for, n = found */
    int   idx[QUERY_CAND_MAX];
    float d[QUERY_CAND_MAX];
    float t[NDESC], w[NDESC];           /* the target this list belongs to */
} query_cands_t;

typedef struct {
    int          nslices;
    const float *qn;                 /* [nslices*NDESC] RobustScaled descriptors */
    float       *qt;                 /* [NDESC][nslices] column copy of qn: a 2-dim scan reads two
                                      * contiguous arrays, not one float per 144-byte row (owned) */
    const float *proj;               /* [nslices*2] PCA-plane coords, or NULL (owned by the corpus) */
    query_cands_t cache[QUERY_SLOTS];/* recent completed searches (reused while a target holds still) */
    int          cache_next;         /* round-robin slot to overwrite */
    int          last_slot;          /* slot served most recently (stale fallback) */
    query_cands_t scan;              /* search in progress (budgeted) */
    int          scan_active, scan_pos;
    long         blk_cost;           /* search work done this render block (reset by query_tick) */
    float        nav_min[NDESC];     /* per-dim min/max over qn (0..1 nav mapping) */
    float        nav_max[NDESC];

    int          ring[QUERY_RING_MAX];  /* recently-played slice indices (forbidden) */
    int          ring_cap;              /* = repeat_avoid, 0..QUERY_RING_MAX */
    int          ring_count;
    int          ring_head;
    int          last_sel;              /* last chosen slice (concatenation-cost continuity), -1 = none */
    unsigned     rng;
} query_t;

/* Bind the query to a descriptor array and precompute nav min/max + the column copy.
 * Allocates (worker thread only); release with query_free. */
void query_init(query_t *q, const float *qn, int nslices);
void query_free(query_t *q);

/* Set repetition-avoidance depth (recently-played slices excluded). Clears ring. */
void query_set_repeat_avoid(query_t *q, int n);

/* Map mode: build a target `t[NDESC]` and weight mask `w[NDESC]` that constrain
 * only the two nav descriptors dx,dy (0..1 positions mapped across the corpus). */
void query_target_map(const query_t *q, int dx, int dy, float xpos, float ypos,
                      float *t, float *w);

/* Mosaic mode: target = a full RobustScaled descriptor vector, all dims weighted. */
void query_target_full(const float *robust_vec, float *t, float *w);

/* Select a slice index for target `t` / weights `w`: the k nearest by weighted
 * Euclidean distance, then a random pick among the first ceil(1+radius*(k-1)) of
 * them, excluding the repeat-avoid ring. Returns a slice index, or -1 if empty.
 * Pushes the chosen index onto the ring. */
int  query_select(query_t *q, const float *t, const float *w, int k, float radius, float smooth);

/* Start of a render block: advance a budgeted search by one budget and reset the
 * per-block search allowance (fresh searches beyond QUERY_SCAN_BUDGET per block wait
 * for the next block, so a 16-pad chord does not run 16 searches in one block). */
void query_tick(query_t *q);

#endif /* CONCAT_QUERY_H */
