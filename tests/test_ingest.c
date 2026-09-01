/* Concat — ingest tests (folder scan + decode + resample). Runs ON the Move,
 * so it reads real audio from the device's sample library. */
#include "ingest.h"
#include "corpus.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int npass = 0, nfail = 0;
static void check(const char *name, int cond, const char *extra) {
    printf("  %s %s%s%s\n", cond ? "PASS" : "FAIL", name, extra ? "  -> " : "", extra ? extra : "");
    if (cond) npass++; else nfail++;
}

int main(void) {
    corpus_folder_t folders[INGEST_MAX_FOLDERS];
    int nf = ingest_scan_folders(folders, INGEST_MAX_FOLDERS);
    printf("  scanned %d folders:\n", nf);
    for (int i = 0; i < nf; i++)
        printf("    %-28s %5d files   %s\n", folders[i].name, folders[i].nfiles, folders[i].path);
    check("scan found folders", nf > 0, NULL);
    if (nf == 0) { printf("\n%d passed, %d failed\n", npass, nfail); return 1; }

    /* Decode the first audio file of the first folder that has any. */
    static char paths[8][INGEST_PATH];
    int fi = 0, na = 0;
    for (fi = 0; fi < nf; fi++) { na = ingest_list_audio(folders[fi].path, paths, 8); if (na > 0) break; }
    char buf[700];
    snprintf(buf, sizeof(buf), "%d in %s", na, fi < nf ? folders[fi].name : "?");
    check("list audio in a folder", na > 0, buf);
    if (na == 0) { printf("\n%d passed, %d failed\n", npass, nfail); return 1; }

    int len = 0;
    float *audio = ingest_decode(paths[0], 44100, &len);
    snprintf(buf, sizeof(buf), "%d frames (%.2fs @44.1k) from %s", len, len / 44100.0, paths[0]);
    check("decode → mono 44.1k", audio != NULL && len > 0, buf);
    if (!audio) { printf("\n%d passed, %d failed\n", npass, nfail); return 1; }

    int mid = len / 2;
    check("decoded samples finite", isfinite(audio[0]) && isfinite(audio[mid]) && isfinite(audio[len - 1]), NULL);
    float peak = 0.0f;
    for (int i = 0; i < len; i++) { float v = fabsf(audio[i]); if (v > peak) peak = v; }
    snprintf(buf, sizeof(buf), "peak %.3f", peak);
    check("decoded audio non-silent", peak > 1e-4f, buf);

    /* End-to-end: describe the decoded audio. */
    analysis_t *a = analysis_create(44100, 1024, 40, 13);
    float dv[NDESC];
    int dl = len > 44100 ? 44100 : len;
    concat_slice_descriptor(a, audio, dl, dv);
    snprintf(buf, sizeof(buf), "centroid=%.0fHz pitch=%.0fHz loud=%.3f", dv[D_CENTROID_MEAN], dv[D_PITCH], dv[D_LOUD_MEAN]);
    check("describe decoded audio", isfinite(dv[D_CENTROID_MEAN]) && isfinite(dv[D_PITCH]) && dv[D_LOUD_MEAN] > 0.0f, buf);
    analysis_destroy(a);
    free(audio);

    printf("\n%d passed, %d failed\n", npass, nfail);
    return nfail ? 1 : 0;
}
