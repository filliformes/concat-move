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

static uint32_t g_index_gen = 0;                 /* bumped for every index handed out */
static void index_stamp(corpus_index_t *ix) { ix->gen = __atomic_add_fetch(&g_index_gen, 1, __ATOMIC_RELAXED); }

/* Large reads/writes in 4 MB steps so a cancel request is honoured within ~50 ms. */
#define IO_STEP (4u << 20)
static int read_chunked(void *dst, size_t bytes, FILE *fp) {
    unsigned char *p = (unsigned char *)dst;
    while (bytes) {
        if (concat_cancelled()) return 0;
        size_t n = bytes < IO_STEP ? bytes : IO_STEP;
        if (fread(p, 1, n, fp) != n) return 0;
        p += n; bytes -= n;
    }
    return 1;
}
static int write_chunked(const void *src, size_t bytes, FILE *fp) {
    const unsigned char *p = (const unsigned char *)src;
    while (bytes) {
        if (concat_cancelled()) return 0;
        size_t n = bytes < IO_STEP ? bytes : IO_STEP;
        if (fwrite(p, 1, n, fp) != n) return 0;
        p += n; bytes -= n;
    }
    return 1;
}

/* Per-slice coordinates on the PCA plane (same units as pca_min/max), so Learned-map
 * queries are a 2-D search that matches what the View draws. */
static void corpus_project(corpus_index_t *ix) {
    free(ix->proj); ix->proj = NULL; ix->query.proj = NULL;
    if (!ix->pca_ok || ix->nslices <= 0) return;
    ix->proj = (float *)malloc(sizeof(float) * 2 * (size_t)ix->nslices);
    if (!ix->proj) return;
    for (int i = 0; i < ix->nslices; i++) {
        const float *q = &ix->qn[(size_t)i * NDESC];
        double p1 = 0.0, p2 = 0.0;
        for (int j = 0; j < NDESC; j++) { double dq = q[j] - ix->pca_mean[j]; p1 += dq * ix->pca_axis[0][j]; p2 += dq * ix->pca_axis[1][j]; }
        ix->proj[2 * i] = (float)p1; ix->proj[2 * i + 1] = (float)p2;
    }
    ix->query.proj = ix->proj;                 /* after query_init (which clears the query) */
}

long corpus_mem_available_mb(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0;
    char line[160]; long kb = 0;
    while (fgets(line, sizeof line, fp)) if (!strncmp(line, "MemAvailable:", 13)) { kb = atol(line + 13); break; }
    fclose(fp);
    return kb / 1024;
}

/* Order-independent content signature: XOR of per-item (path+mtime+size) hashes,
 * mixed with the slicing params. A trimmed range and a descriptor mask only enter
 * the hash when set, so plain folders keep their existing cache. Returns 0
 * (-> "no signature") if any stat fails. */
static uint64_t corpus_signature(const corpus_item_t *items, int nitems, const corpus_spec_t *spec,
                                 int slice_mode, int min_len, int max_len, float sil_gate, int sr) {
    uint64_t x = 0;
    for (int f = 0; f < nitems; f++) {
        struct stat st;
        if (stat(items[f].path, &st) != 0) return 0;
        uint64_t h = fnv1a(0xcbf29ce484222325ULL, items[f].path, (int)strlen(items[f].path));
        long mt = (long)st.st_mtime, sz = (long)st.st_size;
        h = fnv1a(h, &mt, sizeof(mt));
        h = fnv1a(h, &sz, sizeof(sz));
        if (items[f].start_s > 0.0 || items[f].end_s > 0.0) {
            double rg[2] = { items[f].start_s, items[f].end_s };
            h = fnv1a(h, rg, sizeof(rg));
        }
        x ^= h;
    }
    int parms[4] = { slice_mode, min_len, max_len, sr };
    uint64_t s = fnv1a(0xcbf29ce484222325ULL, &x, sizeof(x));
    s = fnv1a(s, parms, sizeof(parms));
    s = fnv1a(s, &sil_gate, sizeof(sil_gate));
    if (spec && spec->has_mask) s = fnv1a(s, spec->mask, sizeof(spec->mask));
    return s ? s : 1;   /* never 0 (reserved) */
}

static void cache_path(char *out, int outsz, const char *cache_dir, const char *folder) {
    uint64_t h = fnv1a(0xcbf29ce484222325ULL, folder, (int)strlen(folder));
    snprintf(out, outsz, "%s/concat_%016llx.idx", cache_dir, (unsigned long long)h);
}

/* Load a cached index if the signature matches and it still fits the pool cap. */
static corpus_index_t *cache_load(const char *cache_dir, const char *folder,
                                  uint64_t sig, int pool_cap_samples, int mem_cap_samples) {
    char path[600]; cache_path(path, sizeof(path), cache_dir, folder);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    cache_hdr_t hd;
    if (fread(&hd, sizeof(hd), 1, fp) != 1 ||
        hd.magic != CACHE_MAGIC || hd.ver != CACHE_VER || hd.ndesc != NDESC ||
        hd.sig != sig || hd.nslices <= 0 || hd.pool_len <= 0 ||
        hd.pool_len > pool_cap_samples ||
        (mem_cap_samples > 0 && hd.pool_len > mem_cap_samples)) { fclose(fp); return NULL; }

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
           & read_chunked(ix->pool, sizeof(int16_t) * (size_t)hd.pool_len, fp);
    fclose(fp);
    if (!ok) { corpus_index_free(ix); return NULL; }
    /* Validate every slice's pool range (a corrupt-but-header-valid cache would else
     * make the render thread read out of the pool). */
    for (int i = 0; i < ix->nslices; i++)
        if (ix->slices[i].pool_off < 0 || ix->slices[i].len < 1 ||
            (long)ix->slices[i].pool_off + ix->slices[i].len > (long)ix->pool_len) { corpus_index_free(ix); return NULL; }
    query_init(&ix->query, ix->qn, ix->nslices);
    corpus_project(ix);
    index_stamp(ix);
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
           & write_chunked(ix->pool, sizeof(int16_t) * (size_t)ix->pool_len, fp);
    if (fclose(fp) != 0) ok = 0;
    if (ok) rename(tmp, path); else remove(tmp);
}

void corpus_index_free(corpus_index_t *ix) {
    if (!ix) return;
    free(ix->pool);
    free(ix->slices);
    free(ix->qn);
    free(ix->centroids);
    free(ix->proj);
    query_free(&ix->query);
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
        if (concat_cancelled()) { free(cen); free(cnt); free(asg); return; }
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
        /* Solid black on any hit — mono OLED thresholds at 128, so a subtle darken
         * would drop below-threshold dots. Then a 2x2 cluster for visibility. */
        for (int dx = 0; dx < 2 && px + dx < W; dx++)
            for (int dy = 0; dy < 2 && py + dy < H; dy++)
                pix[(py + dy) * W + (px + dx)] = 0;
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

#define WIN_SECONDS    CONCAT_WIN_SECONDS
#define MEM_FLOOR_MB   150    /* stop adding files if free RAM drops below this mid-build */

/* ── Per-file descriptor cache ───────────────────────────────────────────────
 * One small file per (audio file, range, slicing params) under <cache_dir>/files/,
 * holding the slice boundaries and the raw (unscaled) descriptors computed so far.
 * When a corpus changes, the whole-index cache misses, but unchanged files skip
 * slicing and descriptors (the costly part: YIN) and only re-read their audio.
 * Validated by the audio file's mtime + size. Slices the pool decimated away have
 * no descriptor yet; they are computed if a later build keeps them. */
#define PFC_MAGIC 0x31444643u   /* "CFD1" */
#define PFC_VER   1u

typedef struct { int start, len; } pfc_slice_t;   /* start: output frame from the range start */
typedef struct {
    int          n, cap;
    long long    covered;      /* frames sliced so far (whole windows from the range start) */
    pfc_slice_t *sl;
    float       *desc;         /* [cap*NDESC] raw descriptors */
    uint8_t     *has;          /* descriptor computed? */
    int          dirty;
} pfc_t;

static void pfc_free(pfc_t *p) { free(p->sl); free(p->desc); free(p->has); memset(p, 0, sizeof(*p)); }

static int pfc_grow(pfc_t *p, int need) {
    if (need <= p->cap) return 1;
    int nc = p->cap ? p->cap : 256;
    while (nc < need) nc *= 2;
    pfc_slice_t *s = (pfc_slice_t *)realloc(p->sl, sizeof(pfc_slice_t) * (size_t)nc);
    if (!s) return 0;
    p->sl = s;
    float *d = (float *)realloc(p->desc, sizeof(float) * NDESC * (size_t)nc);
    if (!d) return 0;
    p->desc = d;
    uint8_t *h = (uint8_t *)realloc(p->has, (size_t)nc);
    if (!h) return 0;
    p->has = h;
    p->cap = nc;
    return 1;
}

static void pfc_path(char *out, int outsz, const char *cache_dir, const corpus_item_t *it,
                     int slice_mode, int min_len, int max_len, float sil_gate, int sr) {
    uint64_t h = fnv1a(0xcbf29ce484222325ULL, it->path, (int)strlen(it->path));
    double rg[2] = { it->start_s, it->end_s };
    int parms[5] = { slice_mode, min_len, max_len, sr, WIN_SECONDS };
    h = fnv1a(h, rg, sizeof(rg));
    h = fnv1a(h, parms, sizeof(parms));
    h = fnv1a(h, &sil_gate, sizeof(sil_gate));
    snprintf(out, outsz, "%s/files/%016llx.cfd", cache_dir, (unsigned long long)h);
}

static int pfc_load(pfc_t *p, const char *path, const struct stat *ast) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint32_t hdr[2]; long long meta[3]; int n = 0;
    int ok = fread(hdr, sizeof(hdr), 1, fp) == 1 && hdr[0] == PFC_MAGIC && hdr[1] == PFC_VER
          && fread(meta, sizeof(meta), 1, fp) == 1
          && meta[0] == (long long)ast->st_mtime && meta[1] == (long long)ast->st_size
          && fread(&n, sizeof(n), 1, fp) == 1 && n >= 0 && n < (1 << 22) && pfc_grow(p, n > 0 ? n : 1);
    if (ok && n > 0)
        ok = fread(p->sl, sizeof(pfc_slice_t), (size_t)n, fp) == (size_t)n
          && fread(p->has, 1, (size_t)n, fp) == (size_t)n
          && fread(p->desc, sizeof(float) * NDESC, (size_t)n, fp) == (size_t)n;
    fclose(fp);
    if (!ok) { pfc_free(p); return 0; }
    p->n = n; p->covered = meta[2]; p->dirty = 0;
    for (int i = 0; i < n; i++)
        if (p->sl[i].start < 0 || p->sl[i].len < 1) { pfc_free(p); return 0; }
    return 1;
}

static void pfc_store(const pfc_t *p, const char *path, const char *cache_dir, const struct stat *ast) {
    char dir[600], tmp[700];
    snprintf(dir, sizeof(dir), "%s/files", cache_dir);
    mkdir(dir, 0755);                                   /* fine if it exists */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    uint32_t hdr[2] = { PFC_MAGIC, PFC_VER };
    long long meta[3] = { (long long)ast->st_mtime, (long long)ast->st_size, p->covered };
    int ok = fwrite(hdr, sizeof(hdr), 1, fp) == 1 && fwrite(meta, sizeof(meta), 1, fp) == 1
          && fwrite(&p->n, sizeof(p->n), 1, fp) == 1;
    if (ok && p->n > 0)
        ok = fwrite(p->sl, sizeof(pfc_slice_t), (size_t)p->n, fp) == (size_t)p->n
          && fwrite(p->has, 1, (size_t)p->n, fp) == (size_t)p->n
          && fwrite(p->desc, sizeof(float) * NDESC, (size_t)p->n, fp) == (size_t)p->n;
    if (fclose(fp) != 0) ok = 0;
    if (ok) rename(tmp, path); else remove(tmp);
}

corpus_index_t *corpus_index_build(const char *folder, int sr,
                                   int slice_mode, int min_len, int max_len,
                                   float sil_gate, int pool_cap_samples, int mem_cap_samples,
                                   const char *cache_dir, int force_rescan,
                                   corpus_build_info_t *info) {
    corpus_build_info_t local;
    corpus_spec_t *spec_out = info ? info->spec : NULL;
    void (*tick)(void *) = info ? info->tick : NULL;
    void *tick_ctx = info ? info->tick_ctx : NULL;
    if (!info) info = &local;
    memset((void *)info, 0, sizeof(*info));
    info->spec = spec_out; info->tick = tick; info->tick_ctx = tick_ctx;
#define TICK() do { if (info->tick) info->tick(info->tick_ctx); } while (0)
    info->phase = CB_SCAN;
    TICK();

    int cap = pool_cap_samples;
    if (mem_cap_samples > 0 && mem_cap_samples < cap) { cap = mem_cap_samples; info->mem_limited = 1; }
    if (cap < 1) cap = 1;

    corpus_item_t *items = (corpus_item_t *)malloc(sizeof(corpus_item_t) * MAX_FILES);
    if (!items) { info->err = CB_ERR_MEM; return NULL; }
    corpus_spec_t spec;                                          /* ~17 KB: analyst stack is 8 MB */
    int nfiles = corpus_resolve(folder, &spec, items, MAX_FILES);
    info->watch_stamp = corpus_watch_stamp(&spec);
    if (spec_out) *spec_out = spec;
    info->sources_missing = spec.sources_missing;
    if (nfiles < 0)  { free(items); info->err = CB_ERR_SPEC; return NULL; }
    if (nfiles == 0) { free(items); info->err = CB_ERR_NOFILES; return NULL; }
    info->files_total = nfiles;
    if (spec.has_slicing) {                                      /* the .concat file's slicing wins */
        slice_mode = spec.slice_mode;
        min_len = (int)((long)spec.min_ms * sr / 1000);
        max_len = (int)((long)spec.max_ms * sr / 1000);
        if (spec.has_gate) sil_gate = spec.sil_gate;
    }

    /* Fast path: an on-disk cache whose signature still matches -> load, skip analysis.
     * It is valid up to the user's Pool Cap; it is only refused if free RAM can't hold it. */
    uint64_t sig = cache_dir ? corpus_signature(items, nfiles, &spec, slice_mode, min_len, max_len, sil_gate, sr) : 0;
    if (cache_dir && sig && !force_rescan) {
        info->phase = CB_LOAD;
        TICK();
        corpus_index_t *hit = cache_load(cache_dir, folder, sig, pool_cap_samples, mem_cap_samples);
        if (hit) {
            corpus_write_scatter_bmp(hit, cache_dir, folder);   /* regenerate scatter image (cheap) */
            free(items);
            info->from_cache = 1; info->mem_limited = 0; info->nslices = hit->nslices;
            info->files_done = nfiles; info->progress = 95; info->phase = CB_DONE;
            return hit;
        }
        if (concat_cancelled()) { free(items); info->err = CB_ERR_CANCEL; return NULL; }
    }

    info->phase = CB_ANALYSE;
    TICK();
    analysis_t *a = analysis_create(sr, 1024, 40, 13);
    corpus_index_t *ix = (corpus_index_t *)calloc(1, sizeof(corpus_index_t));
    slice_range_t *ranges = (slice_range_t *)malloc(sizeof(slice_range_t) * MAX_SLICES_FILE);
    int win_cap = sr * WIN_SECONDS;
    float *win = (float *)malloc(sizeof(float) * (size_t)win_cap);
    if (!a || !ix || !ranges || !win) {
        free(items); free(ranges); free(win); analysis_destroy(a); free(ix);
        info->err = CB_ERR_MEM; return NULL;
    }

    ix->pool = (int16_t *)malloc(sizeof(int16_t) * (size_t)cap);
    ix->pool_cap = cap;
    int scap = 1024;
    ix->slices = (cslice_t *)malloc(sizeof(cslice_t) * scap);
    float *raw = (float *)malloc(sizeof(float) * NDESC * scap);
    if (!ix->pool || !ix->slices || !raw) {
        free(items); free(ranges); free(win); free(raw); analysis_destroy(a);
        corpus_index_free(ix); info->err = CB_ERR_MEM; return NULL;
    }

    /* OOM eviction: analyse every file, but cap the pooled AUDIO. A keep-stride
     * driven by how full the pool is decimates the retained (audible) slices so
     * they spread across the whole corpus (even within one big file) instead
     * of truncating to the first files. A corpus that fits keeps everything.
     * The stride starts from a file-size estimate so it is right from the first slice. */
    double est_samples = 0.0;
    for (int f = 0; f < nfiles; f++) {
        if (items[f].end_s > items[f].start_s) { est_samples += (items[f].end_s - items[f].start_s) * sr; continue; }
        struct stat est; if (stat(items[f].path, &est) != 0) continue;
        const char *e = strrchr(items[f].path, '.');
        double factor = 0.25;                                    /* wav/aiff 16-bit stereo: 4 B/sample */
        if (e && !strcasecmp(e, ".flac")) factor = 0.5;          /* ~2:1 */
        else if (e && !strcasecmp(e, ".mp3")) factor = 2.75;     /* ~10:1 */
        est_samples += (double)est.st_size * factor;
    }
    int base_stride = 1;
    if (est_samples > (double)cap) base_stride = (int)(est_samples / (double)cap + 0.5);
    if (base_stride < 1) base_stride = 1;
    if (base_stride > 256) base_stride = 256;

    int  stop = 0;          /* hard stop: alloc failure or cancel */
    int  decoded_any = 0;
    long slice_idx = 0;     /* global slice counter, for the decimation stride */
    for (int f = 0; f < nfiles && !stop; f++) {
        if (concat_cancelled()) { stop = 1; break; }
        if (ix->pool_len >= (long)ix->pool_cap * 98 / 100) { info->files_skipped = nfiles - f; break; }
        long av = corpus_mem_available_mb();
        if (av > 0 && av < MEM_FLOOR_MB) { info->mem_limited = 1; info->files_skipped = nfiles - f; break; }

        ingest_reader_t *rd = ingest_open(items[f].path, sr);
        if (!rd) { info->files_failed++; info->files_done = f + 1; continue; }
        decoded_any = 1;
        pfc_t pfc; memset(&pfc, 0, sizeof(pfc));
        char pfc_file[640]; struct stat ast;
        int use_pfc = cache_dir && stat(items[f].path, &ast) == 0;
        if (use_pfc) {
            pfc_path(pfc_file, sizeof(pfc_file), cache_dir, &items[f], slice_mode, min_len, max_len, sil_gate, sr);
            if (!force_rescan && pfc_load(&pfc, pfc_file, &ast)) info->files_cached++;
        }
        long long win_base = 0;                                  /* frames from the range start */
        int pfc_cursor = 0;
        long long remain = -1;                                   /* frames left in a trimmed range */
        if (items[f].start_s > 0.0) {
            long long st = (long long)(items[f].start_s * sr);
            if (!ingest_seek(rd, st))                            /* decoder can't seek: read past it */
                while (st > 0 && !concat_cancelled()) {
                    int got = ingest_read(rd, win, st < win_cap ? (int)st : win_cap);
                    if (got <= 0) break;
                    st -= got;
                }
        }
        if (items[f].end_s > items[f].start_s) remain = (long long)((items[f].end_s - items[f].start_s) * sr);
        for (;;) {                                               /* one window at a time */
            if (concat_cancelled()) { stop = 1; break; }
            int want = win_cap;
            if (remain >= 0 && remain < want) want = (int)remain;
            if (want <= 0) break;
            int len = ingest_read(rd, win, want);
            if (len <= 0) break;
            if (remain >= 0) remain -= len;
            int nr = 0, pidx_base = 0;
            if (win_base + len <= pfc.covered) {                 /* sliced before: boundaries from the cache */
                while (pfc_cursor < pfc.n && pfc.sl[pfc_cursor].start < win_base) pfc_cursor++;
                pidx_base = pfc_cursor;
                while (pfc_cursor < pfc.n && pfc.sl[pfc_cursor].start < win_base + len && nr < MAX_SLICES_FILE) {
                    ranges[nr].start = (int)(pfc.sl[pfc_cursor].start - win_base);
                    ranges[nr].len = pfc.sl[pfc_cursor].len;
                    if (ranges[nr].start + ranges[nr].len > len) ranges[nr].len = len - ranges[nr].start;
                    nr++; pfc_cursor++;
                }
            } else {
                nr = (slice_mode == 1)
                    ? concat_fixed_slice(a, win, len, max_len, sil_gate, ranges, MAX_SLICES_FILE)
                    : concat_onset_slice(a, win, len, min_len, max_len, sil_gate, ranges, MAX_SLICES_FILE);
                if (concat_cancelled()) { stop = 1; break; }     /* the slicer bails out early on cancel */
                pidx_base = pfc.n;
                if (use_pfc && pfc_grow(&pfc, pfc.n + nr + 1)) {  /* record this window's boundaries */
                    for (int r = 0; r < nr; r++) {
                        pfc.sl[pfc.n + r].start = (int)(win_base + ranges[r].start);
                        pfc.sl[pfc.n + r].len = ranges[r].len;
                        pfc.has[pfc.n + r] = 0;
                    }
                    pfc.n += nr; pfc.covered = win_base + len; pfc.dirty = 1;
                    pfc_cursor = pfc.n;
                } else use_pfc = 0;
            }
            for (int s = 0; s < nr && !stop; s++) {
                if (concat_cancelled()) { stop = 1; break; }   /* per slice: YIN makes each one costly */
                long pf = (long)ix->pool_len * 100 / (ix->pool_cap > 0 ? ix->pool_cap : 1);   /* % full */
                int stride = pf < 75 ? 1 : pf < 88 ? 2 : pf < 95 ? 4 : 8;   /* pack to 75%, then thin the tail */
                if (base_stride > stride) stride = base_stride;             /* size-estimate floor */
                if ((slice_idx++ % stride) != 0) continue;         /* decimate as the pool fills */
                int slen = ranges[s].len;
                if ((long)ix->pool_len + slen > (long)ix->pool_cap) continue;  /* no room: drop audio, keep scanning */
                if (ix->nslices >= scap) {
                    int ns = scap * 2;
                    cslice_t *nsl = (cslice_t *)realloc(ix->slices, sizeof(cslice_t) * ns);
                    if (nsl) ix->slices = nsl;
                    float    *nr2 = nsl ? (float *)realloc(raw, sizeof(float) * NDESC * ns) : NULL;
                    if (nr2) raw = nr2;
                    if (!nsl || !nr2) { stop = 1; info->mem_limited = 1; break; }
                    scap = ns;
                }
                const float *src = &win[ranges[s].start];
                int off = ix->pool_len;
                for (int i = 0; i < slen; i++) {
                    float v = src[i];
                    if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                    ix->pool[off + i] = (int16_t)(v * 32767.0f);
                }
                ix->slices[ix->nslices].pool_off = off;
                ix->slices[ix->nslices].len = slen;
                float *dst = &raw[(size_t)ix->nslices * NDESC];
                int pi = pidx_base + s;
                if (use_pfc && pi < pfc.n && pfc.has[pi] && ranges[s].len == pfc.sl[pi].len) {
                    memcpy(dst, &pfc.desc[(size_t)pi * NDESC], sizeof(float) * NDESC);
                } else {
                    concat_slice_descriptor(a, src, slen, dst);
                    if (use_pfc && pi < pfc.n) {
                        memcpy(&pfc.desc[(size_t)pi * NDESC], dst, sizeof(float) * NDESC);
                        pfc.has[pi] = 1; pfc.dirty = 1;
                    }
                }
                ix->slices[ix->nslices].pitch = raw[(size_t)ix->nslices * NDESC + D_PITCH];  /* for Notes retune */
                ix->nslices++;
                ix->pool_len += slen;
            }
            win_base += len;
            if (ix->pool_len >= (long)ix->pool_cap * 98 / 100) break;
        }
        ingest_close(rd);
        if (use_pfc && pfc.dirty) pfc_store(&pfc, pfc_file, cache_dir, &ast);   /* partial progress counts too */
        pfc_free(&pfc);
        info->files_done = f + 1;
        info->progress = (int)((long)(f + 1) * 90 / nfiles);
        TICK();
    }

    free(items); free(ranges); free(win); analysis_destroy(a);

    if (concat_cancelled()) { free(raw); corpus_index_free(ix); info->err = CB_ERR_CANCEL; return NULL; }
    if (ix->nslices == 0) {
        free(raw); corpus_index_free(ix);
        info->err = !decoded_any ? CB_ERR_NOAUDIO : (stop || info->mem_limited) ? CB_ERR_MEM : CB_ERR_GATED;
        return NULL;
    }

    info->phase = CB_FINAL;
    TICK();
    concat_robust_fit(raw, ix->nslices, ix->median, ix->iqr);
    ix->qn = (float *)malloc(sizeof(float) * NDESC * (size_t)ix->nslices);
    if (!ix->qn) { free(raw); corpus_index_free(ix); info->err = CB_ERR_MEM; return NULL; }
    for (int i = 0; i < ix->nslices; i++)
        concat_robust_apply(&raw[(size_t)i * NDESC], ix->median, ix->iqr, &ix->qn[(size_t)i * NDESC]);
    free(raw);
    if (spec.has_mask)                                              /* unused descriptors: a zero column */
        for (int i = 0; i < ix->nslices; i++)                       /* drops out of every distance, k-means */
            for (int d = 0; d < NDESC; d++)                         /* and PCA with no other change */
                if (spec.mask[d] == 0.0f) ix->qn[(size_t)i * NDESC + d] = 0.0f;

    query_init(&ix->query, ix->qn, ix->nslices);
    corpus_kmeans(ix);                                              /* pad-jump cluster centroids */
    corpus_pca(ix);                                                 /* learned 2-D map axes */
    corpus_project(ix);                                             /* 2-D coords for Learned queries */
    if (concat_cancelled()) { corpus_index_free(ix); info->err = CB_ERR_CANCEL; return NULL; }
    corpus_write_scatter_bmp(ix, cache_dir, folder);                /* canvas.js scatter image */
    info->progress = 95;
    if (cache_dir && sig && !info->mem_limited) {                   /* never overwrite the full cache */
        info->phase = CB_SAVE;                                      /* with a memory-decimated subset */
        TICK();
        cache_store(cache_dir, folder, sig, ix);                    /* best-effort; aborts on cancel */
    }
    index_stamp(ix);
    info->nslices = ix->nslices;
    info->phase = CB_DONE;
    return ix;
#undef TICK
}
