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

typedef struct {
    int          nslices;
    const float *qn;                 /* [nslices*NDESC] RobustScaled descriptors */
    float        nav_min[NDESC];     /* per-dim min/max over qn (0..1 nav mapping) */
    float        nav_max[NDESC];

    int          ring[QUERY_RING_MAX];  /* recently-played slice indices (forbidden) */
    int          ring_cap;              /* = repeat_avoid, 0..QUERY_RING_MAX */
    int          ring_count;
    int          ring_head;
    unsigned     rng;
} query_t;

/* Bind the query to a descriptor array and precompute nav min/max. */
void query_init(query_t *q, const float *qn, int nslices);

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
int  query_select(query_t *q, const float *t, const float *w, int k, float radius);

#endif /* CONCAT_QUERY_H */
