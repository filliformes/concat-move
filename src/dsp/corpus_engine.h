/**
 * Concat — corpus engine (clean-room MIT): folder → RAM pool + descriptor index.
 *
 * Ties the tested kernels together (ingest decode → onset/fixed slice → per-slice
 * MultiStats descriptor → RobustScaling → query_init). Offline (analyst thread)
 * only — decodes from disk and allocates. The resulting index is read-only from
 * the render thread (grains read pool[], query reads qn[]).
 */
#ifndef CONCAT_CORPUS_ENGINE_H
#define CONCAT_CORPUS_ENGINE_H

#include <stdint.h>
#include "analysis.h"
#include "corpus.h"
#include "query.h"

typedef struct { int pool_off; int len; float pitch; } cslice_t;  /* pool location + raw pitch (Hz, 0=unvoiced) */

typedef struct {
    int16_t  *pool;        /* mono int16 audio, all retained slices concatenated */
    int       pool_cap;    /* samples allocated */
    int       pool_len;    /* samples used */
    cslice_t *slices;
    int       nslices;
    float    *qn;          /* [nslices*NDESC] RobustScaled descriptors (query space) */
    float     median[NDESC], iqr[NDESC];
    query_t   query;       /* bound to qn */
    int       kclusters;   /* k-means clusters over qn (pad-jump targets); 0 if none */
    float    *centroids;   /* [kclusters*NDESC] cluster centres in query space */
} corpus_index_t;

/* Build an index+pool from every audio file in `folder`.
 *   slice_mode: 0 = onset, 1 = fixed-grain (grain length = max_len).
 *   min_len/max_len: slice clamps in samples. sil_gate: RMS floor.
 *   pool_cap_samples: hard cap on pooled audio (stops adding when full — v1, no eviction).
 *   cache_dir: if non-NULL, an on-disk index cache lives here (keyed by folder path;
 *     validated by a signature over file mtimes/sizes + slicing params). A hit loads
 *     the index directly (skips decode+analysis); a miss analyses then writes the cache.
 *   force_rescan: if non-zero, ignore any cached index and rebuild (still rewrites the cache).
 *   progress: optional 0..100 counter updated as files are processed.
 * Returns a heap index (free with corpus_index_free), or NULL if no audio / OOM. */
corpus_index_t *corpus_index_build(const char *folder, int sr,
                                   int slice_mode, int min_len, int max_len,
                                   float sil_gate, int pool_cap_samples,
                                   const char *cache_dir, int force_rescan,
                                   volatile int *progress);

void corpus_index_free(corpus_index_t *ix);

#endif /* CONCAT_CORPUS_ENGINE_H */
