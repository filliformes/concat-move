/**
 * Concat — corpus engine (clean-room MIT). See corpus_engine.h. Offline only.
 */
#include "corpus_engine.h"
#include "ingest.h"
#include <stdlib.h>
#include <string.h>

#define MAX_FILES        2048
#define MAX_SLICES_FILE  4096

void corpus_index_free(corpus_index_t *ix) {
    if (!ix) return;
    free(ix->pool);
    free(ix->slices);
    free(ix->qn);
    free(ix);
}

corpus_index_t *corpus_index_build(const char *folder, int sr,
                                   int slice_mode, int min_len, int max_len,
                                   float sil_gate, int pool_cap_samples,
                                   volatile int *progress) {
    if (progress) *progress = 0;

    char (*paths)[INGEST_PATH] = (char (*)[INGEST_PATH])malloc(sizeof(char[INGEST_PATH]) * MAX_FILES);
    if (!paths) return NULL;
    int nfiles = ingest_list_audio(folder, paths, MAX_FILES);
    if (nfiles <= 0) { free(paths); return NULL; }

    analysis_t *a = analysis_create(sr, 1024, 40, 13);
    corpus_index_t *ix = (corpus_index_t *)calloc(1, sizeof(corpus_index_t));
    slice_range_t *ranges = (slice_range_t *)malloc(sizeof(slice_range_t) * MAX_SLICES_FILE);
    if (!a || !ix || !ranges) { free(paths); free(ranges); analysis_destroy(a); free(ix); return NULL; }

    ix->pool = (int16_t *)malloc(sizeof(int16_t) * (size_t)pool_cap_samples);
    ix->pool_cap = pool_cap_samples;
    int scap = 1024;
    ix->slices = (cslice_t *)malloc(sizeof(cslice_t) * scap);
    float *raw = (float *)malloc(sizeof(float) * NDESC * scap);
    if (!ix->pool || !ix->slices || !raw) {
        free(paths); free(ranges); free(raw); analysis_destroy(a);
        corpus_index_free(ix); return NULL;
    }

    int pool_full = 0;
    for (int f = 0; f < nfiles && !pool_full; f++) {
        int len = 0;
        float *audio = ingest_decode(paths[f], sr, &len);
        if (audio && len > 0) {
            int nr = (slice_mode == 1)
                ? concat_fixed_slice(a, audio, len, max_len, sil_gate, ranges, MAX_SLICES_FILE)
                : concat_onset_slice(a, audio, len, min_len, max_len, sil_gate, ranges, MAX_SLICES_FILE);
            for (int s = 0; s < nr; s++) {
                int slen = ranges[s].len;
                if (ix->pool_len + slen > ix->pool_cap) { pool_full = 1; break; }
                if (ix->nslices >= scap) {
                    int ns = scap * 2;
                    cslice_t *nsl = (cslice_t *)realloc(ix->slices, sizeof(cslice_t) * ns);
                    float    *nr2 = (float *)realloc(raw, sizeof(float) * NDESC * ns);
                    if (!nsl || !nr2) { pool_full = 1; if (nsl) ix->slices = nsl; if (nr2) raw = nr2; break; }
                    ix->slices = nsl; raw = nr2; scap = ns;
                }
                const float *src = &audio[ranges[s].start];
                int off = ix->pool_len;
                for (int i = 0; i < slen; i++) {
                    float v = src[i];
                    if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                    ix->pool[off + i] = (int16_t)(v * 32767.0f);
                }
                ix->slices[ix->nslices].pool_off = off;
                ix->slices[ix->nslices].len = slen;
                concat_slice_descriptor(a, src, slen, &raw[ix->nslices * NDESC]);
                ix->slices[ix->nslices].pitch = raw[ix->nslices * NDESC + D_PITCH];  /* for Notes retune */
                ix->nslices++;
                ix->pool_len += slen;
            }
            free(audio);
        }
        if (progress) *progress = (int)((long)(f + 1) * 100 / nfiles);
    }

    free(paths); free(ranges); analysis_destroy(a);

    if (ix->nslices == 0) { free(raw); corpus_index_free(ix); return NULL; }

    concat_robust_fit(raw, ix->nslices, ix->median, ix->iqr);
    ix->qn = (float *)malloc(sizeof(float) * NDESC * ix->nslices);
    if (!ix->qn) { free(raw); corpus_index_free(ix); return NULL; }
    for (int i = 0; i < ix->nslices; i++)
        concat_robust_apply(&raw[i * NDESC], ix->median, ix->iqr, &ix->qn[i * NDESC]);
    free(raw);

    query_init(&ix->query, ix->qn, ix->nslices);
    if (progress) *progress = 100;
    return ix;
}
