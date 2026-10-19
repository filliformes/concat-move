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
#include "corpus_file.h"

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
    int       pca_ok;      /* learned 2-D map (PCA) available */
    float     pca_mean[NDESC];      /* descriptor-space mean */
    float     pca_axis[2][NDESC];   /* top-2 principal axes (unit vectors) */
    float     pca_min[2], pca_max[2];  /* projection range, for 0..1 nav mapping */
    char      scatter_bmp[600];    /* on-disk 128x64 8-bit BMP scatter (legacy; unused by as_page) */
    char      map_pts[4096];       /* cached scatter as int 0..1000 "x,y;..." for the View canvas
                                    * (built once per load by the DSP; runtime-only, not cached to disk) */
    float    *proj;        /* [nslices*2] PCA-plane coords (pca_min/max units); NULL when !pca_ok.
                            * Learned-map queries run in this 2-D space (runtime-only). */
    uint32_t  gen;         /* unique per built/loaded index: the render thread flushes grains
                            * when it changes (pointer compare alone can alias a freed index) */
} corpus_index_t;

/* Streamed decode window: slicing runs per window from the range start, so this is
 * part of what makes two analyses identical (device build and Corpus Builder). */
#define CONCAT_WIN_SECONDS 60

/* ── Build report ────────────────────────────────────────────────────────────
 * Filled by corpus_index_build so the UI can say what happened. Fields marked
 * volatile are updated live while the build runs (read by other threads). */
enum { CB_IDLE = 0, CB_SCAN, CB_LOAD, CB_ANALYSE, CB_FINAL, CB_SAVE, CB_DONE };
enum { CB_OK = 0, CB_ERR_NOFILES, CB_ERR_NOAUDIO, CB_ERR_GATED, CB_ERR_MEM, CB_ERR_CANCEL, CB_ERR_SPEC };
typedef struct {
    volatile int phase;          /* CB_* */
    volatile int files_total;
    volatile int files_done;
    volatile int progress;       /* 0..100 overall (100 only once the index is complete) */
    int nslices;                 /* slices in the result */
    int files_failed;            /* could not be opened/decoded */
    int files_skipped;           /* not reached because the pool filled */
    int from_cache;              /* 1 = loaded from the disk cache */
    int mem_limited;             /* pool was capped below the user's Pool Cap by free RAM */
    int err;                     /* CB_ERR_* when NULL is returned */
    int sources_missing;         /* .concat sources that did not resolve */
    int files_cached;            /* files whose slices/descriptors came from the per-file cache */
    unsigned long long watch_stamp;   /* corpus_watch_stamp() taken right after the sources
                                       * were read: a later edit always differs from it */
    corpus_spec_t *spec;         /* optional, caller-owned: receives the resolved spec (name,
                                  * slicing/mask overrides, watch list, parse error). Kept
                                  * across the reset at the start of a build. */
} corpus_build_info_t;

/* Build an index+pool from every audio file in `folder`, the single file `folder`, or the
 * sources of the .concat file `folder` (see corpus_file.h; its slicing overrides the args).
 *   slice_mode: 0 = onset, 1 = fixed-grain (grain length = max_len).
 *   min_len/max_len: slice clamps in samples. sil_gate: RMS floor.
 *   pool_cap_samples: the user's pool cap. A cached index is valid up to this size.
 *   mem_cap_samples: what free RAM allows right now (0 = no limit). When it is below
 *     pool_cap_samples the build is decimated to fit and NOT written to the cache, so a
 *     tight-memory evening never overwrites the full cached corpus.
 *   cache_dir: if non-NULL, an on-disk index cache lives here (keyed by folder path;
 *     validated by a signature over file mtimes/sizes + slicing params).
 *   force_rescan: ignore any cached index and rebuild (still rewrites the cache).
 *   info: optional report (see above).
 * Decoding is streamed in windows, so memory stays bounded for any file length.
 * Returns a heap index (free with corpus_index_free), or NULL (see info->err). */
corpus_index_t *corpus_index_build(const char *folder, int sr,
                                   int slice_mode, int min_len, int max_len,
                                   float sil_gate, int pool_cap_samples, int mem_cap_samples,
                                   const char *cache_dir, int force_rescan,
                                   corpus_build_info_t *info);

/* MemAvailable in MB from /proc/meminfo (0 if unreadable). Worker threads only. */
long corpus_mem_available_mb(void);

void corpus_index_free(corpus_index_t *ix);

#endif /* CONCAT_CORPUS_ENGINE_H */
