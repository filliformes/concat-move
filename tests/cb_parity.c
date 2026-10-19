/* Corpus Builder parity, native side: run the builder's analysis entry points
 * (src/wasm/cb_wasm.c) compiled for this machine and print slices + raw descriptors.
 * tests/cb_parity.js prints the same from the WASM build; compare the two.
 *   cb_parity <file> [start_s end_s]   (onset, 20..2000 ms, gate 0.02 = device defaults) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void *cb_alloc(int); int cb_load(uint8_t *, int, const char *); int cb_analyse(double, double, int, int, int, float);
int *cb_slices(void); float *cb_desc(void); int cb_ndesc(void); int cb_decode(void);

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *fp = fopen(argv[1], "rb"); if (!fp) { printf("open failed\n"); return 1; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *buf = cb_alloc((int)sz);
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { printf("read failed\n"); return 1; }
    fclose(fp);
    if (!cb_load(buf, (int)sz, argv[1])) { printf("decode failed\n"); return 1; }
    double a = argc > 3 ? atof(argv[2]) : 0, b = argc > 3 ? atof(argv[3]) : 0;
    printf("frames %d\n", cb_decode());
    int n = cb_analyse(a, b, 0, 20, 2000, 0.02f), nd = cb_ndesc();
    printf("slices %d\n", n);
    for (int i = 0; i < n; i++) {
        printf("%d %d", cb_slices()[2 * i], cb_slices()[2 * i + 1]);
        for (int d = 0; d < nd; d++) printf(" %.7g", cb_desc()[(size_t)i * nd + d]);
        printf("\n");
    }
    return 0;
}
