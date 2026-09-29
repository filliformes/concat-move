/* Concat session simulator: drives the REAL dsp.so through a full user session on
 * the device, in its own process (never touches the live module).
 *
 *   S1 first launch (create with no state -> auto-pick -> analysis), UI contract reads
 *   S2 play a note in the default mode, release
 *   S3 sweep EVERY declared parameter like a knob turn (enums: every option)
 *   S4 Map mode: 16 pads with velocity + poly pressure
 *   S5 browse every factory recipe
 *   S6 stress: max density / size / poly / k / radius
 *   S7 Mosaic with a synthetic live input
 *   S8 state save -> destroy -> create(with state) -> compare every param
 *   T1..T4 module swap / teardown timing, incl. destroy DURING a forced folder rebuild
 *
 * Reports per-call latency vs the SPI slack (~2370 us) and Schwung's 1 ms
 * param-slow threshold, render cost per 128-frame block, and audio health.
 * Build: aarch64-linux-gnu-gcc -O2 -o session_sim session_sim.c -ldl -lm -lpthread
 * Run:   ./session_sim <dsp.so> <module_dir> <folder_for_teardown_test>          */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sched.h>

typedef struct {
    uint32_t api_version;
    void* (*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*on_midi)(void *, const uint8_t *, int, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    int   (*get_error)(void *, char *, int);
    void  (*render_block)(void *, int16_t *, int);
} api_t;

typedef struct {                       /* leading fields of host_api_v1_t */
    uint32_t api_version; int sample_rate; int frames_per_block;
    uint8_t *mapped_memory; int audio_out_offset; int audio_in_offset;
    uint8_t  pad[4096];                /* zero tail in case the plugin reads further */
} host_t;

#define FR 128
#define SR 44100
#define BUDGET_US 2370.0
#define SLOW_US   1000.0

static api_t  *A;
static host_t  H;
static uint8_t SHM[65536];
static const char *OUTDIR = "/data/UserData/concat_sim";

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

/* ---------- per-call latency table ---------- */
typedef struct { char key[56]; long n; double max, sum; } kt_t;
static kt_t KT[400]; static int NKT;
static void kt_add(const char *op, const char *key, double us) {
    char k[56]; snprintf(k, sizeof k, "%s:%s", op, key);
    for (int i = 0; i < NKT; i++) if (!strcmp(KT[i].key, k)) { KT[i].n++; KT[i].sum += us; if (us > KT[i].max) KT[i].max = us; return; }
    if (NKT < 400) { snprintf(KT[NKT].key, sizeof KT[NKT].key, "%s", k); KT[NKT].n = 1; KT[NKT].sum = us; KT[NKT].max = us; NKT++; }
}
static char GBUF[70000];
static void SP(void *in, const char *k, const char *v) { double t = now_us(); A->set_param(in, k, v); kt_add("set", k, now_us() - t); }
static int GP(void *in, const char *k) {
    double t = now_us(); int n = A->get_param(in, k, GBUF, sizeof GBUF); kt_add("get", k, now_us() - t);
    if (n >= 0 && n < (int)sizeof GBUF) GBUF[n] = 0; else if (n >= (int)sizeof GBUF) GBUF[sizeof GBUF - 1] = 0; else GBUF[0] = 0;
    return n;
}
static void MIDI(void *in, uint8_t a, uint8_t b, uint8_t c) { uint8_t m[3] = { a, b, c }; double t = now_us(); A->on_midi(in, m, 3, 0); kt_add("midi", "", now_us() - t); }

/* ---------- synthetic live input (Mosaic) ---------- */
static unsigned RNG = 12345; static long INCNT; static double INPH;
static void fill_input(void) {
    int16_t *in = (int16_t *)(SHM + H.audio_in_offset);
    for (int i = 0; i < FR; i++) {
        RNG = RNG * 1664525u + 1013904223u;
        double noise = ((RNG >> 9) & 0xffff) / 32768.0 - 1.0;
        double env = ((INCNT / 4410) % 2) ? 0.6 : 0.05;                 /* 100 ms bursts */
        double s = env * (0.5 * sin(INPH) + 0.3 * noise);
        INPH += 2.0 * M_PI * 220.0 / SR * (1 + ((INCNT / 22050) % 4));  /* pitch steps */
        in[i * 2] = in[i * 2 + 1] = (int16_t)(s * 32767.0);
        INCNT++;
    }
}

/* ---------- render + audio health ---------- */
typedef struct { char name[64]; long blocks, over, clips, clicks, silent, cover; double rsum, rmax, peak, sumsq, dcsum, csum, cmax; long nsamp; } phase_t;
static double CT[400000];
static double cpu_us(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static phase_t P;
static double RT[400000]; static int NRT;
static void phase_begin(const char *name) { memset(&P, 0, sizeof P); snprintf(P.name, sizeof P.name, "%s", name); NRT = 0; }
static void render(void *in, int nb, int pace_us) {
    static int16_t buf[FR * 2];
    for (int b = 0; b < nb; b++) {
        fill_input();
        double t = now_us(), c = cpu_us(); A->render_block(in, buf, FR); double dt = now_us() - t, dc = cpu_us() - c;
        P.csum += dc; if (dc > P.cmax) P.cmax = dc; if (dc > BUDGET_US) P.cover++; if (NRT < 400000) CT[NRT] = dc;
        P.blocks++; P.rsum += dt; if (dt > P.rmax) P.rmax = dt; if (dt > BUDGET_US) P.over++; if (NRT < 400000) RT[NRT++] = dt;
        int nz = 0;
        for (int i = 0; i < FR * 2; i++) {
            int s = buf[i]; if (s) nz = 1;
            double x = s / 32768.0; if (fabs(x) > P.peak) P.peak = fabs(x);
            P.sumsq += x * x; P.dcsum += x; P.nsamp++;
            if (s >= 32767 || s <= -32767) P.clips++;
        }
        for (int ch = 0; ch < 2; ch++) for (int i = 1; i < FR; i++) {
            int d = buf[i * 2 + ch] - buf[(i - 1) * 2 + ch];
            if (d > 19660 || d < -19660) P.clicks++;                    /* >0.6 FS sample-to-sample jump */
        }
        if (!nz) P.silent++;
        if (pace_us > 0) usleep(pace_us);
    }
}
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static void phase_end(void) {
    qsort(RT, NRT, sizeof(double), cmpd); qsort(CT, NRT, sizeof(double), cmpd);
    double p99 = NRT ? RT[(int)(NRT * 0.99)] : 0.0, c99 = NRT ? CT[(int)(NRT * 0.99)] : 0.0;
    printf("    cpu-time/block: avg=%5.0f p99=%5.0f max=%6.0f us  over-budget=%ld/%ld\n", P.blocks ? P.csum / P.blocks : 0.0, c99, P.cmax, P.cover, P.blocks);
    double rms = P.nsamp ? sqrt(P.sumsq / P.nsamp) : 0.0;
    printf("[%-30s] blk=%5ld render avg=%5.0f p99=%5.0f max=%6.0f us over=%ld | peak=%6.1f rms=%6.1f dBFS clips=%ld clicks?=%ld silent=%ld dc=%+.4f\n",
           P.name, P.blocks, P.blocks ? P.rsum / P.blocks : 0.0, p99, P.rmax, P.over,
           20 * log10(P.peak + 1e-9), 20 * log10(rms + 1e-9), P.clips, P.clicks, P.silent, P.nsamp ? P.dcsum / P.nsamp : 0.0);
}

/* ---------- chain_params scan ---------- */
typedef struct { char key[48], type[12]; double mn, mx; int nopt; char opt[16][24]; } pd_t;
static pd_t PD[160]; static int NPD;
static void parse_params(const char *js) {
    NPD = 0; const char *p = js;
    while ((p = strstr(p, "{\"key\":\"")) && NPD < 160) {
        p += 8; const char *e = strchr(p, '"'); if (!e) break;
        pd_t *d = &PD[NPD]; memset(d, 0, sizeof *d);
        snprintf(d->key, sizeof d->key, "%.*s", (int)(e - p), p);
        const char *next = strstr(e, "{\"key\":\"");
        const char *t = strstr(e, "\"type\":\"");
        if (t && (!next || t < next)) { t += 8; const char *te = strchr(t, '"'); snprintf(d->type, sizeof d->type, "%.*s", (int)(te - t), t); }
        const char *mn = strstr(e, "\"min\":"); if (mn && (!next || mn < next)) d->mn = atof(mn + 6);
        const char *mx = strstr(e, "\"max\":"); if (mx && (!next || mx < next)) d->mx = atof(mx + 6);
        const char *op = strstr(e, "\"options\":[");
        if (op && (!next || op < next)) {
            op += 11;
            while (*op && *op != ']' && d->nopt < 16) {
                if (*op == '"') { const char *oe = strchr(op + 1, '"'); snprintf(d->opt[d->nopt++], 24, "%.*s", (int)(oe - op - 1), op + 1); op = oe + 1; }
                else op++;
            }
        }
        NPD++; p = e;
    }
}

static void dump(const char *name, const char *buf, int n) {
    char path[600]; snprintf(path, sizeof path, "%s/%s", OUTDIR, name);
    FILE *f = fopen(path, "w"); if (!f) return; if (n > 0) fwrite(buf, 1, (size_t)n, f); fclose(f);
}
static int is_audio(const char *n) {
    const char *d = strrchr(n, '.'); if (!d) return 0;
    return !strcasecmp(d, ".wav") || !strcasecmp(d, ".flac") || !strcasecmp(d, ".mp3");
}
static int first_audio_in(const char *dir, char *out, int outsz) {
    DIR *dp = opendir(dir); if (!dp) return 0;
    char best[256] = { 0 }; struct dirent *e;
    while ((e = readdir(dp))) if (e->d_name[0] != '.' && is_audio(e->d_name) && (!best[0] || strcmp(e->d_name, best) < 0)) snprintf(best, sizeof best, "%s", e->d_name);
    closedir(dp);
    if (!best[0]) return 0;
    snprintf(out, (size_t)outsz, "%s/%s", dir, best); return 1;
}
static double wait_ready(void *in, double max_s) {
    double t0 = now_us();
    for (;;) {
        render(in, 20, 2900);
        GP(in, "analyze"); GP(in, "map_live"); GP(in, "map_pts");   /* what the UI polls */
        GP(in, "analyze");
        if (atoi(GBUF) >= 100) return (now_us() - t0) / 1e6;
        if ((now_us() - t0) / 1e6 > max_s) return -1.0;
    }
}
static void fmtv(const pd_t *d, double v, char *vb, int n) {
    if (!strcmp(d->type, "int")) snprintf(vb, (size_t)n, "%d", (int)lround(v)); else snprintf(vb, (size_t)n, "%.4f", v);
}

int main(int argc, char **argv) {
    const char *so     = argc > 1 ? argv[1] : "/data/UserData/concat_sim/dsp.so";
    const char *moddir = argc > 2 ? argv[2] : "/data/UserData/concat_sim";
    const char *bigdir = argc > 3 ? argv[3] : "/data/UserData/UserLibrary/Samples/POSSIBLES";
    setvbuf(stdout, NULL, _IOLBF, 0);
    setpriority(PRIO_PROCESS, 0, 10);                 /* be gentle to the live Move */

    void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    api_t *(*init)(const void *) = (api_t *(*)(const void *))dlsym(h, "move_plugin_init_v2");
    if (!init) { printf("no move_plugin_init_v2\n"); return 1; }
    memset(&H, 0, sizeof H);
    H.api_version = 1; H.sample_rate = SR; H.frames_per_block = FR;
    H.mapped_memory = SHM; H.audio_out_offset = 0; H.audio_in_offset = 4096;
    A = init(&H);
    printf("api_version=%u get_error=%s\n", A->api_version, A->get_error ? "set" : "NULL");

    /* ===== RESTORE mode: the host's real order is create(NULL) then set_param("state").
     * Does the default-corpus auto-pick race the restore and clobber the saved corpus? ===== */
    if (argc > 4 && !strcmp(argv[4], "restore")) {
        char saved[512];
        if (!first_audio_in(bigdir, saved, sizeof saved)) { printf("no audio in %s\n", bigdir); return 1; }
        char state[1400];
        snprintf(state, sizeof state, "src_scope=1\ndensity=20.000000\ncorpus_file=%s\npreset_idx=0\n", saved);
        int wrong = 0, runs = 20;
        for (int r = 0; r < runs; r++) {
            void *in = A->create_instance(moddir, NULL);           /* host passes NULL config */
            if (r % 2) usleep(2000);                               /* vary the gap the host leaves */
            SP(in, "state", state);                                /* host restores state after create */
            wait_ready(in, 120);
            for (int k = 0; k < 40; k++) render(in, 10, 2900);     /* let any second build land */
            GP(in, "corpus_file");
            int ok = !strcmp(GBUF, saved);
            if (!ok) { wrong++; printf("  run %2d: corpus_file CLOBBERED -> [%s]\n", r, GBUF); }
            A->destroy_instance(in);
        }
        printf("RESTORE saved=[%s]  clobbered %d / %d runs\n", saved, wrong, runs);
        printf("DONE\n");
        return 0;
    }

    /* ===== PERF mode: big corpus, worst-case query load ===== */
    if (argc > 4 && !strcmp(argv[4], "perf")) {
        char big[512];
        if (!first_audio_in(bigdir, big, sizeof big)) { printf("no audio in %s\n", bigdir); return 1; }
        void *in = A->create_instance(moddir, NULL); wait_ready(in, 60);
        GP(in, "_dbg"); const char *ns = strstr(GBUF, "nslices="); int n0 = ns ? atoi(ns + 8) : -1;
        double t0p = now_us();
        SP(in, "src_scope", "Folder"); SP(in, "corpus_file", big);
        double tr = wait_ready(in, 1800);
        double tlive = -1;                      /* "Analyze 100" is not "new corpus live": wait for the swap */
        for (int k = 0; k < 2400; k++) {
            render(in, 10, 2900);
            GP(in, "_dbg"); ns = strstr(GBUF, "nslices="); int n1 = ns ? atoi(ns + 8) : -1;
            if (n1 != n0 && n1 > 0) { tlive = (now_us() - t0p) / 1e6; break; }
        }
        GP(in, "_dbg"); printf("PERF folder=%s analyze=100 after %.1fs, new corpus LIVE after %.1fs  %s\n", bigdir, tr, tlive, GBUF);
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(1, &cs); sched_setaffinity(0, sizeof cs, &cs); setpriority(PRIO_PROCESS, 0, 0);   /* quiet core; CPU-time metric is preemption-proof */
        const char *modes[2] = { "Raw", "Learned" };
        for (int m = 0; m < 2; m++) {
            SP(in, "map_mode", modes[m]);
            char nm[64];
            SP(in, "play_mode", "Notes");
            snprintf(nm, sizeof nm, "PERF %s 8-note chord", modes[m]); phase_begin(nm);
            for (int k = 0; k < 8; k++) MIDI(in, 0x90, (uint8_t)(60 + k), 100);
            render(in, 690, 0);
            for (int k = 0; k < 8; k++) MIDI(in, 0x80, (uint8_t)(60 + k), 0);
            render(in, 100, 0); phase_end();
            snprintf(nm, sizeof nm, "PERF %s 16-pad chord d100", modes[m]); phase_begin(nm);
            SP(in, "density", "100"); SP(in, "k_nearest", "16"); SP(in, "radius", "1");
            for (int k = 0; k < 16; k++) MIDI(in, 0x90, (uint8_t)(48 + k), 110);
            render(in, 690, 0);
            for (int k = 0; k < 16; k++) MIDI(in, 0x80, (uint8_t)(48 + k), 0);
            render(in, 100, 0); phase_end();
            SP(in, "preset_index", "0");
        }
        for (int i = 0; i < NKT; i++) if (KT[i].max > 150.0) printf("  %-30s n=%7ld avg=%8.0f us max=%9.0f us\n", KT[i].key, KT[i].n, KT[i].sum / KT[i].n, KT[i].max);
        A->destroy_instance(in);
        printf("DONE\n");
        return 0;
    }

    /* ===== S1 first launch ===== */
    double t0 = now_us();
    void *in = A->create_instance(moddir, NULL);
    double tc = now_us() - t0; kt_add("create", "fresh", tc);
    printf("S1 create_instance(no state): %.0f us\n", tc);
    int n = GP(in, "chain_params"); dump("chain_params.json", GBUF, n); parse_params(GBUF);
    printf("S1 chain_params: %d bytes, %d entries\n", n, NPD);
    n = GP(in, "ui_hierarchy"); dump("ui_hierarchy.json", GBUF, n); printf("S1 ui_hierarchy: %d bytes\n", n);
    n = GP(in, "name"); printf("S1 name=[%s]  unknown-key get_param -> %d (expect -1)\n", GBUF, A->get_param(in, "no_such_key_xyz", GBUF, 64));
    phase_begin("S1 first launch (analysing)");
    double tready = wait_ready(in, 180);
    phase_end();
    GP(in, "corpus_file"); printf("S1 auto-picked corpus_file=[%s]  ready after %.2f s\n", GBUF, tready);
    GP(in, "_dbg"); printf("S1 dbg: %s\n", GBUF);
    GP(in, "map_pts"); printf("S1 map_pts: %zu bytes\n", strlen(GBUF));

    /* ===== S2 play a note in the default mode ===== */
    GP(in, "play_mode"); printf("S2 default play_mode=%s\n", GBUF);
    phase_begin("S2 hold C4 2s");     MIDI(in, 0x90, 60, 100); render(in, 690, 0); phase_end();
    phase_begin("S2 release 1s");     MIDI(in, 0x80, 60, 0);   render(in, 345, 0); phase_end();

    /* ===== S3 sweep every parameter (note held) ===== */
    printf("--- S3 per-parameter sweeps (like turning each knob end to end) ---\n");
    MIDI(in, 0x90, 60, 100);
    for (int i = 0; i < NPD; i++) {
        pd_t *d = &PD[i];
        if (!strcmp(d->type, "filepath") || !strcmp(d->type, "canvas") || !strcmp(d->type, "string")) continue;
        if (!strcmp(d->key, "rescan") || !strcmp(d->key, "analyze") || !strcmp(d->key, "src_scope") ||
            !strcmp(d->key, "preset_index") || !strcmp(d->key, "preset_count")) continue;
        int gn = GP(in, d->key); char save[64]; snprintf(save, sizeof save, "%s", GBUF);
        if (gn < 0) printf("  !! get_param(%s) -> %d : declared key does not answer\n", d->key, gn);
        char ph[64]; snprintf(ph, sizeof ph, "sweep %s", d->key); phase_begin(ph);
        if (!strcmp(d->type, "enum")) {
            for (int o = 0; o < d->nopt; o++) {
                SP(in, d->key, d->opt[o]); GP(in, d->key);
                if (strcmp(GBUF, d->opt[o])) printf("  !! enum %s: set '%s' read back '%s'\n", d->key, d->opt[o], GBUF);
                render(in, 24, 0);
            }
        } else {
            char vb[32];
            for (int s = 0; s <= 24; s++) { fmtv(d, d->mn + (d->mx - d->mn) * s / 24.0, vb, sizeof vb); SP(in, d->key, vb); render(in, 6, 0); }
            for (int s = 24; s >= 0; s--) { fmtv(d, d->mn + (d->mx - d->mn) * s / 24.0, vb, sizeof vb); SP(in, d->key, vb); render(in, 2, 0); }
        }
        phase_end();
        SP(in, d->key, save);
        MIDI(in, 0x80, 60, 0); MIDI(in, 0x90, 60, 100);
    }
    MIDI(in, 0x80, 60, 0);

    /* ===== S4 Map mode pads + pressure ===== */
    SP(in, "play_mode", "Map");
    phase_begin("S4 Map: 16 pads + pressure");
    for (int p = 0; p < 16; p++) {
        uint8_t note = (uint8_t)(36 + p);
        MIDI(in, 0x90, note, (uint8_t)(40 + p * 5));
        for (int a = 0; a < 8; a++) { MIDI(in, 0xA0, note, (uint8_t)(a * 16)); render(in, 4, 0); }
        MIDI(in, 0x80, note, 0); render(in, 8, 0);
    }
    phase_end();

    /* ===== S5 recipes ===== */
    printf("--- S5 recipes ---\n");
    GP(in, "preset_count"); int nrec = atoi(GBUF);
    for (int r = 0; r < nrec; r++) {
        char rb[8]; snprintf(rb, sizeof rb, "%d", r); SP(in, "preset_index", rb);
        GP(in, "preset_name"); char nm[64]; snprintf(nm, sizeof nm, "recipe %d %s", r, GBUF);
        GP(in, "play_mode"); char pm[16]; snprintf(pm, sizeof pm, "%s", GBUF);
        phase_begin(nm); MIDI(in, 0x90, (uint8_t)(48 + r), 100); render(in, 345, 0); MIDI(in, 0x80, (uint8_t)(48 + r), 0); render(in, 100, 0); phase_end();
        printf("      (mode=%s)\n", pm);
    }
    SP(in, "preset_index", "0");

    /* ===== S6 stress ===== */
    SP(in, "play_mode", "Map"); SP(in, "density", "100"); SP(in, "grain_size", "2000"); SP(in, "poly", "128");
    SP(in, "k_nearest", "16"); SP(in, "radius", "1"); SP(in, "smooth", "1"); SP(in, "pos_jitter", "1"); SP(in, "level_norm", "1");
    phase_begin("S6 stress: max everything");  MIDI(in, 0x90, 60, 127); render(in, 1380, 0); MIDI(in, 0x80, 60, 0); phase_end();
    SP(in, "preset_index", "0");

    /* ===== S7 Mosaic ===== */
    SP(in, "play_mode", "Mosaic"); SP(in, "dry_mix", "0.3");
    phase_begin("S7 Mosaic live input 3s"); MIDI(in, 0x90, 60, 100); render(in, 1035, 0); MIDI(in, 0x80, 60, 0); phase_end();
    SP(in, "preset_index", "0");

    /* ===== S8 state round trip ===== */
    SP(in, "density", "37"); SP(in, "grain_env", "Gauss"); SP(in, "level_norm", "0.73"); SP(in, "play_mode", "Map");
    n = GP(in, "state"); char *state = strdup(GBUF); dump("state.txt", state, n); printf("S8 state: %d bytes\n", n);
    static char before[160][64];
    for (int i = 0; i < NPD; i++) { GP(in, PD[i].key); snprintf(before[i], 64, "%s", GBUF); }
    t0 = now_us(); A->destroy_instance(in); printf("S8 destroy (after full session): %.0f us\n", now_us() - t0);
    t0 = now_us(); in = A->create_instance(moddir, state); printf("S8 create_instance(with state): %.0f us\n", now_us() - t0);
    int mism = 0;
    for (int i = 0; i < NPD; i++) {
        if (!strcmp(PD[i].type, "canvas") || !strcmp(PD[i].type, "string") || !strcmp(PD[i].key, "analyze") || !strcmp(PD[i].key, "rescan")) continue;
        GP(in, PD[i].key);
        if (strcmp(before[i], GBUF)) {
            double a = atof(before[i]), b = atof(GBUF);
            int numeq = before[i][0] && GBUF[0] && fabs(a - b) < 1e-3 && (a != 0.0 || b != 0.0 || before[i][0] == '0');
            if (!numeq) { printf("  !! state mismatch %-14s before=[%s] after=[%s]\n", PD[i].key, before[i], GBUF); mism++; }
        }
    }
    printf("S8 state round-trip mismatches: %d\n", mism);
    phase_begin("S8 reload (cache)"); double trl = wait_ready(in, 180); phase_end();
    printf("S8 reload ready after %.2f s\n", trl);

    /* ===== T teardown / module swap ===== */
    t0 = now_us(); A->destroy_instance(in); printf("T1 destroy idle: %.0f us\n", now_us() - t0);

    in = A->create_instance(moddir, NULL); render(in, 17, 2900);
    t0 = now_us(); A->destroy_instance(in); printf("T2 destroy ~50 ms after a fresh create (auto-pick in flight): %.0f us\n", now_us() - t0);

    char big[512];
    if (first_audio_in(bigdir, big, sizeof big)) {
        in = A->create_instance(moddir, NULL); wait_ready(in, 60);
        SP(in, "src_scope", "Folder"); SP(in, "corpus_file", big); SP(in, "rescan", "1");
        render(in, 170, 2900);
        GP(in, "analyze"); printf("T3 progress before destroy: %s%%  (folder %s)\n", GBUF, bigdir);
        alarm(600);
        t0 = now_us(); A->destroy_instance(in);
        alarm(0);
        printf("T3 destroy DURING a forced folder rebuild: %.0f us\n", now_us() - t0);
    } else printf("T3 skipped: no audio in %s\n", bigdir);

    double wmax = 0;
    for (int k = 0; k < 10; k++) { in = A->create_instance(moddir, NULL); render(in, 5, 0); t0 = now_us(); A->destroy_instance(in); double d = now_us() - t0; if (d > wmax) wmax = d; }
    printf("T4 rapid create/destroy x10: worst destroy %.0f us\n", wmax);

    printf("--- per-call latency (max over whole session; SPI slack ~2370 us, param-slow at 1000 us) ---\n");
    for (int i = 0; i < NKT; i++)
        if (KT[i].max > 150.0)
            printf("  %-30s n=%7ld avg=%8.0f us  max=%9.0f us %s\n", KT[i].key, KT[i].n, KT[i].sum / KT[i].n, KT[i].max,
                   KT[i].max > BUDGET_US ? "<== EXCEEDS SPI SLACK" : KT[i].max > SLOW_US ? "<- over 1 ms (param-slow)" : "");
    printf("DONE\n");
    return 0;
}
