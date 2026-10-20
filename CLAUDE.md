# Concat — Claude Code context

## What this is
A **corpus / concatenative-synthesis instrument** for Ableton Move: analyse a *folder* of audio,
slice + describe every unit, hold retained slices in a RAM pool, and play back the slices nearest
a moving point in descriptor space. Fills the catalog's real gap — five granular modules, all
read one buffer; nothing navigates a descriptor space across a sample *folder*.

Plugin type: **sound_generator**
Module ID: `concat`  ·  Abbrev: `CONCAT`
API: `plugin_api_v2_t` (entry `move_plugin_init_v2`, DSP file **`dsp.so`**)
Language: C

Read `design-spec.md` for full design intent. This file is the compressed, imperative version.

---

## v1 decisions (locked 2026-09-01)
- **Both play models in ONE sound_generator** (not a sound_generator + an audio_fx): a sound
  generator can read audio input via `host->mapped_memory + host->audio_in_offset` (line/mic SPI
  mailbox), so the mosaicker's target is the **live line/mic input**. No second module.
- **Fixed pool at load**, implemented as a bounded arena internally so incremental/live growth is
  a v2 increment. Mosaic-from-Move-track-audio (`pfx_track_shm`) is v2.
- **Point-anywhere corpus:** primary control is a **File Browser** (`filepath_browser.mjs`, rooted
  `/data/UserData`) → `set_param("corpus_path", <path>)`; `corpora/`+`recordings/` scan is a
  secondary quick-pick enum. Both are menu items, not knobs. `corpus_path` persists in state.
- **Played by ear (v1) — NO overtake module.** One `sound_generator` + `ui_chain.js` menus.
  Pads → **k-means cluster** jumps (default K=16, trigger-only; no pad LEDs in shadow mode). OLED
  descriptor-map scatter = v2 overtake.
- **Ingest:** decode WAV/FLAC/MP3 via **dr_libs** (single-header, GLIBC-safe); loader normalises
  → mono / 44.1 kHz (offline cubic resample) / int16; silence-gate low-loudness slices.
- **Descriptors (~18–20d):** loudness, centroid, flatness, YIN pitch+conf, ZCR, MFCC1–13. Query
  distance = per-dim **z-score** (per-corpus std); nav X/Y = per-corpus **min-max→0–1**.
- **Slicing:** onset (spectral flux) clamp **min 60 / max 400 ms**, ~20 ms floor; fixed-grain
  fallback for drones. **Grains:** 24-voice steal-oldest, density **grains/s** (1–100, def 20),
  size 10–500 ms. **Index cache** under module dir, keyed by hash(path)+mtimes (rescan = changed only).

## Sonic intent
CataRT / MuBu descriptor-space concatenative synthesis; audio mosaicking (beatbox → corpus);
timbre-map granular. The corpus IS the instrument — travel a timbre map across many files, not
time within one. NOT a one-shot sampler, time-stretch, single-buffer granular, or an
analysis-only display.

---

## DSP architecture (three threads — Mello async-loader pattern)
1. **Render** (`render_block`, 128f @ 44.1k): schedule/play grains, run the nearest-neighbour
   query, and in Mosaic mode read line/mic in and extract target descriptors. Resident memory
   ONLY — no disk, no alloc.
2. **Analyst** (`SCHED_OTHER`, cores 0–2): walk folder → stream files → slice → describe → write
   on-disk index. Publishes an **atomic progress counter** `get_param` reads without scanning.
3. **Loader** (`SCHED_OTHER`): build mono-int16 pool → **atomic pointer swap** (Mello
   `active_bank`). `create_instance`/`set_param` return instantly; folder-select only ARMS them.

- **Query:** linear scan over ~N×20 floats in `render_block` (<100 µs at 4000 slices). **No
  kd-tree** (degrades past ~10 dims). Selection = weighted-Euclidean kNN to target, random among K
  within `radius`, **repetition avoidance** = exclude last `repeat_avoid` played slices (CataRT
  "forbidding"). Concatenation-cost = v2.
- **FFT:** in-house iterative radix-2 (`analysis.c`), 1024-pt, Hann, hop 512. (Planned as kissfft; replaced before 0.1.0.)
- **v2 headline:** UMAP/PCA learned 2-D map projection (FluCoMa) replacing raw-descriptor nav axes —
  precomputed at analysis, stored in index → nav-layer swap, not an engine change.
- **Clean-room DSP reference:** `references/dsp-notes.md` (MFCC=MelBands→DCT-II, centroid/flatness
  from SpectralShape, YINFFT, spectral-flux onset, CataRT scheduling). Sources = FluCoMa (BSD-3,
  studied not vendored) + Schwarz CataRT papers. Reimplement clean; cite in commits.
- **Descriptors (~36 dims, per-slice MultiStats — FluCoMa-derived):** per-frame MFCC 1–13 +
  centroid + flatness + ZCR (1024-pt FFT, hop 512, 40 mel → log → DCT-II) summarised per slice by
  **mean + std** (std captures morphology); + loudness (mean/peak) + YIN pitch+conf (median voiced).
  Normalise: query = per-dim **RobustScaling** (median/IQR); nav axes = per-corpus min-max→0–1.
  ~40–80× realtime on CM4. **FFT = in-house radix-2** (never FFTW — GPL/NC conflict).
- **Slicing:** onset (spectral flux) + min/max clamps; fixed-grain fallback.
- **Voices:** polyphonic grain cloud, target ~16–24 grains, mono corpus → stereo with pan spread.
- **Grain-lock** window-phase params (grain_size, env) at grain start (avoid clicks).

## Memory / OOM (dominant risk — CM4 1.85 GB measured, NO swap, rootfs full)
- Measured 2026-09-01: MemTotal 1894 MB, MemAvailable 1258 MB, Swap 0, MoveOriginal 489 MB (light set).
- `pool_mb` default **128 MB** (~25 min mono), max **512 MB**. Guard at each corpus load:
  `effective_cap = min(pool_mb, MemAvailable_now − 350 MB reserve)`, floor 32 MB. Read
  `/proc/meminfo` via `open()` + parse (never `shm_open`/cached). Beyond the cap, **evict
  least-used slices' audio** (keep the index) rather than failing — audible corpus becomes a
  shifting subset.
- Everything under `/data/UserData/` — corpus, index. **Never `/tmp`.**
- If the pool pushes the system OOM, the killer may take MoveOriginal down mid-set. Design around it.

---

## Parameters
Sound-generator knob pages (≤8/page, switch via `set_param("_level", <page>)`). Full table with
ranges/defaults + open questions in `design-spec.md`. Pages: **Corpus** (folder, pool_mb, rescan,
slice_mode, min/max_slice_ms, analyze_progress) · **Map** (x_desc, y_desc, x_pos, y_pos, radius,
k_nearest, follow) · **Grain** (density, grain_size, grain_env, pitch, pitch_jitter, pan_spread,
reverse) · **Play/Out** (play_mode Map/Notes/Mosaic, input_src, mosaic_rate, note_match, dry_mix,
volume, width, sat).

- `corpus_path` (string) set from the File Browser is the **primary** corpus control; `quick_pick`
  is a **dynamic enum** served from DSP `get_param` (freshly-scanned `corpora/`+`recordings/` every
  call — Mello `bank_a` pattern; never return -1 for it → falls back to stale module.json). Both
  are ui_hierarchy menu items, not knobs.
- `rescan`/action knobs = wide-int Pattern 3 (`type:int, min:0, max:127`); fire on `atoi(val)!=0`.
  Display counts up (host doesn't re-poll get_param) — document in README.
- Expose `x_pos, y_pos, radius, density, pitch` as prime **host-LFO** targets.

## Modulation
Full MIDI: note (Notes-mode pitch-match; vel→gain/density), bend (±transpose grains), CC1
(→radius/density), aftertouch (→density/jitter), CC123 (release all). Pads (68–99) in Map mode =
map regions → set target (trigger only; **no pad-LED feedback from DSP in shadow mode**). MIDI
clock reaches `on_midi` as len==1 for `follow=Sequence` / tempo-synced `mosaic_rate`.

---

## Open questions (resolve before implementing the relevant section)
- Final descriptor set + per-corpus vs global normalisation.
- Slicing method + default min/max ms.
- Corpus root: `/data/UserData/schwung/corpora/<folder>/` vs File Browser pick; ingest → mono 44.1k int16.
- Grain polyphony ceiling + density units.
- Live descriptor-map OLED view vs overtake UI later; pad→region clustering (k-means?).
- `pool_mb` default vs measured `MemAvailable`.
- v2: live-arena growth; mosaic from Move-track audio.

---

## Move hardware constraints (never violate)
- 128 frames @ 44100 Hz (~2.9 ms); audio int16 stereo interleaved.
- **No heap alloc / no printf in `render_block`.** Pool/index alloc on worker threads only.
- `-ffast-math` (no FTZ on ARM); denormal-guard feedback paths.
- `create_instance` must do almost nothing (module load blocks SPI ~673 ms) — ARM workers, don't scan/alloc.
- `get_param` serves a cached snapshot — never rescan folder per repaint.
- Workers: `SCHED_OTHER` + `taskset` cores 0–2 (core 3 = SPI FIFO 90).
- **GLIBC:** the Move runs a glibc new enough for the 2.33/2.34 symbols the Debian 12 toolchain emits (stat, pthread_*, dlopen); 0.1.0 already needed them. Still: `open("/dev/shm/...")` not `shm_open`; no `-lrt`; no `-Ofast` (libmvec).
- Files on device owned by `ableton:users`. Never write `/tmp`.

## API-specific constraints (sound_generator / plugin_api_v2)
- Struct has **8 fields** — `get_error` REQUIRED between `get_param` and `render_block` (set NULL
  if unused) or `render_block` reads wrong offset → SIGSEGV.
- `get_param` returns **`snprintf(...)` byte count**, and **-1 for unknown keys** (never 0).
- Return **`ui_hierarchy` from `get_param`** (module.json alone is not enough → "No presets").
- `"component_type":"sound_generator"` and `"raw_midi":true` at **root** of module.json (not only
  in capabilities). `"dsp":"dsp.so"`, `"audio_out":true`,`"midi_in":true` in capabilities.
- Persist all params via `get_param("state")` / restore from `config_json` in `create_instance`.
- chain_params: MUST be DSP-served (return the JSON from `get_param`). The module.json
  (`return -1`) fallback does NOT render the filepath Source picker on device. Schwung >= 1.3.0
  raised `SHADOW_PARAM_VALUE_LEN` to **64 KB** (was ~4 KB), so full labels + step fit with room
  to spare — Concat now serves ~4.9 KB and REQUIRES the Move to be on Schwung 1.3.0+.
- v1.3.0 also adds `visible_if` (level/param conditional visibility), `capabilities.claims_ccs`
  / `claims_edit_ccs` (grab CCs + Undo/Copy/Delete gestures), and more-consistent canvas UI.
- Async asset load = Mello pattern (pthread worker + `atomic_exchange` pointer swap).

---

## Repo map (v0.2.0)
- `src/dsp/concat.c` — params, analyst thread (cancellable, bounded teardown, status file), MIDI,
  render, play modes, JSON state. `corpus_engine.c` (build, caches, k-means, PCA), `corpus_file.c`
  + `json.c` (.concat), `ingest.c` (streamed dr_libs decode, sorted listings), `query.c`, `grain.c`,
  `analysis.c`, `corpus.c`.
- `src/builder/` — Corpus Builder web page (served by schwung-manager); `cb.js` = WASM of the
  analysis code, rebuilt with `scripts/build_wasm.sh`. `src/web_ui.html` — Remote UI panel.
- `docs/corpus-builder-design.md` — corpus file + builder design and verified manager API facts.
- `src/module.json` — version must match git tag
- `scripts/build.sh` — Docker ARM64 cross-compile (always use this; never ad-hoc aarch64)
- `scripts/install.sh` — `scp -r dist/concat root@move.local:/data/UserData/schwung/modules/sound_generators/` + fix ownership + `chmod +x dsp.so`
- `tests/` — test suite

## Pipeline
Scaffold: `/scaffold-sound-generator concat`. Then `/move` orchestrates code → `/dsp-review` +
`/move-review` → build/deploy/test → `/move-schwung-release`.
Reference synths: **Mello** (async WAV bank loader — closest to Concat's corpus loader), Octocosme
(voice arch), Serge/Aphex (mod + params).
