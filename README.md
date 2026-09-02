# Concat

**Corpus concatenative-synthesis instrument for Ableton Move** (via the [Schwung](https://github.com/charlesvestal/schwung) framework).

Point Concat at a *folder* of audio. It analyses and slices every file, describes each slice in a ~36-dimensional timbre space, holds the slices in a RAM pool, and plays back a moving cloud of grains drawn from wherever you are in that space. Travel a **timbre map across many files at once** — or feed it live audio and it re-voices what it hears through your corpus.

> Five granular modules in the Move catalog all read **one** buffer. Concat is the one that navigates a descriptor space across a whole sample **library**.

- **Type:** `sound_generator` · **Module ID:** `concat` · **Abbrev:** `CONCAT`
- **API:** `plugin_api_v2` (`dsp.so`) · **Version:** 0.1.0
- **Author:** Filliformes · **License:** MIT

---

## Table of contents
- [What it does](#what-it-does)
- [Requirements](#requirements)
- [Install](#install)
- [Quickstart — first sound in a minute](#quickstart--first-sound-in-a-minute)
- [Play modes](#play-modes)
- [The pages — full knob reference](#the-pages--full-knob-reference)
- [Pads & expression](#pads--expression)
- [Recipes](#recipes)
- [How it works](#how-it-works)
- [Build from source](#build-from-source)
- [Project layout](#project-layout)
- [Troubleshooting](#troubleshooting)
- [Credits & references](#credits--references)

---

## What it does

Concat is a **CataRT / FluCoMa-style** concatenative synthesizer. Instead of reading time within a single sample, you navigate **timbre** across a whole corpus:

- **Analyse a folder** → onset-slice every file → describe each slice (MFCCs, spectral centroid, flatness, ZCR, loudness, YIN pitch — mean + std, ~36 dims).
- **Navigate** a 2-D map of that space with the X/Y knobs (raw descriptors, or a **learned PCA plane**), or trigger **k-means timbre clusters** with the pads.
- **Play** a polyphonic **grain cloud** of the nearest slices, with per-note envelopes, a DJ filter, saturation, an LFO, and tempo sync.
- Or switch to **Mosaic** and it reconstructs a live line/mic signal out of your corpus in real time.

Everything runs on the Move's CM4 — analysis on a background thread, playback in the real-time audio callback, with an on-disk cache so re-loading a folder is instant.

---

## Requirements

- **Ableton Move** running the **Schwung** framework (v0.9.x+). See the [Schwung repo](https://github.com/charlesvestal/schwung) for installation.
- A folder of audio somewhere under `/data/UserData/UserLibrary/` (Samples, Recordings, Magneto Recs, …). WAV / FLAC / MP3 are supported.

---

## Install

### Via Schwung Manager (recommended)
1. Open the Schwung Manager in a browser: **`http://move.local:7700`**.
2. Install a custom module from this GitHub repo (or upload the `concat-module.tar.gz` from a release).
3. Power-cycle the Move so the module manifest is picked up.

### Manual (SSH)
```bash
scp -r dist/concat root@move.local:/data/UserData/schwung/modules/sound_generators/
ssh root@move.local 'chown -R ableton:users /data/UserData/schwung/modules/sound_generators/concat && chmod +x /data/UserData/schwung/modules/sound_generators/concat/dsp.so'
```
Then power-cycle the Move.

---

## Quickstart — first sound in a minute

1. **Enter Schwung** (long-press Track/Session/Note/Settings, or Shift+Vol+Track) and add **Concat** to a slot.
2. On the **Corpus** page (the landing page), select the **Source** cell (knob 1), press the jog wheel to open the file browser (rooted at `/data/UserData/UserLibrary`), and pick **any audio file**. Concat analyses that file's **whole folder**.
3. Watch **Analyze** count up to **100%**. (A big folder at the default 128 MB pool can take ~1 minute the first time; after that it's cached and instant.)
4. It defaults to **Play Mode = Notes**, **Note Match = Zone** — so **hit the pads**: each pad plays a different part of your corpus.
5. Prefer to explore by hand? Set **Play → Play Mode = Map**, go to the **Map** page and sweep **X Pos / Y Pos**. Turn **Map Mode = Learned** for a spread-out timbre plane.

That's it. From here, shape the sound on the Grain / Shape / Out pages.

> **Tip:** if analysis feels slow on a huge library, lower **Pool Cap** (Corpus page) or pick a smaller sub-folder. The result is cached per folder — subsequent picks of the same folder load instantly.

---

## Play modes

Set on the **Play** page → **Play Mode**.

| Mode | What it does |
|------|--------------|
| **Notes** *(default)* | Polyphonic — each held pad is its own voice with its own amp envelope. **Note Match** decides how a note maps to the corpus (below). Great for playing the corpus like an instrument. |
| **Map** | Free navigation. The X/Y knobs move a point through the timbre map; grains play the nearest slices. Hold a **pad** to jump to a k-means **timbre cluster**; release to return to the knobs. Add the **LFO** (Mod page) to auto-travel. |
| **Mosaic** | Reads the live **line/mic** input, extracts its timbre every ~23 ms, and plays the nearest corpus slices — real-time audio mosaicking ("beatbox → corpus"). Blend the dry input with **Dry Mix** (Out page). |

**Note Match** (Notes mode):
- **Zone** *(default)* — each pad maps to a different **position** on the map (different part of the corpus), at natural pitch. Pads = different sounds.
- **Pitch** — picks the nearest-pitched slice and **retunes** it to the played note. Play the corpus chromatically.
- **Off** — plays the current map point, retuned to the note (chromatic sampler of one spot).

---

## The pages — full knob reference

Pages are reached from the Corpus (root) menu: **Map · Mod · Grain · Shape · Play · Out**.

### Corpus (root)
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **Source** | file browser (`.wav/.flac/.mp3`) | — | Pick a file; its **folder** (recursively) becomes the corpus. |
| **Pool Cap** | 16–512 MB | 128 | RAM budget for pooled audio. Guarded against system OOM. |
| **Rescan** | trigger | — | Force a fresh re-analysis (rebuilds the disk cache). Turn right to fire. |
| **Slice Mode** | Onset / Fixed | Onset | Onset = spectral-flux slicing; Fixed = fixed-length grains. |
| **Min Slice** | 20–500 ms | 60 | Minimum slice length. |
| **Max Slice** | 100–2000 ms | 400 | Maximum slice length. |
| **Analyze** | 0–100 % | — | Live progress read-out (counts up during analysis). |
| **Silence Gate** | 0–1 | 0.02 | Drops slices quieter than this (skips silence). |

### Map
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **X Descriptor** | Bright / Pitch / Loud / Noisy / Rolloff / MFCC1–3 | Bright | X axis in **Raw** map mode. |
| **Y Descriptor** | same list | Pitch | Y axis in Raw map mode. |
| **X Position** | 0–1 | 0.5 | Position along X. A prime host-LFO target. |
| **Y Position** | 0–1 | 0.5 | Position along Y. |
| **Map Mode** | Raw / Learned | Raw | **Learned** = a PCA timbre plane (the two axes of greatest variation) — a spread-out, coherent map instead of two hand-picked descriptors. |
| **Radius** | 0–1 | 0.15 | How far around the target grains may wander (variety). |
| **K Nearest** | 1–16 | 4 | Neighbourhood size for selection. |
| **Repeat Avoid** | 0–16 | 4 | Excludes the last N played slices (CataRT "forbidding"). |

### Mod
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **Follow** | Manual / LFO / Sequence | Manual | Manual = knobs only. **LFO** = free-running auto-travel. **Sequence** = tempo-locked to MIDI clock. |
| **LFO Rate** | 0.01–10 | 0.2 | Hz in LFO mode; **cycles per beat** in Sequence mode (0.25 = 1 bar, 1 = 1/4, 2 = 1/8…). |
| **LFO Shape** | Circle / SineX / SineY / S&H | Circle | Circle = Lissajous sweep; S&H = beat-synced random jumps (with Sequence). |

### Grain
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **Density** | 1–100 g/s | 20 | Grains per second. |
| **Grain Size** | 10–2000 ms | 120 | Grain length. Grains loop the slice (with a seam crossfade) when longer than it. |
| **Grain Env** | Hann / Tukey / Tri / Gauss / Expo / RevExpo / Trapez / Black | Hann | Window shape. |
| **Pitch** | −24…+24 st | 0 | Transpose the grains. |
| **Pan Spread** | 0–1 | 0.3 | Random stereo spread per grain. |
| **Grain Rand** | 0–1 | 0 | Randomises each grain's size (organic clouds). |
| **Reverse** | 0–1 | 0 | Probability a grain plays backwards. |
| **Pitch Jitter** | 0–1 | 0 | Random per-grain detune. |

### Shape (per-note envelopes, Notes mode)
| Knob | Range | Default | Notes |
|------|-------|---------|-------|
| **Amp Atk / Dec / Sus / Rel** | 0–1 | 0.01 / 0.2 / 1.0 / 0.2 | Per-note amplitude ADSR (each pad gets its own). |
| **Filt Env** | 0–1 | 0.5 | Filter-envelope amount (bipolar; 0.5 = no effect). |
| **Filt Atk / Dec / Sus** | 0–1 | 0.01 / 0.3 / 0 | Filter-envelope stages (modulates the DJ filter). |

### Play
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **Play Mode** | Map / Notes / Mosaic | Notes | See [Play modes](#play-modes). |
| **Note Match** | Off / Pitch / Zone | Zone | How pads map to the corpus (Notes mode). |
| **Pressure** | Off / Proximity / Density / Bright / Sat / Pitch / Grain / Jitter | Proximity | Which parameter **pad pressure** (aftertouch) modulates. |
| **Press Amt** | 0–1 | 1.0 | Depth of the pressure modulation. |
| **Vel Amount** | 0–1 | 1.0 | 0 = velocity ignored (always full); 1 = velocity fully scales volume. |
| **Polyphony** | 1–128 | 48 | Grain-voice ceiling. |

### Out
| Knob | Range | Default | Notes |
|------|-------|---------|-------|
| **Filter** | 0–1 | 0.5 | **DJ-style bipolar filter**: centre = bypass, left = low-pass sweeping down, right = high-pass sweeping up. |
| **Resonance** | 0–1 | 0 | Filter resonance. |
| **Saturation** | 0–1 | 0 | tanh drive (all modes). |
| **Dry Mix** | 0–1 | 0 | Blends the live input (Mosaic mode). |
| **Volume** | 0–1 | 0.8 | Output level. |
| **Width** | 0–1 | 1.0 | Stereo width. |

---

## Pads & expression

- **Notes mode:** pads play polyphonic voices (up to 16 held notes, 128 grain voices total). **Zone** spreads pads across the corpus; **Pitch** plays it chromatically.
- **Map mode:** **hold a pad** to jump the query to one of 16 **k-means timbre clusters**; release to return to the X/Y knobs / LFO.
- **Pad pressure (aftertouch):** routed to the **Pressure** destination on the Play page — squeeze a pad to open the filter (Bright), thicken the cloud (Density), pull nearby sounds (Proximity), etc.
- **Velocity:** scales volume, governed by **Vel Amount**.
- **MIDI:** pitch bend (± transpose), MIDI clock (drives Sequence mode + tempo-locked LFO), CC123 (all notes off).

---

## Recipes

- **Play a field-recording library melodically** — Notes mode, Note Match = **Pitch**, pick a tonal corpus, add a little **Amp Rel** on the Shape page.
- **Self-playing timbre drift** — Map mode, Map Mode = **Learned**, Mod → Follow = **LFO**, Shape = **Circle**, LFO Rate ~0.1 Hz. Let it wander.
- **Beat-synced glitch** — Map mode, Follow = **Sequence**, LFO Shape = **S&H**, LFO Rate = 2 (1/8-note jumps) with the Move's clock running.
- **Live mosaic** — Play Mode = **Mosaic**, beatbox/sing into the input; raise **Density** and lower **Grain Size** for a tighter reconstruction; **Dry Mix** to taste.
- **Smooth pad / drone** — long **Grain Size** (800–2000 ms), higher **Density**, **Radius** low, a soft **Grain Env** (Gauss). Concatenation-cost keeps consecutive grains flowing.
- **Percussive stabs** — short **Grain Size** (20–60 ms), fast **Amp Atk/Rel**, **Grain Rand** up for variation.

---

## How it works

Three cooperating threads (the Mello async-loader pattern):

1. **Render** (`render_block`, 128 frames @ 44.1 kHz): schedules/plays the grain cloud, runs the nearest-neighbour query, and — in Mosaic mode — reads the live input and extracts its descriptors. Resident memory only; no disk, no allocation, no locks.
2. **Analyst** (`SCHED_OTHER`, cores 0–2): walks the folder → decodes (dr_libs) → normalises to mono/44.1 k/int16 → onset-slices → describes each slice → fits normalisation, k-means clusters and the PCA map → publishes an **atomic index swap**.
3. **Loader / cache:** the built index (pool + descriptors + clusters + PCA) is written to an on-disk cache under `/data/UserData/schwung/concat-cache/`, keyed by a signature over the folder's file mtimes/sizes and slice settings. Re-picking an unchanged folder loads in milliseconds.

**Signal path.** dr_libs decode → spectral-flux onset slicing (adaptive local threshold) → per-slice MultiStats descriptors (mean + std over MFCC 1–13, centroid, flatness, ZCR, + loudness and YIN pitch) → RobustScaling (median/IQR) → linear-scan weighted **kNN** with repetition-avoidance and a **concatenation-cost** term (favours grain-to-grain continuity) → grain cloud (looping read, seam crossfade, per-window declick, per-note live envelopes).

**DSP is clean-room / first-party**, MIT-licensed. In-house radix-2 FFT (never FFTW). Algorithms studied from FluCoMa (BSD-3) and Schwarz's CataRT papers, reimplemented from scratch.

**Memory.** Fixed pool at load (default 128 MB, max 512), guarded against system OOM (`min(pool_mb, MemAvailable − 350 MB)`). If a corpus exceeds the pool, it **decimates a representative subset across the whole library** rather than truncating to the first files.

---

## Build from source

Requires Docker (for the aarch64 cross-compile) and SSH access to the Move.

```bash
./scripts/build.sh      # Docker ARM64 cross-compile → dist/concat/dsp.so + concat-module.tar.gz
./scripts/install.sh    # scp to the Move + fix ownership + chmod +x
./scripts/test.sh       # cross-compile + run the unit test suite on-device
```

After deploying, reload with `restart-move.sh` on the device, then **power-cycle** the Move (the module manifest is cached at startup).

---

## Project layout

```
src/
  module.json          # metadata, capabilities, ui_hierarchy, chain_params
  dsp/
    concat.c           # main DSP: params, threads, MIDI, render, all play modes
    analysis.{c,h}     # FFT, MFCC/mel/DCT, centroid/flatness/flux, YIN, Welford
    corpus.{c,h}       # onset slicing, per-slice descriptors, RobustScaling
    corpus_engine.{c,h}# folder → pool + index; disk cache; k-means; PCA; OOM eviction
    query.{c,h}        # kNN selection, repeat-avoid, concatenation-cost
    grain.{c,h}        # grain cloud: scheduling, windows, looping read
    ingest.{c,h}       # dr_libs decode, resample, recursive folder scan
    vendor/            # dr_wav / dr_flac / dr_mp3 (public domain)
scripts/               # build.sh, install.sh, test.sh, Dockerfile
tests/                 # on-device unit tests (analysis, corpus, engine, ingest, query)
```

---

## Troubleshooting

- **Analyze stuck at 0%** — the folder has no decodable audio at that path, or the pick didn't register; re-select a **file** (not a folder) in the Source browser. Very large single files (>128 MB) are skipped.
- **Slow first analysis** — expected for big libraries; lower **Pool Cap** or pick a sub-folder. It's cached afterward — instant on re-pick. Hit **Rescan** to force a rebuild after changing files.
- **Source shows "float 0.00–1.00" or "None"** — the module metadata didn't load; power-cycle the Move.
- **No sound** — check Play Mode/Volume, confirm Analyze reached 100%, and that a note/pad is held (Notes) or X/Y is on a populated region (Map).

---

## Credits & references

- Built on **[Schwung](https://github.com/charlesvestal/schwung)** by Charles Vestal & contributors.
- Decoding via **[dr_libs](https://github.com/mackron/dr_libs)** (public domain).
- Algorithms studied (not vendored) from **[FluCoMa](https://www.flucoma.org/)** (BSD-3) and Diemo Schwarz's **CataRT** papers on corpus-based concatenative synthesis.
- Instrument & DSP by **Filliformes** — MIT licensed (see `LICENSE`).

*Not affiliated with or endorsed by Ableton.*
