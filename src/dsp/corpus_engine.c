/**
 * Concat — corpus engine (clean-room MIT). See corpus_engine.h. Offline only.
 */
#include "corpus_engine.h"
#include "ingest.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_FILES        2048
#define MAX_SLICES_FILE  4096

/* ── On-disk index cache ─────────────────────────────────────────────────────
 * A folder's analysed index (pool + descriptors) is cached as one binary file.
 * The cache is validated by a signature over each file's mtime/size plus the
 * slicing params, so an unchanged folder reloads in a disk read instead of a
 * full decode+analysis. Device-local aarch64 only (raw struct dump). */

#define CACHE_MAGIC 0x54434E43u   /* "CNCT" */
#define CACHE_VER   4u            /* v2 k-means; v3 periodic Hann + adaptive onset; v4 PCA map */

typedef struct {
    uint32_t magic, ver;
    uint64_t sig;
    int32_t  ndesc, nslices, pool_len, kclusters, pca_ok;
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
    ix->kclusters = (hd.kclusters > 0 && hd.kclusters <= 64) ? hd.kclusters : 0;   /* clamp (corrupt-cache guard) */
    ix->pca_ok = hd.pca_ok ? 1 : 0;
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
           & (fread(ix->pca_mean,    sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fread(ix->pca_axis[0], sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fread(ix->pca_axis[1], sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fread(ix->pca_min,     sizeof(float), 2, fp) == 2)
           & (fread(ix->pca_max,     sizeof(float), 2, fp) == 2)
           & (fread(ix->pool,   sizeof(int16_t), hd.pool_len, fp) == (size_t)hd.pool_len);
    fclose(fp);
    if (!ok) { corpus_index_free(ix); return NULL; }
    /* Validate every slice's pool range (a corrupt-but-header-valid cache would else
     * make the render thread read out of the pool). */
    for (int i = 0; i < ix->nslices; i++)
        if (ix->slices[i].pool_off < 0 || ix->slices[i].len < 1 ||
            ix->slices[i].pool_off + ix->slices[i].len > ix->pool_len) { corpus_index_free(ix); return NULL; }
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
    cache_hdr_t hd; memset(&hd, 0, sizeof hd);   /* zero padding — don't write indeterminate bytes */
    hd.magic = CACHE_MAGIC; hd.ver = CACHE_VER; hd.sig = sig;
    hd.ndesc = NDESC; hd.nslices = ix->nslices; hd.pool_len = ix->pool_len;
    hd.kclusters = ix->kclusters; hd.pca_ok = ix->pca_ok;
    size_t nq = (size_t)NDESC * ix->nslices, nc = (size_t)NDESC * (ix->kclusters > 0 ? ix->kclusters : 0);
    int ok = (fwrite(&hd, sizeof(hd), 1, fp) == 1)
           & (fwrite(ix->median, sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fwrite(ix->iqr,    sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fwrite(ix->slices, sizeof(cslice_t), ix->nslices, fp) == (size_t)ix->nslices)
           & (fwrite(ix->qn,     sizeof(float), nq, fp) == nq)
           & (nc == 0 || fwrite(ix->centroids, sizeof(float), nc, fp) == nc)
           & (fwrite(ix->pca_mean,    sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fwrite(ix->pca_axis[0], sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fwrite(ix->pca_axis[1], sizeof(float), NDESC, fp) == (size_t)NDESC)
           & (fwrite(ix->pca_min,     sizeof(float), 2, fp) == 2)
           & (fwrite(ix->pca_max,     sizeof(float), 2, fp) == 2)
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
            else memcpy(&cen[k * NDESC], &ix->qn[((it * 2654435761u + k * 40503u) % (unsigned)n) * NDESC], sizeof(float) * NDESC);  /* deterministic reseed */
        }
    }
    free(cnt); free(asg);
    ix->centroids = cen;
    ix->kclusters = K;
}

/* PCA learned map: project the NDESC descriptor space onto its top-2 principal
 * axes so the X/Y map becomes a spread-out timbre plane. Power iteration +
 * deflation for the top 2 eigenvectors of the covariance. Offline. */
static void power_iter(const double *C, int d, const double *deflate_v, double deflate_l, double *out) {
    double v[NDESC]; for (int j = 0; j < d; j++) v[j] = (j == 0) ? 1.0 : 0.001 * (j + 1);
    for (int it = 0; it < 60; it++) {
        double w[NDESC], norm = 0.0;
        for (int j = 0; j < d; j++) {
            double s = 0.0;
            for (int k = 0; k < d; k++) {
                double c = C[j * d + k];
                if (deflate_v) c -= deflate_l * deflate_v[j] * deflate_v[k];   /* deflate 1st axis */
                s += c * v[k];
            }
            w[j] = s; norm += s * s;
        }
        norm = sqrt(norm); if (norm < 1e-12) break;
        for (int j = 0; j < d; j++) v[j] = w[j] / norm;
    }
    for (int j = 0; j < d; j++) out[j] = v[j];
}

static void corpus_pca(corpus_index_t *ix) {
    int n = ix->nslices, d = NDESC;
    ix->pca_ok = 0;
    if (n < 4) return;
    double *C = (double *)calloc((size_t)d * d, sizeof(double));
    double mean[NDESC], v1[NDESC], v2[NDESC];
    if (!C) return;
    for (int j = 0; j < d; j++) { double m = 0.0; for (int i = 0; i < n; i++) m += ix->qn[i * d + j]; mean[j] = m / n; }
    for (int i = 0; i < n; i++) {                    /* covariance (symmetric) */
        const float *q = &ix->qn[i * d];
        for (int j = 0; j < d; j++) { double dj = q[j] - mean[j]; for (int k = j; k < d; k++) C[j * d + k] += dj * (q[k] - mean[k]); }
    }
    for (int j = 0; j < d; j++) for (int k = j; k < d; k++) { C[j * d + k] /= n; C[k * d + j] = C[j * d + k]; }

    power_iter(C, d, NULL, 0.0, v1);
    double l1 = 0.0; { double w[NDESC]; for (int j = 0; j < d; j++) { double s = 0; for (int k = 0; k < d; k++) s += C[j * d + k] * v1[k]; w[j] = s; } for (int j = 0; j < d; j++) l1 += v1[j] * w[j]; }
    power_iter(C, d, v1, l1, v2);
    free(C);

    /* Gram-Schmidt: force v2 ⊥ v1 exactly (robust when the 2nd axis is near-degenerate —
     * power iteration on a rank-deficient deflated covariance can return a non-orthogonal
     * guess). If v2 collapses to ~0, the projection range does too → pca_ok stays 0. */
    { double dot = 0.0; for (int j = 0; j < d; j++) dot += v1[j] * v2[j];
      double n2 = 0.0; for (int j = 0; j < d; j++) { v2[j] -= dot * v1[j]; n2 += v2[j] * v2[j]; }
      n2 = sqrt(n2);
      if (n2 > 1e-9) for (int j = 0; j < d; j++) v2[j] /= n2; else for (int j = 0; j < d; j++) v2[j] = 0.0; }

    float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
    for (int i = 0; i < n; i++) {
        const float *q = &ix->qn[i * d];
        double p1 = 0.0, p2 = 0.0;
        for (int j = 0; j < d; j++) { double dq = q[j] - mean[j]; p1 += dq * v1[j]; p2 += dq * v2[j]; }
        if (p1 < mn[0]) mn[0] = (float)p1;
        if (p1 > mx[0]) mx[0] = (float)p1;
        if (p2 < mn[1]) mn[1] = (float)p2;
        if (p2 > mx[1]) mx[1] = (float)p2;
    }
    for (int j = 0; j < d; j++) { ix->pca_mean[j] = (float)mean[j]; ix->pca_axis[0][j] = (float)v1[j]; ix->pca_axis[1][j] = (float)v2[j]; }
    ix->pca_min[0] = mn[0]; ix->pca_max[0] = mx[0];
    ix->pca_min[1] = mn[1]; ix->pca_max[1] = mx[1];
    ix->pca_ok = (mx[0] > mn[0] && mx[1] > mn[1]) ? 1 : 0;
}

/* Write a 128x64 8-bit grayscale BMP of the corpus scatter for canvas.js to blit.
 * Uses PCA plane when available, else the raw centroid/loud descriptor axes.
 * Format is bare BMP (bottom-up, DIB v3, 8-bit indexed grayscale, 256-colour table). */
static void corpus_write_scatter_bmp(corpus_index_t *ix, const char *cache_dir, const char *folder) {
    if (!cache_dir || ix->nslices <= 0) return;
    /* 8x downsampled: 128 px * 8 = 1024 columns padded to /4 → each row = 128 bytes. */
    const int W = 128, H = 64;
    unsigned char pix[64 * 128]; memset(pix, 255, sizeof(pix));   /* white bg */

    int use_pca = ix->pca_ok;
    float minx, maxx, miny, maxy;
    if (use_pca) { minx=ix->pca_min[0]; maxx=ix->pca_max[0]; miny=ix->pca_min[1]; maxy=ix->pca_max[1]; }
    else {
        minx=maxx=ix->qn[D_CENTROID_MEAN]; miny=maxy=ix->qn[D_LOUD_MEAN];
        for (int i = 1; i < ix->nslices; i++) {
            float a = ix->qn[i * NDESC + D_CENTROID_MEAN], b = ix->qn[i * NDESC + D_LOUD_MEAN];
            if (a < minx) minx = a;
            if (a > maxx) maxx = a;
            if (b < miny) miny = b;
            if (b > maxy) maxy = b;
        }
    }
    float rx = (maxx > minx) ? (maxx - minx) : 1.0f, ry = (maxy > miny) ? (maxy - miny) : 1.0f;
    for (int i = 0; i < ix->nslices; i++) {
        double p1, p2;
        if (use_pca) {
            p1 = 0; p2 = 0;
            for (int j = 0; j < NDESC; j++) {
                double dq = ix->qn[i * NDESC + j] - ix->pca_mean[j];
                p1 += dq * ix->pca_axis[0][j]; p2 += dq * ix->pca_axis[1][j];
            }
        } else { p1 = ix->qn[i * NDESC + D_CENTROID_MEAN]; p2 = ix->qn[i * NDESC + D_LOUD_MEAN]; }
        int px = (int)(((p1 - minx) / rx) * (W - 1));
        int py = (int)(((p2 - miny) / ry) * (H - 1));
        if (px < 0) px = 0;
        else if (px > W - 1) px = W - 1;
        if (py < 0) py = 0;
        else if (py > H - 1) py = H - 1;
        /* dot cluster: darken 2x2 for visibility */
        for (int dx = 0; dx < 2 && px + dx < W; dx++)
            for (int dy = 0; dy < 2 && py + dy < H; dy++) {
                unsigned char *p = &pix[(py + dy) * W + (px + dx)];
                if (*p > 60) *p -= 60; else *p = 0;
            }
    }

    /* BMP: 14-byte file header + 40-byte DIB v3 + 1024-byte greyscale palette + pixel data.
     * Rows are bottom-up, each row 4-byte aligned (W=128 already aligned). */
    const int row = W, palsize = 256 * 4, hdr = 14 + 40 + palsize, filesz = hdr + row * H;
    unsigned char bmp[14 + 40 + 256 * 4 + 64 * 128];
    memset(bmp, 0, sizeof(bmp));
    bmp[0]='B'; bmp[1]='M';
    bmp[2]=filesz&0xff; bmp[3]=(filesz>>8)&0xff; bmp[4]=(filesz>>16)&0xff; bmp[5]=(filesz>>24)&0xff;
    bmp[10]=hdr&0xff; bmp[11]=(hdr>>8)&0xff; bmp[12]=(hdr>>16)&0xff; bmp[13]=(hdr>>24)&0xff;
    bmp[14]=40;
    bmp[18]=W&0xff; bmp[19]=(W>>8)&0xff;
    bmp[22]=H&0xff; bmp[23]=(H>>8)&0xff;
    bmp[26]=1; bmp[28]=8;                        /* planes=1, bpp=8 */
    int imgsz = row * H;
    bmp[34]=imgsz&0xff; bmp[35]=(imgsz>>8)&0xff; bmp[36]=(imgsz>>16)&0xff;
    bmp[46]=0; bmp[50]=0;                        /* colors used = 256 (0 = default) */
    /* greyscale palette */
    for (int c = 0; c < 256; c++) {
        int o = 14 + 40 + c * 4;
        bmp[o] = bmp[o+1] = bmp[o+2] = (unsigned char)c;
    }
    /* pixel data — BMP is bottom-up, so flip the row order. */
    for (int y = 0; y < H; y++)
        memcpy(&bmp[hdr + (H - 1 - y) * row], &pix[y * W], W);

    char tmp[620];
    snprintf(ix->scatter_bmp, sizeof(ix->scatter_bmp), "%s/scatter_%016llx.bmp", cache_dir,
             (unsigned long long)fnv1a(0xcbf29ce484222325ULL, folder, (int)strlen(folder)));
    snprintf(tmp, sizeof(tmp), "%s.tmp", ix->scatter_bmp);
    FILE *fp = fopen(tmp, "wb");
    if (fp) {
        int wr = (int)fwrite(bmp, 1, filesz, fp);
        fclose(fp);
        if (wr == filesz) rename(tmp, ix->scatter_bmp); else remove(tmp);
    }
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
        if (hit) {
            corpus_write_scatter_bmp(hit, cache_dir, folder);   /* regenerate scatter image (cheap) */
            free(paths); if (progress) *progress = 100; return hit;
        }
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
    /* Estimate the corpus size from file sizes so the decimation stride is right from the
     * FIRST slice. A fullness-only curve front-loads badly when the pool fills inside the
     * first file or two (a library of many short files); the curve below still corrects an
     * off estimate. Rough per-format bytes -> mono-sample factors. */
    double est_samples = 0.0;
    for (int f = 0; f < nfiles; f++) {
        struct stat est; if (stat(paths[f], &est) != 0) continue;
        const char *e = strrchr(paths[f], '.');
        double factor = 0.25;                                    /* wav 16-bit stereo: 4 B/sample */
        if (e && !strcasecmp(e, ".flac")) factor = 0.5;          /* ~2:1 */
        else if (e && !strcasecmp(e, ".mp3")) factor = 2.75;     /* ~10:1 */
        est_samples += (double)est.st_size * factor;
    }
    int base_stride = 1;
    if (pool_cap_samples > 0 && est_samples > (double)pool_cap_samples)
        base_stride = (int)(est_samples / (double)pool_cap_samples + 0.5);
    if (base_stride < 1) base_stride = 1;
    if (base_stride > 256) base_stride = 256;

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
                if (base_stride > stride) stride = base_stride;             /* size-estimate floor */
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
    corpus_pca(ix);                                                 /* learned 2-D map axes */
    corpus_write_scatter_bmp(ix, cache_dir, folder);                /* canvas.js scatter image */
    if (cache_dir && sig) cache_store(cache_dir, folder, sig, ix);   /* best-effort */
    if (progress) *progress = 100;
    return ix;
}
