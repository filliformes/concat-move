# Concat — Design Spec

> **Status:** Original design (2026-09-01), kept for the reasoning behind it. What shipped is in [docs/MANUAL.md](docs/MANUAL.md) (v0.2.0); some details here changed on the way (for example kissfft became an in-house FFT, and the default Pool Cap and slice limits moved).
> **Plugin type:** sound_generator (`plugin_api_v2_t`)
> **Module ID:** `concat`
> **Abbrev:** `CONCAT`
> **Last updated:** 2026-09-01

---

## What it is

Concat is a **corpus / concatenative-synthesis instrument** for Ableton Move. It analyses a
*folder* of audio (not a single buffer), slices every file into short units, describes each
slice with a handful of audio descriptors (loudness, spectral centroid, spectral flatness,
YIN pitch, MFCCs), and holds the retained slices in a RAM pool as mono int16. At play time it
navigates that descriptor space and plays back the slices nearest a moving target point —
granular playback stitched from a whole sample library rather than one sample.

It is played **two ways in v1** (the same corpus, the same analyst, one module):

1. **Corpus instrument** — you drive the target point through the descriptor space with the
   encoders (an X/Y descriptor pad mapped to two knobs), a radius/jitter knob sprays selection
   around it, pads jump to regions of the map, and incoming MIDI notes pitch-match to the
   nearest slice.
2. **Audio mosaicker** — the target descriptors are extracted in real time from the **line/mic
   input** and the query *follows the live input*: sing / beatbox / feed a source and the
   corpus is re-assembled to match it, onset by onset.

The concatenative gap it fills: the catalog has five granular modules and they all read **one**
buffer. Nothing navigates a descriptor space across a sample *folder*. Concat is that, and the
analysis engine it contains (onset + descriptors + machine listening) is the shared foundation
for the later cross-slot / listening-to-control / OSC direction.

## Sonic intent

**References:** CataRT / MuBu (IRCAM) descriptor-space concatenative synthesis, AudioGuide,
audio mosaicking (beatbox → drum corpus), granular clouds navigated by timbre rather than by
time. The Move's own sample-library-as-instrument ethos.
**Philosophy:** the corpus is the instrument. You don't scrub *time* in one file, you travel a
*timbre map* across many. The same engine listens (mosaic mode) and is played (map mode).
**Not:** a one-shot sampler, a time-stretch tool, a single-buffer granular (those exist —
Granny, Boris, Slicer). Not a pitch-tracker display or a key detector (those analyse only to
show/route). Concat analyses to *build a playable space*.

---

## DSP architecture

**Core algorithm:** offline corpus analysis (streaming from disk) → descriptor index → live
nearest-neighbour query (linear scan) → polyphonic grain playback from a resident RAM pool.

**Voice architecture:** polyphonic grain cloud (target ~16–24 concurrent grains; final count is
a CPU/tuning decision). Mono int16 corpus, stereo out with per-grain pan spread.

**Three threads (Mello async-loader pattern, proven in production):**

1. **SPI/render thread** (`render_block`, 128 frames @ 44.1 kHz) — schedule + play grains, run
   the query, and in mosaic mode read `host->mapped_memory + host->audio_in_offset` (line/mic)
   and extract the target descriptors. Touches **only resident memory**. No disk, no alloc.
2. **Analyst** (`SCHED_OTHER`, affinity to cores 0–2) — walk the folder, stream each file, slice,
   describe, write the on-disk index beside the corpus. Publishes an **atomic progress counter**
   that `get_param` reads without scanning. Never runs on the SPI thread.
3. **Loader** (`SCHED_OTHER`) — build the mono-int16 RAM pool, **atomic pointer swap** on
   completion (exactly Mello's `active_bank` swap). `create_instance` and `set_param` return
   instantly; folder-select only *arms* the analyst/loader.

**Memory model (v1): fixed pool at load.** The corpus is analysed and fixed when a folder is
selected. Implemented internally as a **bounded arena** with a user-visible hard MB cap so that
live/incremental growth (slices appearing while you play) is a small v2 increment, not a rewrite.
OOM is the dominant risk on the CM4 (2 GB total, no swap, rootfs full): read `MemAvailable` at
load, refuse to exceed a fraction of it, and **evict least-used slices' audio** rather than
failing the load — the corpus may exceed the pool, and which slices are audible becomes a
shifting, curated subset.

**FFT:** descriptors need an FFT (1024-pt, Hann, hop 512) — **kissfft (BSD-3), locked** (not FFTW,
whose GPL is incompatible with CC BY-NC-SA's NC clause; not HISSTools unless profiling later demands
NEON throughput). Analyst uses it offline; Mosaic mode also uses it on the render thread.

**Selection (CataRT-derived):** weighted-Euclidean **kNN** to the target point, pick randomly among
K within `radius`; **repetition avoidance** via a ring buffer of the last `repeat_avoid` played
slices (excluded from candidates). Concatenation-cost term = v2. See `references/dsp-notes.md`.

**Query:** linear scan over ~N slices × ~20 float descriptors. At 4000 slices this is ~320 KB of
sequential memory, well under 100 µs — cheaper and simpler than a kd-tree (which degrades past
~10 dims). Runs in `render_block`. **No kd-tree in v1.**

**Descriptors — per-slice MultiStats summary (~34–38 dims, FluCoMa-derived).** A slice is NOT one
descriptor vector: per-frame descriptors are computed across the slice (1024-pt FFT, hop 512) and
**summarised per slice by mean + std** (FluCoMa `MultiStats`; std captures morphology — what makes
matches musical). Per-frame descriptors: 13 MFCC (40-band mel → log → DCT-II), spectral centroid,
spectral flatness, ZCR → mean+std = 32 dims; plus loudness (mean/peak) and YIN pitch + confidence
(median over voiced frames) ≈ 36 dims total. Still a trivial linear-scan query. Benchmarked
~40–80× realtime on the CM4 (Cortex-A72, scalar, no NEON) — 1 h of source ≈ 45–90 s.

**Slicing (proposed v1):** onset-based slices with min/max duration clamps; fixed-grain fallback
mode. Onset detection is cheap (spectral flux). Final method + defaults are a tuning question.

**Known challenges:**
- Growable pool under a live reader (deferred to v2 by choosing fixed-pool for v1).
- OOM avoidance without swap — hard cap + `MemAvailable` check + eviction.
- Real-time target analysis in mosaic mode (streaming FFT on the render thread every N blocks)
  roughly doubles the live DSP vs. map mode — the main "Both in v1" cost.
- No pad-LED feedback from DSP in shadow mode → the descriptor-map visualization must live on the
  OLED (or an overtake UI), not on lit pads.
- Corpus audio must be mono 44.1 kHz int16 in the pool; loader resamples/downmixes on ingest.

---

## Parameters

> Organised as sound-generator knob pages (≤8 knobs/page, page-switched via `set_param("_level", ...)`).
> Ranges/defaults left blank are **open questions**, tunable at implementation. Page split is
> provisional.

### Page: Corpus
| Name | Key | Type | Range/Options | Default | Notes |
|------|-----|------|---------------|---------|-------|
| Corpus path | `corpus_path` | string (menu) | any folder | — | **Primary.** File Browser (`filepath_browser.mjs`, rooted `/data/UserData`) picks a folder → arms analyst+loader; persisted in state. Menu item, not a knob |
| Quick pick | `quick_pick` | enum (dynamic, menu) | `corpora/*` + `recordings/*` | — | Secondary. Dynamic `get_param` scan (Mello `bank_a` pattern); sets `corpus_path`. Menu item, not a knob |
| Pool cap | `pool_mb` | int | 16–512 MB | 128 | Hard RAM ceiling; guarded vs `MemAvailable` at load; eviction beyond it |
| Rescan | `rescan` | int (action) | 0–127 | 0 | Re-analyse folder (LESSON D Pattern 3 wide-int) |
| Slice mode | `slice_mode` | enum | Onset / Fixed | Onset | |
| Min slice | `min_slice_ms` | float | 20–500 ms | 60 | Onset mode clamp |
| Max slice | `max_slice_ms` | float | 100–2000 ms | 400 | Onset mode clamp / fixed grain length |
| Analyze | `analyze_progress` | read-only | 0–100% | — | Served from atomic counter; never scans |
| — | | | | | (1 free slot) |

### Page: Map
| Name | Key | Type | Range/Options | Default | Notes |
|------|-----|------|---------------|---------|-------|
| X descriptor | `x_desc` | enum | Bright / Pitch / Loud / Noisy / MFCC1… | Bright | Which descriptor is the X axis |
| Y descriptor | `y_desc` | enum | Bright / Pitch / Loud / Noisy / MFCC1… | Pitch | Which descriptor is the Y axis |
| X position | `x_pos` | float | 0–1 | 0.5 | Target point X (encoder) |
| Y position | `y_pos` | float | 0–1 | 0.5 | Target point Y (encoder) |
| Radius | `radius` | float | 0–1 | 0.15 | Selection spread around target |
| K nearest | `k_nearest` | int | 1–16 | 4 | Randomly pick among K nearest matches |
| Follow | `follow` | enum | Manual / LFO / Sequence | Manual | What drives x_pos/y_pos (host LFO can also target these) |
| Repeat avoid | `repeat_avoid` | int | 0–16 | 4 | Ring buffer of recently-played slices excluded from kNN (CataRT "forbidding") — prevents stuck-grain |

### Page: Grain
| Name | Key | Type | Range/Options | Default | Notes |
|------|-----|------|---------------|---------|-------|
| Density | `density` | float | 1–100 grains/s | 20 | Grain scheduling rate |
| Grain size | `grain_size` | float | 10–500 ms | 120 | Playback grain length (may differ from slice) — grain-locked |
| Envelope | `grain_env` | enum | Hann / Tukey / Tri … | Hann | Grain window shape |
| Pitch | `pitch` | float | semitones (±24) | 0 | Transpose of played grains |
| Pitch jitter | `pitch_jitter` | float | 0–1 | 0 | Per-grain pitch spray |
| Pan spread | `pan_spread` | float | 0–1 | 0.3 | Stereo spread across grains |
| Reverse | `reverse` | float | 0–1 (prob) | 0 | Probability a grain plays reversed |
| — | | | | | (1 free slot) |

### Page: Play / Out
| Name | Key | Type | Range/Options | Default | Notes |
|------|-----|------|---------------|---------|-------|
| Play mode | `play_mode` | enum | Map / Notes / Mosaic | Map | Map=encoders, Notes=MIDI pitch-match, Mosaic=line-in target |
| Input source | `input_src` | enum | Line / Mic (/ Track v2) | Line | Mosaic target source (SPI mailbox line/mic in) |
| Mosaic rate | `mosaic_rate` | enum | Onset / 1/16 / 1/8 … | Onset | How often to re-query from live input |
| Note match | `note_match` | enum | Off / Pitch / Zone | Pitch | MIDI note → nearest-by-pitch slice, or note→map zone |
| Dry mix | `dry_mix` | float | 0–1 | 0 | Blend of live input passthrough (mosaic mode) |
| Volume | `volume` | float | 0–1 | 0.8 | Master output |
| Width | `width` | float | 0–1 | 1.0 | Stereo width |
| Saturation | `sat` | float | 0–1 | 0 | Output soft-sat / glue |

---

## Modulation

- Full MIDI (sound_generator): note on/off (pitch-match in Notes mode; velocity → grain
  gain/density), pitch bend (± transpose active grains), CC1 mod wheel (→ radius or density),
  aftertouch (→ density or jitter), CC123 panic (release all grains).
- Host LFOs (framework, 2 per slot) can target any `chain_params` key — expose `x_pos`, `y_pos`,
  `radius`, `density`, `pitch` as prime LFO destinations so the map can wander under an LFO.
- Pads (notes 68–99): in Map mode, each pad = a region of the descriptor map (e.g. k-means
  cluster) → sets the target point. Functional trigger only — **no pad-LED map feedback** from
  DSP in shadow mode.
- MIDI clock (0xF8/0xFA/0xFB/0xFC) reaches the synth's `on_midi` as len==1 (LESSON J addendum) —
  usable for `follow=Sequence` and tempo-synced `mosaic_rate`.

---

## Resolved design decisions (session 2, 2026-09-01)

- **File formats / ingest:** decode WAV + FLAC + MP3 via **dr_libs** (`dr_wav`/`dr_flac`/`dr_mp3`,
  public-domain single-header, GLIBC-2.29-safe; AIFF deferred). Loader normalises every file →
  mono downmix → 44.1 kHz (offline cubic resample) → int16 into the pool; analysis runs post-
  normalisation. Silence-gate slices below a loudness floor.
- **Descriptors (~36 dims, per-slice MultiStats):** per-frame MFCC 1–13, centroid, flatness, ZCR →
  **mean + std** per slice (FluCoMa `MultiStats`; std captures a slice's morphology); + loudness
  (mean/peak) + YIN pitch+confidence (median over voiced frames).
- **Normalisation (two-tier, FluCoMa-derived):** query distance uses per-dimension **RobustScaling**
  (median/IQR — outlier-robust, beats plain z-score on real corpora) so no descriptor dominates;
  MFCCs weighted for mosaic timbre match. Navigation axes use **per-corpus min-max → 0–1** so the
  encoder always spans the corpus. Per-corpus, not global. Stats stored in the index.
- **Slicing:** onset (spectral flux, adaptive) clamped **min 60 ms / max 400 ms**, stable regions
  chunked at max; **fixed-grain fallback** (grain=max, 50% hop) for drones/textures; ~20 ms floor.
- **Grain engine:** 24-voice pool, steal-oldest; **density in grains/s** (1–100, default 20);
  grain size 10–500 ms independent of slice length.
- **Index cache:** under the module's writable dir, keyed by `hash(folder path)` + file-mtime
  manifest → rescan only re-analyses changed files. Also scan `schwung/recordings/` as corpora.
- **Mosaic target:** line/mic (v1) via SPI mailbox `audio_in_offset`; Move-track audio via
  `pfx_track_shm` = v2.
- **Corpus mental model → point-anywhere.** Primary = File Browser at any folder (`corpus_path`
  string, persisted); `corpora/`+`recordings/` scan is a secondary quick-pick enum. Both are menu
  items, not knobs.
- **Visual map → played by ear (v1).** No overtake module: one `sound_generator` with `ui_chain.js`
  menus, encoder navigation, and pads→**k-means cluster** jumps (default **K=16**, trigger-only —
  no pad LEDs in shadow mode). OLED descriptor scatter + cursor + lit pad regions = v2 overtake.

- **Pool sizing (measured on-device 2026-09-01):** MemTotal 1894 MB, MemAvailable **1258 MB**,
  **SwapTotal 0**, baseline MoveOriginal RSS 489 MB (light set — a sample-heavy set raises it).
  → default `pool_mb` **128 MB** (~25 min mono), max **512 MB** (~100 min). Runtime guard at load:
  `effective_cap = min(pool_mb, MemAvailable_now − 350 MB reserve)`, floored 32 MB; evict beyond.
  Re-read `/proc/meminfo` (`open()`, parse MemAvailable) at each corpus load — don't trust a cached number.

## Open questions (remaining)

- [ ] Pad→region cluster count final (K=16 default) and re-cluster trigger (on load / on rescan).
- [ ] **v2 headline — learned 2-D map:** replace raw-descriptor nav axes with a **UMAP/PCA**
      projection of the full descriptor space (FluCoMa `UMAP`/`PCA`/`MDS`) — the world-class,
      most-playable "map." v1 keeps raw axes (`x_desc`/`y_desc`); the projection is precomputed at
      analysis and stored in the index, so it's a nav-layer swap, not an engine change.
- [ ] Fixed-pool → incremental/live-arena growth: v2.
- [ ] Move-track audio as mosaic target (`pfx_track_shm`): v2.
- [ ] Concatenation-cost term (seam continuity between consecutive grains): v2.

---

## Hardware constraints (Move / CM4)

- Block size: 128 frames at 44100 Hz (~2.9 ms).
- Audio format: int16 stereo interleaved (out); int16 stereo in at `mapped_memory + audio_in_offset`.
- No FTZ on ARM — compile `-ffast-math`; denormal-guard feedback paths.
- **No heap allocation in `render_block`** — pool + index allocated on the loader/analyst threads only.
- No `printf`/logging in the render path.
- **`create_instance` must do nearly nothing** (module load already blocks SPI ~673 ms) — no scan, no pool alloc; arm the worker threads instead.
- **`get_param` serves a cached snapshot** — never rescan the folder per repaint.
- **Never write `/tmp`** (rootfs full). Corpus, index, everything under `/data/UserData/` (~49 GB).
- Worker threads: demote to `SCHED_OTHER`, `taskset` cores 0–2 (core 3 is SPI at FIFO 90).
- 1.85 GB RAM total (measured), **no swap** — hard pool cap + `MemAvailable` check + eviction, or the OOM killer may take MoveOriginal down mid-set. Default pool 128 MB, max 512 MB; guard = `min(pool_mb, MemAvailable−350 MB)`.
- GLIBC ≤ 2.29: use `open("/dev/shm/...")` not `shm_open`; don't link `-lrt`; don't use `-Ofast`.
- Files on device owned by `ableton:users`.

---

## Design conversation reference

> "Schwung has grown a lot since I last made a module in July. Analyze all the stuff that is
> available now. What is genuinely missing creatively?" … catalog analysis surfaced
> corpus/concatenative synthesis as a structural gap (five granular modules, all one-buffer);
> the CM4/2 GB RAM, three-thread analyst/loader design, fixed-vs-arena pool, and OOM constraints
> were worked out over the following turns. v1 decisions (2026-09-01): **Both** play models
> (corpus instrument + audio mosaicker) in one sound_generator; **fixed pool at load**.
