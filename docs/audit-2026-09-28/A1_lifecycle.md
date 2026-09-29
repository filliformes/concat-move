# A1: Concat lifecycle, threading, teardown and state audit

Scope: `C:/Users/filli/Projects/concat-move` (working tree incl. uncommitted Level Norm / Pos Jitter / equal-power seam diff), against Schwung main (8cc9c5e, v1.5.0) cloned in the scratchpad. Read-only audit: nothing built, nothing run on device.

Line numbers: `concat.c` = `src/dsp/concat.c` (working tree). Schwung paths are relative to the clone root.

---

## 0. TL;DR

* **Swapping the module calls Concat's `destroy_instance` on the SPI audio callback.** That function `pthread_join`s an analyst thread that has no cancellation point inside `corpus_index_build`. While a build is running, the whole Move stops: audio, OLED, pads and LEDs all ride the same SPI frame. Nothing in Schwung times this out and nothing recovers it. The stall lasts as long as the rest of the build. That is a few seconds on a cache hit and minutes on a cache miss with the 512 MB default pool. To a user it looks exactly like a hard freeze. **This is the most likely cause.**
* Several Concat bugs make "a build is in flight when the user swaps" much more likely than it looks:
  * The auto-pick path **always** fires in a chain, because the chain passes `NULL` config to `create_instance` and restores state later. It races the state restore and can start one or two extra builds. It can also overwrite the user's saved `corpus_file`.
  * Cache validity depends on the `MemAvailable` reading at arm time, so an unchanged corpus can miss the cache and re-analyse from scratch.
  * Every Rescan knob detent re-arms a forced rebuild.
* The deploy hazard is real: `scp` rewrites the inode that `MoveOriginal` has `dlopen`ed. Note, however, that `crash_maps.txt` is **also written on SIGTERM** (`src/schwung_shim.c:3891-3898,4135-4143`). `restart-move.sh` sends SIGTERM first. So a `crash_maps.txt` timestamp alone does not prove a SIGSEGV. Check the "Caught SIGxxx" line.
* The uncommitted diff (Level Norm / Pos Jitter / equal-power seam) adds no blocking and no unbounded loop. Its per-grain loops stay bounded (see 6.4). The diff is **not** the cause. It only coincides with a deploy, which means a fresh process and fresh builds.

---

## 1. What the host does on a module swap (evidence)

### 1.1 Call path: all of it on the SPI callback thread

1. The shadow UI picker calls `applyChainComponentPick`, then `applyComponentSelectionConfirmed`, then `setSlotParam(slot, "synth:module", id)` (`src/shadow/shadow_ui.js:14967-15100`, write at `~15100`). It does **not** read `synth:state` before the write. The only `:state` reads happen on page plan (`componentTrailingMenus`, `shadow_ui.js:2554`) and on autosave or preset save.
2. The SHM param request is served by `shadow_inprocess_handle_param_request()` (`src/host/shadow_chain_mgmt.c:4026`). The shim calls it every frame from the pre-ioctl section of the SPI hook (`src/schwung_shim.c:6725-6728`). The comment at `shadow_chain_mgmt.c:990-996` states it explicitly: "runs from shim_pre_transfer on the SPI audio thread".
3. It forwards to the chain's `set_param(instance, "synth:module", value)` (`shadow_chain_mgmt.c:5051-5058`). The comment there says the load happens "synchronously, right here, on the SPI callback".
4. Chain `v2_set_param` handles `synth:module` (`src/modules/chain/dsp/chain_host.c:1231-1245`) as follows:
   * `v2_synth_panic`
   * `v2_unload_synth`, which calls **Concat `destroy_instance`** (`chain_host.c:217-218`) and then **`dlclose(concat)`** (`chain_host.c:221-223`)
   * `v2_load_synth(new)`, which does `dlopen`, then `create_instance` (`chain_host.c:621`), then a 256 KB `malloc` and `get_param("chain_params")` / `get_param("ui_hierarchy")` (`:630-636`), then `fopen` of module.json, and so on.
5. Back in `shadow_chain_mgmt.c:5067-5082` there is a synchronous `get_param("synth:default_forward_channel")` read-back.

`docs/REALTIME_SAFETY.md:43-48` confirms this: "create_instance, destroy_instance, set_param, get_param, on_midi… render_block… all run on the SPI callback. There is no control thread." `:205-225` measured a module load at 673 ms of a frozen system ("The whole system stops, not just the slot being loaded").

### 1.2 Same-module re-create or "preview"
No module preview instantiation exists for sound generators. Re-picking the same module is a full unload and reload, with `destroy_instance` then `create_instance` on a new instance (`shadow_ui.js` comment at ~15085: "re-picking the same module is a reload"). Patch loads use the same unload-then-load sequence (`src/modules/chain/dsp/chain_patch.c:1548-1583`).

### 1.3 How state reaches a sound generator (important for section 6)
* `create_instance(synth_path, pack_config)`: `pack_config` is `NULL` except for "pack" modules (`chain_host.c:484-532,621`). **Concat's `json_defaults` is therefore always NULL in a chain.**
* State arrives afterwards through `set_param(inst, "state", patch->synth_state)` (`chain_patch.c:1579-1583`). Before that come `set_param("preset", "<n>")` (`:1564-1567`) and 16 CC1=0 `on_midi` calls (`:1572-1577`). All of this runs on the same SPI call as the load.
* Stored format: the UI stores `synth:state` as a JSON string (JSON.stringify escapes the newlines). The chain decodes it with `json_decode_quoted_string` (`chain_patch.c:922-927`) before passing it back, so Concat gets its raw `k=v\n` text. If the stored value is a JSON object, the chain compacts it (`:909-921`). That never happens for Concat.
* Buffer sizes: `MAX_SYNTH_STATE_LEN` = 16384 (`chain_internal.h:297`). `SHADOW_PARAM_VALUE_LEN` = 131072 (`src/host/shadow_constants.h:197`).

### 1.4 Watchdogs, timeouts, heal
* **No SPI stall watchdog exists.** The only "watchdog" in the shim respawns `shadow_ui` and runs on the SPI thread itself (`schwung_shim.c:4711-4718`). A blocked SPI thread therefore disables it too.
* The boot-healthy counter (`schwung_shim.c:6595-6605`) only counts frames.
* `schwung-heal` is about tool IDs (`src/host/heal_tool_id.h`) and is unrelated.
* The UI side has claim and response timeouts (`src/shadow/shadow_ui.c:946-1261`). They only make the JS give up. They cannot un-block the DSP.

The chain host itself documents the exact hazard Concat triggers (`chain_host.c:133-155`): "pthread_join is a futex wait with NO priority inheritance, so a FIFO-70 thread is waiting on a SCHED_OTHER one". Schwung bounds its own join by polling a stop flag between units of work. **Concat does not.**

### 1.5 Crash handler
`crash_signal_handler` (`schwung_shim.c:3891-4050`) is installed for **SIGSEGV, SIGBUS, SIGABRT and SIGTERM** (`:4135-4143`). It logs `Caught <SIG> si_addr pc lr` through `unified_log_crash`, writes `/data/UserData/schwung/crash_maps.txt` (`:4020-4040`) and then calls `_exit`. `src/restart-move.sh:31-36` sends SIGTERM first, so **a restart also produces crash_maps.txt**.

---

## 2. Ranked findings

| # | Sev | Conf | Title |
|---|-----|------|-------|
| F1 | BLOCKER | High | `destroy_instance` joins a non-cancellable analyst on the SPI callback. The stall lasts for the whole in-flight build (seconds to minutes) |
| F2 | HIGH | High | The auto-pick always fires in a chain (json_defaults is NULL). It races the state restore, can clobber the saved corpus, and starts extra builds |
| F3 | HIGH | High | In-place `scp` of a dlopen'd `dsp.so` corrupts the running code, giving SIGSEGV/SIGBUS or wild execution |
| F4 | HIGH | Med | The cache hit depends on transient `MemAvailable`, so unchanged corpora re-analyse, rewrite 100s of MB, and stretch F1 |
| F5 | HIGH | Med | OOM exposure: 512 MB default pool, decode transients not budgeted (MP3 up to GBs), old and new pools co-resident, no swap |
| F6 | MED | High | `arm_analyst` does `/proc/meminfo` I/O and takes a non-PI mutex on the SPI thread. The analyst holds the same mutex across I/O |
| F7 | MED | Med | Index swap ABA / UAF: grains flushed by pointer compare plus a fixed 60 ms sleep. A reused address or a parked slot leads to reads from a freed pool |
| F8 | MED | High | Big `free()` and `munmap` of the pool plus analysis teardown on the SPI thread in `destroy_instance` |
| F9 | MED | High | A failed build publishes NULL and frees the working corpus (silence) |
| F10 | MED | Med | Rescan and Scope knob detents each queue a full (forced) rebuild |
| F11 | MED | Low-Med | Unbounded directory recursion (depth 8 with symlink fan-out) inside the non-cancellable build |
| F12 | LOW | High | State: a `corpus_file` longer than 255 chars is silently dropped on restore; old states pick up new defaults; `get_param("state")` return can exceed `buf_len` |
| F13 | LOW | High | Unchecked `malloc` in `concat_robust_fit` gives a NULL deref on the analyst, which kills MoveOriginal |
| F14 | LOW | High | `create_instance` spawns a thread and allocates about 100 KB on SPI. Acceptable, but it inherits FIFO 70 until it demotes itself |
| F15 | LOW | High | Miscellaneous realtime costs on SPI get_param: the ~5 KB chain_params format, the `apply_recipe` scan, `map_live` projection. Bounded and fine |

---

### F1 (BLOCKER): the SPI-thread `pthread_join` waits out the whole analysis

**Evidence**
* `concat.c:537-552`: `destroy_instance` sets `shutdown`, signals, then `pthread_join(inst->analyst, NULL)` (line 545). It is called from `chain_host.c:217-218`, which runs on the SPI thread (section 1.1).
* `concat.c:468-504`: the analyst checks `shutdown` **only** at the top of its loop (line 471). Once it passes line 493 it runs `corpus_index_build(...)` (497-498) to completion, then `build_map_pts` (499), the swap, `usleep(60000)` and `corpus_index_free(old)` (500-501). Only then does it see `shutdown`.
* `corpus_engine.c:342-463` has no cancellation parameter. Its cost:
  * Cache miss: `ingest_list_audio` (recursive) for up to 2048 files (`MAX_FILES`, line 14). A `stat` of every file, twice (signature at 40-57 and estimate at 388-395). Then per file a whole-file decode (`ingest_decode`, up to 128 MB file, `ingest.c:160-199`), onset slicing, per-slice MFCC. Then k-means (16 iterations × n × 16 × 36, line 154-194), PCA, a BMP write, and **`cache_store` writing the whole pool to eMMC** (113-138, up to 512 MB).
  * Cache hit: `corpus_signature` stats, then `fread` of the whole pool (`cache_load` 65-110, up to 512 MB), then a BMP write.
* `P_POOL_MB` default is **512** (`concat.c:118`). CLAUDE.md says 128. The cap is `min(pool_mb, MemAvailable-350)` (`concat.c:450-455`). On a stock light set (1258 MB available) that is 512 MB, about 268 M mono samples, or about 101 minutes of audio.

**How long `join` blocks** (rough, CM4, eMMC):

| State of analyst when destroy runs | Join blocks for |
|---|---|
| Idle in `pthread_cond_wait` | Wake latency, µs to low ms |
| In auto-pick directory scan (`concat.c:405-435`) | 1-100 ms, plus the build it arms is skipped because shutdown wins (line 471) |
| Cache hit, 128 MB pool | ~2-4 s (fread ~128 MB plus stats) |
| Cache hit, 512 MB pool | ~6-10 s |
| Cache miss, Scope=File (≤128 MB file, ≤12.7 min stereo WAV) | ~10-30 s (decode, analyse, write cache) |
| Cache miss, Scope=Folder, pool 512 MB | **~1.5-3 min** analysis (40-80× realtime on ~100 min) **plus ~15-25 s** cache write |
| Pathological recursion (F11) | Effectively unbounded |

**Can it block forever?** Checked every path:
* Lost wakeup: none. The wait is predicate-guarded under the mutex (`concat.c:470`), and `shutdown` is set under the mutex and signalled (541-544).
* Shutdown has priority over `arm` and `auto_pick` (line 471 is checked before 475), so a queued rebuild never runs after destroy.
* The thread is joined exactly once. `analyst_started` guards the no-thread case (540). Joining an already-exited thread is fine.
* The auto-pick branch unlocks before scanning (477) and re-locks inside `arm_analyst` (441). There is no self-deadlock.
* The SPI thread holds no lock the analyst needs while joining. The analyst's `malloc`, stdio and `mmap` do not depend on the SPI thread.

So in the healthy case the block is **bounded but can be minutes long**. It becomes **unbounded** with F11 (recursive scan fan-out), with F5 (memory thrash livelock on a swapless system), or if the analyst is starved on cores 0-2. Starvation has no priority inheritance, but RT throttling still gives it about 5%.

**Why it looks like a hard freeze:** the SPI frame carries the audio, the OLED push and the pad/LED I/O. With the SPI thread parked in `futex_wait`, everything user-visible stops. MoveOriginal is not dead, so nothing restarts it. `sshd` keeps working, which is the diagnostic lever (section 3).

**Fix (layered)**

1. Make the build cancellable, which bounds the join to one file's decode (usually under 1-2 s, at worst one 128 MB decode):
```c
/* corpus_engine.h */
corpus_index_t *corpus_index_build(..., volatile int *progress, const volatile int *cancel);
#define CANCELLED(c) ((c) && __atomic_load_n((c), __ATOMIC_RELAXED))
/* check: per file in the decode loop (corpus_engine.c:404), per slice (412),
 * per k-means iteration (166), in scan_audio per entry (ingest.c:101-114),
 * in cache_load/cache_store: fread/fwrite the pool in 4 MB chunks with a check.
 * On cancel: free everything, return NULL. */
```
   Pass `&inst->shutdown` (make it an atomic int) from `analyst_thread`.
2. Never wait more than a few ms on SPI. Use a timed join and orphan the worker when it is busy. Pin the .so so a later `dlclose` cannot unmap code the orphan is still running:
```c
/* analyst_thread(), first thing, OFF the SPI thread: */
Dl_info di;
if (dladdr((void *)analyst_thread, &di) && di.dli_fname)
    dlopen(di.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);   /* never unmapped in this process */
__atomic_store_n(&inst->worker_pinned, 1, __ATOMIC_RELEASE);

/* destroy_instance(): */
set shutdown (atomic) + signal;
struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 5*1000*1000; normalise(&ts);
if (pthread_timedjoin_np(inst->analyst, NULL, &ts) != 0) {
    if (__atomic_load_n(&inst->worker_pinned, __ATOMIC_ACQUIRE)) {
        /* hand ownership of inst (index, analysis, pool) to the worker:
         * it frees them itself when it notices shutdown. */
        __atomic_store_n(&inst->orphaned, 1, __ATOMIC_RELEASE);
        pthread_detach(inst->analyst);
        return;                       /* do NOT free inst here */
    }
    pthread_join(inst->analyst, NULL);/* not yet pinned => it just started, join is short */
}
/* ...normal teardown... */

/* analyst_thread() exit path: */
if (__atomic_load_n(&inst->orphaned, __ATOMIC_ACQUIRE)) { free index(es), analysis, mutex, inst; }
return NULL;
```
   Add `-ldl` to `build.sh` (glibc 2.17 keeps `dlopen` and `dladdr` in libdl, which is GLIBC-safe). The trade-off of `RTLD_NODELETE`: Concat's code stays mapped until MoveOriginal restarts, so re-picking Concat after a redeploy reuses the old code. That is acceptable, because a redeploy already needs a restart (F3).
3. Move the big frees off SPI (see F8). With orphaning in place, the worker already owns the index frees.

---

### F2 (HIGH): the auto-pick always fires and races the state restore

**Evidence**
* `concat.c:524-533`: `set_param(inst, "state", json_defaults)` only runs when `json_defaults` is non-empty. It is always `NULL` in a chain (section 1.3, `chain_host.c:621`). So `corpus_file[0] == 0` and `auto_pick = 1` is signalled **on every instantiation**.
* The analyst wakes immediately on cores 0-2 (`concat.c:475-485`) and checks `inst->corpus_file[0] == 0` **without the lock**. It then runs `auto_pick_corpus` (opendir, stat, list; ms to 100s of ms), then **writes** `inst->corpus_file` (480-481) and `corpus_path` (`set_corpus_path`, 482), then calls `arm_analyst` (483).
* Meanwhile the same SPI call continues into `chain_patch.c:1564-1583` and runs `set_param("state")`. That sets `corpus_file = X` (`concat.c:620-625`) and calls `arm_analyst` with `pending_path = X`.

**Failure scenarios**
1. The analyst finishes the scan **after** the restore. It overwrites `corpus_file` with the auto-picked path and overwrites `pending_path`. The user's corpus is silently replaced. The next autosave persists the wrong `corpus_file`, so the loss is permanent. If the restored `src_scope` was Folder, the auto-pick analyses the **whole first Samples sub-folder** (recursive), which is a long build and widens F1.
2. The analyst finishes **before** the restore. The auto build starts, then the restore arms X, so **two sequential builds** run back to back after every load or boot. A swap during either one triggers F1.
3. Torn strings: `corpus_file` and `corpus_path` are written by two threads with no lock, and `get_param("corpus_file")` (786-787) and `set_corpus_path` read them concurrently.

**Fix**
Decide "no saved corpus" on the SPI side and late, and commit an auto-pick only if nobody set a corpus meanwhile:
```c
/* inst: uint32_t corpus_gen;  (bumped under lock by set_param corpus_file) */
/* set_param("corpus_file"): lock; copy; corpus_gen++; auto_pick = 0; set_corpus_path; snapshot+arm; unlock */

/* render_block: after ~0.5 s of blocks with corpus_file[0]==0 && !auto_pick_sent → lock; auto_pick=1; signal; unlock */

/* analyst auto-pick: */
lock; uint32_t g = inst->corpus_gen; unlock;
if (auto_pick_corpus(f, sizeof f)) {
    lock;
    if (inst->corpus_gen == g && inst->corpus_file[0] == 0) { copy f; corpus_gen++; set_corpus_path; snapshot_locked(); arm = 1; }
    unlock;
}
```
Keep all `corpus_file`/`corpus_path` writes under `inst->lock`, and keep the SPI side's critical section to plain copies (see F6).

---

### F3 (HIGH): the deploy overwrites a mapped `dsp.so` in place

**Evidence:** `scripts/install.sh:9` runs `scp -r dist/concat root@move:.../sound_generators/`, then `chmod` and `chown` (`:11`). There is no stop, restart, or rename. Upstream Schwung stops Move and `kill -9`s MoveOriginal **before** copying (`scripts/install.sh:1099-1109` in the clone).

**Mechanism.** `dlopen` maps the `.so` with `MAP_PRIVATE`. Pages that were never written, which covers all of `.text` and `.rodata`, **are the page-cache pages of that inode**. Linux no longer applies `MAP_DENYWRITE`/`ETXTBSY` to shared libraries; that protection covers only the main exec image, and was removed for libs years ago. scp (legacy or SFTP mode) opens the **existing** file with `O_WRONLY|O_CREAT` (plus `O_TRUNC`, or a trailing `ftruncate`) and rewrites it in place. So:
* Instructions of the running `render_block`, `get_param`, `analyst_thread` and so on change under the CPU. The result is garbage opcodes (SIGILL, which is not caught), wild branches (SIGSEGV with `pc` or `lr` in the concat range), or a silent infinite loop on the SPI thread. That last one is a hard freeze with no crash dump.
* While the file is truncated or shorter, touching a mapped page beyond EOF raises **SIGBUS**.
* `render_block` runs every 2.9 ms, so the crash lands within milliseconds of the copy. That matches a `crash_maps.txt` stamped in the scp minute. **Caveat:** a SIGTERM from a restart also writes `crash_maps.txt` (section 1.5).

**Fix**
```bash
# scripts/install.sh — atomic replace, then restart (never overwrite a live inode)
ssh root@$MOVE_HOST "mkdir -p $DEST/$MODULE_ID"
scp dist/$MODULE_ID/dsp.so      root@$MOVE_HOST:$DEST/$MODULE_ID/dsp.so.new
scp dist/$MODULE_ID/module.json dist/$MODULE_ID/canvas.js root@$MOVE_HOST:$DEST/$MODULE_ID/
ssh root@$MOVE_HOST "cd $DEST/$MODULE_ID && chmod 755 dsp.so.new && chown ableton:users dsp.so.new \
   && mv -f dsp.so.new dsp.so && chown -R ableton:users . \
   && nohup /data/UserData/schwung/restart-move.sh >/dev/null 2>&1 &"
```
`rename()` gives the path a **new inode**. The running process keeps the old unlinked inode mapped and intact, and the next `dlopen` after the restart gets the new code. Always restart, because the host caches the manifest and Concat's code can stay pinned (F1 fix 2).

---

### F4 (HIGH, med confidence): cache validity depends on transient free memory

**Evidence:** `corpus_engine.c:73-74` makes `cache_load` reject when `hd.pool_len > pool_cap_samples`. `pool_cap_samples` comes from `arm_analyst` as `min(pool_mb, MemAvailable−350)` (`concat.c:450-455`), sampled at arm time.

**Scenario.** First boot: 1258 MB available, cap 512 MB, the index is built and cached with `pool_len` ≈ 500 MB. Later, a heavier set, or a re-arm in the same process where the current 500 MB pool already lowers `MemAvailable`, gives cap = 400 MB. That is a **cache miss**, so the same unchanged folder is fully re-analysed and a new cache written. That turns a "fast" reload into minutes, the exact window F1 needs, and it looks random to the user ("sometimes it freezes").

**Fix.** Validate the cache against the **user setting** (`pool_mb`) only, and apply the memory guard as a separate decision. If the cached pool does not fit current memory, either load and decimate slices while reading (skip every Nth slice's audio, keep descriptors), or refuse and show "low memory". Do not silently re-analyse. Also do the memory computation on the analyst at build time (F6).

---

### F5 (HIGH, med confidence): OOM and thrash exposure (candidate for a freeze that never recovers)

**Evidence**
* Default pool 512 MB (`concat.c:118`) against the spec's 128 MB.
* During a rebuild the **old index stays resident** until after the swap (`concat.c:497-501`). The guard at arm time only budgets the new pool.
* Decode transients are unbudgeted. `ingest.c:160-165` caps by **file bytes** (128 MB), but:
  * WAV 16-bit stereo 128 MB means 256 MB f32 interleaved + 128 MB mono + up to about 128 MB resample, **about 500 MB peak** (`ingest.c:170-199`).
  * **MP3** at 128 kbps: a 30 MB file (about 30 min) decodes to about 635 MB f32 stereo + 318 MB mono. A 100 MB MP3 decodes to **about 2 GB or more**. The byte cap does not protect MP3 at all.
* All of this lives inside MoveOriginal's address space. The Move has no swap (1.85 GB total).

**Scenario.** Near-zero `MemAvailable` on a swapless eMMC system. The kernel evicts file-backed text pages (MoveOriginal, shadow_ui, libc) and re-reads them constantly. That produces a long thrash livelock before the OOM killer acts, which feels like a total freeze. If the OOM killer does act, it kills **MoveOriginal**, which holds the biggest RSS, and takes the set down (the risk named in CLAUDE.md).

**Fix**
* Default `pool_mb` 128 (per spec).
* Cap decode by **decoded frames**: query the frame count first (`drwav_init_file` / `drflac_open_file` / `drmp3_get_pcm_frame_count`). Skip files over N minutes, or stream-decode in chunks into the mono buffer. That removes the f32 interleaved copy and the mono copy.
* Budget `reserve = 350 MB + old_pool + 2×largest_decode` at build time.
* Re-read `MemAvailable` periodically during the build and stop adding audio under a floor.

---

### F6 (MED): file I/O and a non-PI lock on the SPI thread in `arm_analyst`

**Evidence**
* `concat.c:439-459` locks `inst->lock` (441), then calls **`mem_available_mb()` → `open/read/close("/proc/meminfo")`** (451, implementation 378-388) while **holding the lock**.
* It is reached on SPI from:
  * `set_param("corpus_file")` (624)
  * `set_param("src_scope")` (630)
  * `set_param("rescan")` (637)
  * state restore via recursion (665), which in turn reaches `corpus_file` and `src_scope`.
* It is also reached from the analyst's auto-pick branch (483). There a **SCHED_OTHER** thread holds the mutex across a syscall. If the SPI thread then enters `set_param(corpus_file)` or `destroy_instance` (541) it blocks on a default (non-PI) mutex, which is a priority inversion.

**Assessment.** `/proc/meminfo` is procfs with no block I/O, typically 20-60 µs. It is not a freeze by itself, but it violates the project rule "never file I/O on SPI" (CLAUDE.md, REALTIME_SAFETY rule 1). The lock-across-I/O on the worker is the real hazard, although it is small.

**Fix.** The SPI side only snapshots params under the lock (plain stores). Move `mem_available_mb()` and the cap computation into `analyst_thread` right before `corpus_index_build`. Never make syscalls while holding `inst->lock`.
```c
static void arm_analyst_locked(concat_t *inst, int force) { /* copies only, no syscalls */ }
/* analyst: long avail = mem_available_mb(); ps = compute_cap(pool_mb_snapshot, avail); */
```
If you want contention-free SPI, replace the mutex with a trylock and a "pending" flag, retried from `render_block`.

---

### F7 (MED): index swap ABA / UAF: pointer-compare flush and a 60 ms sleep

**Evidence**
* The analyst publishes with `__atomic_exchange_n` and then `usleep(60000); corpus_index_free(old)` (`concat.c:500-501`). That is a timing assumption, not a handshake.
* `render_block` flushes grains only when `ix != inst->ge_ix` (929). Grains hold `g->audio = &ix->pool[...]` (`grain.c:102`).
* The chain may **park a silent synth** and skip `render_block`. Concat does not declare `requires_continuous_processing`; `chain_host.c:684,778-785`.

**Scenario.** The slot is parked, for example in Notes mode after release. Grains can still be `active` with `*g->amp == 0`, which renders silence. Then two rebuilds happen, for example a Scope toggle followed by a corpus pick. Index A is freed 60 ms after B is published, B is freed after C. C's `corpus_index_t` (about 6 KB from the malloc heap) can be allocated **at A's old address**. When rendering resumes, `ix == ge_ix` (ABA), so the grains are not flushed and they read A's **munmapped** pool (>128 KB, so mmap-backed). The result is SIGSEGV on the SPI thread and MoveOriginal dies. The same class of hazard applies if render ever holds `ix` for more than 60 ms.

**Fix.** Use a generation number plus a render-acknowledged retire:
```c
/* corpus_index_t: uint32_t gen;  inst: uint32_t next_gen; corpus_index_t *retired; volatile uint32_t seen_gen; */
/* analyst: ni->gen = ++inst->next_gen; old = xchg(&inst->index, ni);
 *          wait until __atomic_load(&inst->seen_gen) == ni->gen || shutdown (poll 5 ms), then free(old).
 *          (If the slot is parked, render never acks: keep `old` in inst->retired and free it on the
 *           next build or in destroy; parked render can't be reading it.) */
/* render_block: if (ix->gen != inst->ge_gen) { grain_flush(); inst->ge_gen = ix->gen; }
 *               __atomic_store_n(&inst->seen_gen, ix->gen, __ATOMIC_RELEASE);   // at END of block */
```

---

### F8 (MED): heavy teardown on the SPI thread

**Evidence:** `concat.c:549-551` runs `analysis_destroy`, then `corpus_index_free(inst->index)`, then `free(inst)` on the SPI thread (called from `chain_host.c:218`). `corpus_index_free` (`corpus_engine.c:140-147`) `free`s a pool of up to 512 MB (an mmap chunk, so `munmap` of up to about 131k touched pages, roughly 10-50 ms) plus `qn`, `slices` and `centroids`.

**Fix.** Let the worker free the index (see the F1 orphan path). In the non-orphan path, have the analyst free `inst->index` just before it exits (it owns the index lifecycle), so `destroy` only joins an already-finished thread.

---

### F9 (MED): a failed build destroys the working corpus

**Evidence:** `concat.c:497-502`: `ni` may be NULL (empty folder, all files over the size cap, OOM, or cancel under the F1 fix). The code still exchanges NULL into `inst->index` and frees `old`. The result is silence plus `progress = 0`.

**Fix:** `if (!ni) { inst->progress = 0; /* keep old */ continue; }`. This is also required once cancellation exists, or cancel would free the live index mid-play.

---

### F10 (MED): knob detents queue expensive rebuilds

**Evidence:**
* `concat.c:634-638`: Rescan fires `arm_analyst(inst, 1)` on **every** non-zero value. The Pattern-3 knob counts up per detent, so turning it 0 to 10 sends 10 arms.
* `concat.c:628-631`: every `src_scope` detent re-arms.
* Arms coalesce only while not yet picked up. Once a build is running, the next arm queues another **full** build. For Rescan that means forced, cache ignored, cache rewritten.

A user who "turns Rescan, then goes to swap the module" hits F1 with a multi-minute build.

**Fix:** edge-detect (fire only on a 0 to non-zero transition, or when the value changes and a build is not already pending for the same path). Coalesce identical requests (same path, params and force). Once cancellation exists, "latest request wins": cancel the in-flight build when a newer arm differs.

---

### F11 (MED, low-med confidence): unbounded recursion inside the non-cancellable build

**Evidence:** `ingest.c:101-114`: `scan_audio` recurses to depth 8 and follows symlinks (`is_dir_entry` stats `DT_LNK`, `ingest.c:44-53`). The file cap only counts **audio files**. A symlink back to an ancestor with b sub-directories fans out to about b^8 `opendir` calls (b=10 means about 10^8), which is effectively forever inside `corpus_index_build`. A join during that is permanent. `ROOTS` already contains a symlinked tree (`breakbeat-samples/Built-in`, per the comment at `ingest.c:48`).

**Fix:** track `(st_dev, st_ino)` of visited directories (a small fixed array) and skip repeats. Cap the total number of directories visited (e.g. 512). Check cancellation per entry.

---

### F12 (LOW): state save and restore

* **Format.** `get_param("state")` (`concat.c:834-848`) emits `key=value\n` for every param except rescan and analyze, in enum order, then `corpus_file=`, then `preset_idx=`. The parser (`concat.c:654-672`) splits on `\n` and the first `=`, and recurses into `set_param`. Paths containing `=` work; paths containing `\n` break (unlikely).
* **Ordering is correct for a fresh instance.** `src_scope` is key #0 and restores while `corpus_file` is empty, so there is no arm (630 guard). `pool_mb` and the slice params restore next, and `corpus_file` comes after them and arms **once** with the right snapshot. If state is re-applied onto a live instance (a user-preset load writes `synth:state`, `shadow_ui_presets.mjs:330`), then `src_scope` arms with the old file and `corpus_file` arms again. That means two arms, which is up to two builds if the analyst picks up the first quickly (F10 coalescing fix).
* `preset_idx` is restored label-only (645-651, clamped). This is correct: `apply_recipe` is not re-run. `preset_name` indexes `RECIPES[inst->preset_idx]` (824). It is always clamped (339-340, 647-648), so it is safe.
* **Truncation:** the parser's `v[256]` (660-662) silently **drops** a `corpus_file` whose value is 256 characters or longer, although `corpus_file[512]`. A long nested path loses its corpus on reload (then F2's auto-pick kicks in). Fix: `v[INGEST_PATH]`.
* **Old states** without `level_norm` or `pos_jitter` keep the defaults 0.5 and 0.15. Old sets therefore change sound (Level Norm reshapes per-grain gain). Decide whether missing keys should restore to a "legacy-neutral" 0 instead: parse a `state_ver=` line, and if it is absent treat the new keys as 0.
* **Return value:** after the loop, the final `snprintf` results are added to `n` even when truncated. The guard at 838 only runs at loop top, so `get_param("state")` can return a value above `buf_len`. That is harmless with the host's 128 KB buffer, but it violates the contract. Clamp the final return: `return n >= buf_len ? buf_len - 1 : n;`.
* **Pretty-printed JSON:** not applicable to Concat. The host passes Concat's opaque string back decoded (section 1.3). If someone hand-edits a set and stores `state` as a JSON **object**, the chain compacts it and Concat's `k=v` parser ignores it (no `=`/`\n` structure), so defaults are kept. That is harmless.
* **Unknown keys** in state (`preset`, future keys) fall through to the named-param loop and are ignored. Good.

### F13 (LOW): NULL deref on the analyst kills the process

`corpus.c:170-173`: `col = malloc(...)` is used unchecked in `concat_robust_fit`. Under memory pressure (F5) this segfaults on the analyst, and a SIGSEGV on any thread kills MoveOriginal. Also check the `corpus_kmeans` and `cache_load` paths. They do check. Fix: `if (!col) { fill median=0, iqr=1; return; }` and propagate failure.

### F14 (LOW): `create_instance` cost and thread creation

* `concat.c:510-535` does `calloc` (about 40 KB), `grain_engine_init` → `build_env_tables` (8×1024 transcendentals, once per process, `grain.c:66-74`), `analysis_create` (about 100 KB, mel filterbank and DCT with `cos`/`log`, `analysis.c:~52-80`) and `pthread_create`. That totals about 0.3-1 ms on SPI: over the 2.4 ms slack only in combination with the host's own load work, and acceptable for a load path.
* The new thread inherits **SCHED_FIFO 70 and the caller's affinity** (default `PTHREAD_INHERIT_SCHED`) until its first two lines demote it (`concat.c:463-466`). That is fine. For zero FIFO residency, create it with an attr of `PTHREAD_EXPLICIT_SCHED` plus `SCHED_OTHER` and a cpuset of 0-2.
* If `pthread_create` fails, `analyst_started=0`, analysis never runs (silent), and destroy skips the join and the mutex destroy (a trivial leak of init'd primitives). Consider surfacing it through `get_error` (currently NULL).
* NULL safety: `render_block` guards `!ix || nslices==0` (925). `get_param` handlers guard `ix` (792-819). `map_live` guards `nslices<=0` and `cap==0` (797, 806). `build_map_pts` guards NULL and `nslices<=0` (710-712). Pad clusters guard `kclusters>0` (1049). Mosaic guards `mono_analysis`. No NULL deref found on SPI paths.

### F15 (LOW): other SPI-side costs (bounded, fine)

* `chain_params` (`concat.c:736-775`): about 150 `snprintf`, about 5 KB, roughly 50-150 µs. The host reads it at load (`chain_host.c:630-636`) and on plan, not per frame. OK.
* Named `get_param` and `set_param`: up to about 65 `strcmp` per call, sub-10 µs. OK. A static sorted key table plus `bsearch` would help if the host polls many keys per frame.
* `apply_recipe` (338-363): at most about 20 keys × 50 `strcmp` per jog detent, about 10-20 µs. No alloc. OK.
* `map_live` (795-814): 8 × `map_project` (72 MACs) plus `snprintf`. OK. `map_pts` is a 4 KB copy. OK.
* `get_param("state")`: about 52 `snprintf`s, about 30 µs. OK.
* Nothing in `get_param` or `set_param` allocates. `render_block` does no alloc and no I/O. `compute_mosaic_target` runs an FFT and MFCC per 1024 input samples on SPI, bounded (that is a DSP-review item).

---

## 3. Most likely cause of the module-switch freeze

### H1: SPI thread blocked in `pthread_join` on a running analysis (F1, amplified by F2, F4, F10). Probability about 60%

**Chain of events:**
1. The user deploys, and Move restarts (or crashes and restarts via F3).
2. The slot restores Concat. `create_instance` runs with NULL config, so auto-pick fires. It races `set_param("state")` (F2), producing one or two builds. The state-restored arm may also miss the cache because the cap is computed from current `MemAvailable` (F4). That can mean a full re-analysis plus up to 512 MB of cache writes.
3. The user opens the slot and picks another module. `synth:module` leads to `v2_unload_synth`, then Concat `destroy_instance`, then `pthread_join` on the SPI thread (FIFO 70). Everything stops until `corpus_index_build` returns: seconds on a hit, **minutes** on a miss or folder scope.
4. It looks exactly like a hard freeze. The user power-cycles before it would have recovered.

**Confirm on device** (sshd stays alive while the SPI thread is blocked):
* Before swapping, read `synth:_dbg` or `synth:analyze`. `prog<100` or `arm=1` means a build is in flight. Also check `/data/UserData/schwung/concat-cache/` mtimes and sizes changing during the build.
* During the freeze:
  ```sh
  P=$(pidof MoveOriginal)
  for t in /proc/$P/task/*; do echo "$t $(cat $t/comm) $(awk '{print $3}' $t/stat) $(cat $t/wchan)"; done
  cat /proc/$P/task/<spi_tid>/stack      # expect futex_wait_queue_me (pthread_join)
  cat /proc/$P/task/<analyst_tid>/stack  # expect R or blkdev/ext4 read/write
  cat /proc/$P/task/<analyst_tid>/io     # read_bytes/write_bytes climbing
  ```
  An SPI thread in `futex_wait` plus a busy analyst thread confirms H1. Then wait: if audio returns once the analyst finishes, that is conclusive.
* After recovery, the unified log should carry a param-slow line for key `synth:module` with a duration in seconds (`src/host/shim_worker.c:1593-1620`), and `[spi_timing] param max` in seconds.
* Reproduce deliberately: set Scope=Folder on a big folder, press Rescan, and swap the module immediately. It should freeze for about the analysis time. Then with Rescan finished (`analyze=100`), swap again: it should be instant.

### H2: memory exhaustion thrash livelock (F5). Probability about 15%

A 512 MB pool plus decode transients (especially MP3) plus the old pool during a rebuild, with no swap. The freeze would then be system-wide (SSH also sluggish), and it can start slightly before the click, or be triggered by the new module's allocations.

**Confirm:** over SSH, `free -m`, `vmstat 1` (huge `bi` with swap 0 means page-cache thrash), `cat /proc/pressure/memory` if PSI is available, `dmesg | grep -i -E "oom|killed process"` after reboot (persisted kernel log if any), and `VmRSS` of MoveOriginal while a build runs. It is likely if `MemAvailable` is under about 100 MB at the time.

### H3: stale or corrupted code from the in-place scp (F3). Probability about 15%

If MoveOriginal **survived** the copy (the Concat slot parked, so no `render_block`, and no param traffic), the first calls into the overwritten `.text` happen when the user clicks the slot or swaps. `get_param`/`destroy_instance` then execute foreign bytes: SIGSEGV (crash plus restart) or a tight loop on the SPI thread (a true permanent freeze, no crash dump). The woken analyst also returns into rewritten code. This fits "freeze right after the latest deploy" well **only if no restart happened between install and the click**.

**Confirm:**
* In the unified log, check whether the install-minute entry is `Caught SIGSEGV` or `Caught SIGBUS` with `pc`/`lr`, or `Caught SIGTERM` (a restart).
* `grep concat /data/UserData/schwung/crash_maps.txt`. Check whether `lr` or `pc` falls inside `.../concat/dsp.so`, then compute `lr - base` and run `addr2line -e <the OLD dsp.so build>`.
* Compare the MoveOriginal PID or start time (`ps -o lstart`) with the install time.
* Going forward, the atomic `mv` deploy removes this class entirely.

### H4: not Concat, the incoming module's load, or `_dl_load_lock` inversion. Probability about 10%

`v2_load_synth(new)` runs the new module's `create_instance` on SPI (673 ms measured for minijv). `dlopen` there can also wait on `_dl_load_lock` held by the chain bus worker (`chain_host.c:157-168`, `REALTIME_SAFETY.md:80-87`).

**Confirm:** swap from a **different** module to the same target. If it also freezes, H4. The param-slow log names the key but not the phase, so check `/proc/<pid>/task/<spi>/stack` for `futex` inside `ld-linux` (`_dl_load_lock`) versus `pthread_join`.

(F7's ABA UAF produces a **crash**, not a freeze, and is unlikely right at a swap.)

---

## 4. Recommended fix order

1. **Deploy safety (F3):** atomic `mv` plus restart in `install.sh`. This is a one-line class fix and removes H3.
2. **F1:** a cancellable `corpus_index_build`, plus `pthread_timedjoin_np` with an orphaned, self-freeing worker, plus `RTLD_NODELETE` pinning. Free big buffers off SPI (F8). Keep the old index on a failed or cancelled build (F9).
3. **F2:** the generation-guarded, SPI-deferred auto-pick, with all `corpus_file`/`corpus_path` writes under the lock.
4. **F6 and F4:** move the memory computation to the analyst, stop holding the lock across syscalls, and validate the cache against `pool_mb`, not `MemAvailable`.
5. **F5:** default `pool_mb` 128, a decoded-length cap (especially MP3), budgeting of old pool plus transients.
6. **F10, F11, F7, F12, F13:** edge-detected Rescan and coalescing, a dir-inode visited set, generation-based grain flush and retire, `v[512]` in the state parser, a clamped state return, a checked `malloc`.
