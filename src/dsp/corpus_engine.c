/**
 * Concat — corpus engine (clean-room MIT). See corpus_engine.h. Offline only.
 */
#include "corpus_engine.h"
#include "ingest.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>

#define MAX_FILES        2048
#define MAX_SLICES_FILE  4096

/* ── On-disk index cache ─────────────────────────────────────────────────────
 * A folder's analysed index (pool + descriptors) is cached as one binary file.
 * The cache is validated by a signature over each file's mtime/size plus the
 * slicing params, so an unchanged folder reloads in a disk read instead of a
 * full decode+analysis. Device-local aarch64 only (raw struct dump). */

#define CACHE_MAGIC 0x54434E43u   /* "CNCT" */
#define CACHE_VER   2u            /* v2 adds k-means centroids */

typedef struct {
    uint32_t magic, ver;
    uint64_t sig;
    int32_t  ndesc, nslices, pool_len, kclusters;
} cache_hdr_t;

static uint64_t fnv1a(uint64_t h, const void *data, int n) {
    const unsigned char *p = (const unsigned char *)data;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

/* Order-independent content signature: XOR of per-file (path+mtime+size) hashes,
 * mixed with the slicing params. Returns 0 (→ "no signature") if any stat fails. */
static uint64_t corpus_signature(char (*paths)[INGEST_PATH], int nfiles,
                                 int slice_mode, int min_len, int max_len, float sil_gate, int sr) {
    uint64_t x = 0;
    for (int f = 0; f < nfiles; f++) {
        struct stat st;
        if (stat(paths[f], &st) != 0) return 0;
        uint64_t h = fnv1a(0xcbf29ce484222325ULL, paths[f], (int)strlen(paths[f]));
        long mt = (long)st.st_mtime, sz = (long)st.st_size;
        h = fnv1a(h, &mt, sizeof(mt));
        h = fnv1a(h, &sz, sizeof(sz));
        x ^= h;
    }
    int parms[4] = { slice_mode, min_len, max_len, sr };
    uint64_t s = fnv1a(0xcbf29ce484222325ULL, &x, sizeof(x));
    s = fnv1a(s, parms, sizeof(parms));
    s = fnv1a(s, &sil_gate, sizeof(sil_gate));
    return s ? s : 1;   /* never 0 (reserved) */
}

static void cache_path(char *out, int outsz, const char *cache_dir, const char *folder) {
    uint64_t h = fnv1a(0xcbf29ce484222325ULL, folder, (int)strlen(folder));
    snprintf(out, outsz, "%s/concat_%016llx.idx", cache_dir, (unsigned long long)h);
}

/* Load a cached index if the signature matches and it still fits the pool cap. */
static corpus_index_t *cache_load(const char *cache_dir, const char *folder,
                                  uint64_t sig, int pool_cap_samples) {
    char path[600]; cache_path(path, sizeof(path), cache_dir, folder);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    cache_hdr_t hd;
    if (fread(&hd, sizeof(hd), 1, fp) != 1 ||
        hd.magic != CACHE_MAGIC || hd.ver != CACHE_VER || hd.ndesc != NDESC ||
        hd.sig != sig || hd.nslices <= 0 || hd.pool_len <= 0 ||
        hd.pool_len > pool_cap_samples) { fclose(fp); return NULL; }

    corpus_index_t *ix = (corpus_index_t *)calloc(1, sizeof(corpus_index_t));
    if (!ix) { fclose(fp); return NULL; }
    ix->nslices = hd.nslices; ix->pool_len = hd.pool_len; ix->pool_cap = hd.pool_len;
    ix->kclusters = hd.kclusters;
    ix->pool   = (int16_t *)malloc(sizeof(int16_t) * (size_t)hd.pool_len);
    ix->slices = (cslice_t *)malloc(sizeof(cslice_t) * (size_t)hd.nslices);
    ix->qn     = (float *)malloc(sizeof(float) * (size_t)NDESC * hd.nslices);
    if (hd.kclusters > 0) ix->centroids = (float *)malloc(sizeof(float) * (size_t)hd.kclusters * NDESC);
    if (!ix->pool || !ix->slices || !ix->qn || (hd.kclusters > 0 && !ix->centroids)) {
        fclose(fp); corpus_index_free(ix); return NULL;
    }

    size_t nq = (size_t)NDESC * hd.nslices, nc = (size_t)NDESC * (hd.kclusters > 0 ? hd.kclusters : 0);
    int ok = (fread(ix->median, sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fread(ix->iqr,    sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fread(ix->slices, sizeof(cslice_t), hd.nslices, fp) == (size_t)hd.nslices)
           & (fread(ix->qn,     sizeof(float), nq, fp) == nq)
           & (nc == 0 || fread(ix->centroids, sizeof(float), nc, fp) == nc)
           & (fread(ix->pool,   sizeof(int16_t), hd.pool_len, fp) == (size_t)hd.pool_len);
    fclose(fp);
    if (!ok) { corpus_index_free(ix); return NULL; }
    query_init(&ix->query, ix->qn, ix->nslices);
    return ix;
}

/* Write the index to the cache (best-effort; atomic via temp + rename). */
static void cache_store(const char *cache_dir, const char *folder, uint64_t sig, const corpus_index_t *ix) {
    mkdir(cache_dir, 0777);
    char path[600]; cache_path(path, sizeof(path), cache_dir, folder);
    char tmp[620]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    cache_hdr_t hd = { CACHE_MAGIC, CACHE_VER, sig, NDESC, ix->nslices, ix->pool_len, ix->kclusters };
    size_t nq = (size_t)NDESC * ix->nslices, nc = (size_t)NDESC * (ix->kclusters > 0 ? ix->kclusters : 0);
    int ok = (fwrite(&hd, sizeof(hd), 1, fp) == 1)
           & (fwrite(ix->median, sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fwrite(ix->iqr,    sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fwrite(ix->slices, sizeof(cslice_t), ix->nslices, fp) == (size_t)ix->nslices)
           & (fwrite(ix->qn,     sizeof(float), nq, fp) == nq)
           & (nc == 0 || fwrite(ix->centroids, sizeof(float), nc, fp) == nc)
           & (fwrite(ix->pool,   sizeof(int16_t), ix->pool_len, fp) == (size_t)ix->pool_len);
    fclose(fp);
    if (ok) rename(tmp, path); else remove(tmp);
}

void corpus_index_free(corpus_index_t *ix) {
    if (!ix) return;
    free(ix->pool);
    free(ix->slices);
    free(ix->qn);
    free(ix->centroids);
    free(ix);
}

#define KMEANS_K     16
#define KMEANS_ITERS 16

/* k-means over the RobustScaled descriptors (qn). Fills ix->centroids/kclusters.
 * Offline (analyst thread) only. Deterministic init = evenly spaced slices. */
static void corpus_kmeans(corpus_index_t *ix) {
    int n = ix->nslices, K = KMEANS_K;
    if (n <= 0) return;
    if (K > n) K = n;
    float *cen = (float *)malloc(sizeof(float) * (size_t)K * NDESC);
    int   *cnt = (int *)malloc(sizeof(int) * K);
    int   *asg = (int *)malloc(sizeof(int) * n);
    if (!cen || !cnt || !asg) { free(cen); free(cnt); free(asg); return; }

    for (int k = 0; k < K; k++)                              /* seed: evenly spaced slices */
        memcpy(&cen[k * NDESC], &ix->qn[((long)k * n / K) * NDESC], sizeof(float) * NDESC);

    for (int it = 0; it < KMEANS_ITERS; it++) {
        for (int i = 0; i < n; i++) {                        /* assign to nearest centroid */
            const float *q = &ix->qn[i * NDESC];
            int best = 0; float bd = 1e30f;
            for (int k = 0; k < K; k++) {
                const float *c = &cen[k * NDESC];
                float d = 0.0f;
                for (int j = 0; j < NDESC; j++) { float e = q[j] - c[j]; d += e * e; }
                if (d < bd) { bd = d; best = k; }
            }
            asg[i] = best;
        }
        memset(cen, 0, sizeof(float) * (size_t)K * NDESC);   /* recompute as cluster means */
        memset(cnt, 0, sizeof(int) * K);
        for (int i = 0; i < n; i++) {
            int k = asg[i]; cnt[k]++;
            const float *q = &ix->qn[i * NDESC];
            float *c = &cen[k * NDESC];
            for (int j = 0; j < NDESC; j++) c[j] += q[j];
        }
        for (int k = 0; k < K; k++) {
            if (cnt[k] > 0) { float inv = 1.0f / cnt[k]; for (int j = 0; j < NDESC; j++) cen[k * NDESC + j] *= inv; }
            else memcpy(&cen[k * NDESC], &ix->qn[(rand() % n) * NDESC], sizeof(float) * NDESC);  /* reseed empty */
        }
    }
    free(cnt); free(asg);
    ix->centroids = cen;
    ix->kclusters = K;
}

corpus_index_t *corpus_index_build(const char *folder, int sr,
                                   int slice_mode, int min_len, int max_len,
                                   float sil_gate, int pool_cap_samples,
                                   const char *cache_dir, int force_rescan,
                                   volatile int *progress) {
    if (progress) *progress = 0;

    char (*paths)[INGEST_PATH] = (char (*)[INGEST_PATH])malloc(sizeof(char[INGEST_PATH]) * MAX_FILES);
    if (!paths) return NULL;
    int nfiles = ingest_list_audio(folder, paths, MAX_FILES);
    if (nfiles <= 0) { free(paths); return NULL; }

    /* Fast path: an on-disk cache whose signature still matches → load, skip analysis. */
    uint64_t sig = cache_dir ? corpus_signature(paths, nfiles, slice_mode, min_len, max_len, sil_gate, sr) : 0;
    if (cache_dir && sig && !force_rescan) {
        corpus_index_t *hit = cache_load(cache_dir, folder, sig, pool_cap_samples);
        if (hit) { free(paths); if (progress) *progress = 100; return hit; }
    }

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

    /* OOM eviction: analyse every file, but cap the pooled AUDIO. A keep-stride
     * driven by how full the pool is decimates the retained (audible) slices so
     * they spread across the whole corpus — even within one big file — instead
     * of truncating to the first files. A corpus that fits keeps everything. */
    int  oom_stop = 0;      /* set only on a real alloc failure — a hard stop */
    long slice_idx = 0;     /* global slice counter, for the decimation stride */
    for (int f = 0; f < nfiles && !oom_stop; f++) {
        if (ix->pool_len >= (long)ix->pool_cap * 98 / 100) break;  /* full even after decimation → stop decoding the tail */
        int len = 0;
        float *audio = ingest_decode(paths[f], sr, &len);
        if (audio && len > 0) {
            int nr = (slice_mode == 1)
                ? concat_fixed_slice(a, audio, len, max_len, sil_gate, ranges, MAX_SLICES_FILE)
                : concat_onset_slice(a, audio, len, min_len, max_len, sil_gate, ranges, MAX_SLICES_FILE);
            for (int s = 0; s < nr; s++) {
                long pf = (long)ix->pool_len * 100 / (ix->pool_cap > 0 ? ix->pool_cap : 1);   /* % full */
                int stride = pf < 75 ? 1 : pf < 88 ? 2 : pf < 95 ? 4 : 8;   /* pack to 75%, then thin the tail */
                if ((slice_idx++ % stride) != 0) continue;         /* decimate as the pool fills */
                int slen = ranges[s].len;
                if (ix->pool_len + slen > ix->pool_cap) continue;  /* no room → drop audio, keep scanning */
                if (ix->nslices >= scap) {
                    int ns = scap * 2;
                    cslice_t *nsl = (cslice_t *)realloc(ix->slices, sizeof(cslice_t) * ns);
                    float    *nr2 = (float *)realloc(raw, sizeof(float) * NDESC * ns);
                    if (!nsl || !nr2) { oom_stop = 1; if (nsl) ix->slices = nsl; if (nr2) raw = nr2; break; }
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
    corpus_kmeans(ix);                                              /* pad-jump cluster centroids */
    if (cache_dir && sig) cache_store(cache_dir, folder, sig, ix);   /* best-effort */
    if (progress) *progress = 100;
    return ix;
}
