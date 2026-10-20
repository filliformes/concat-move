# Concat

**Corpus concatenative-synthesis instrument for Ableton Move**, built on the [Schwung](https://github.com/charlesvestal/schwung) framework.

Point Concat at a *folder* of audio. It analyses and slices every file, describes each slice in a roughly 36-dimensional timbre space, keeps the slices in a RAM pool, and plays a moving cloud of grains drawn from wherever you are in that space. You travel a timbre map across many files at once, or feed it live audio and it re-voices what it hears through your corpus.

I made it to fill an actual gap: the other granular modules in the Move catalog each read one buffer. Concat navigates a descriptor space across a whole sample library instead.

- **Type:** `sound_generator` · **Module ID:** `concat` · **Abbrev:** `CONCAT`
- **API:** `plugin_api_v2` (`dsp.so`) · **Version:** 0.1.0
- **Author:** Filliformes · **License:** MIT

---

## Table of contents
- [What it does](#what-it-does)
- [Status](#status)
- [Requirements](#requirements)
- [Install](#install)
- [Quickstart](#quickstart)
- [Corpus files and the Corpus Builder](#corpus-files-and-the-corpus-builder)
- [Play modes](#play-modes)
- [The pages: full knob reference](#the-pages-full-knob-reference)
- [Pads and expression](#pads-and-expression)
- [Recipes](#recipes)
- [How it works](#how-it-works)
- [Build from source](#build-from-source)
- [Project layout](#project-layout)
- [Troubleshooting](#troubleshooting)
- [Credits and references](#credits-and-references)

---

## What it does

Concat is a CataRT / FluCoMa-style concatenative synthesizer. Instead of reading time within a single sample, you navigate timbre across a whole corpus:

- **Analyse a folder:** onset-slice every file, then describe each slice (MFCCs, spectral centroid, flatness, ZCR, loudness, YIN pitch, as mean and std, about 36 dimensions).
- **Navigate** a 2-D map of that space with the X/Y knobs (raw descriptors, or a learned PCA plane), or trigger k-means timbre clusters with the pads.
- **Play** a polyphonic grain cloud of the nearest slices, with per-note envelopes, a DJ filter, saturation, an LFO, and tempo sync.
- Or switch to **Mosaic**, which reconstructs a live line/mic signal out of your corpus in real time.

Analysis runs on a background thread, playback in the real-time audio callback, with an on-disk cache so re-loading a folder is instant.

## Status

Version 0.1.0. The engine is feature-complete and the code has been through a two-reviewer audit (see the commit history). Some things are still rough and worth knowing:

- The filter and Shape filter-envelope are global, not per-voice.
- `Follow = Sequence` needs the Move's MIDI clock running to lock tempo.
- First analysis of a large library can take up to a minute; it is cached afterward.
- Installing or updating the module needs a device power-cycle (Schwung caches the manifest at startup).

---

## Requirements

- **Ableton Move** running the **Schwung** framework (v0.9.x or later). See the [Schwung repo](https://github.com/charlesvestal/schwung) for installation.
- A folder of audio somewhere under `/data/UserData/UserLibrary/` (Samples, Recordings, Magneto Recs, and so on). WAV, FLAC, and MP3 are supported.

---

## Install

### Via Schwung Manager (recommended)
1. Open the Schwung Manager in a browser: **`http://move.local:7700`**.
2. Install a custom module from this GitHub repo, or upload the `concat-module.tar.gz` from a release.
3. Power-cycle the Move so the module manifest is picked up.

### Manual (SSH)
```bash
scp -r dist/concat root@move.local:/data/UserData/schwung/modules/sound_generators/
ssh root@move.local 'chown -R ableton:users /data/UserData/schwung/modules/sound_generators/concat && chmod +x /data/UserData/schwung/modules/sound_generators/concat/dsp.so'
```
Then power-cycle the Move.

---

## Quickstart

1. **Enter Schwung** (long-press Track/Session/Note/Settings, or Shift+Vol+Track) and add **Concat** to a slot.
2. On the **Corpus** page (the landing page), select the **Source** cell (knob 1), press the jog wheel to open the file browser (rooted at `/data/UserData/UserLibrary`), and pick any audio file. By default Concat analyses that file's whole folder; set **Scope = File** (just below Source) to make that one sample the entire corpus.
3. Watch **Analyze** count up to **100%**. A big folder at the default 128 MB pool can take about a minute the first time; after that it is cached and instant.
4. It defaults to **Play Mode = Notes**, **Note Match = Zone**, so hit the pads: each pad plays a different part of your corpus.
5. Prefer to explore by hand? Set **Play → Play Mode = Map**, go to the **Map** page, and sweep **X Pos / Y Pos**. Turn **Map Mode = Learned** for a spread-out timbre plane.

From there, shape the sound on the Grain / Shape / Out pages.

> If analysis feels slow on a huge library, lower **Pool** (Corpus page) or pick a smaller sub-folder. The result is cached per folder, so re-picking the same folder loads in milliseconds.

---

## Corpus files and the Corpus Builder

A corpus does not have to be one folder. A `.concat` file lists several folders and files, can keep only part of a sample, and can choose which descriptors Concat uses. Pick it in the **Source** browser like any sample.

### The Corpus Builder (web page)

With the Move on your network (or USB), open this in a desktop browser:

```
http://move.local:7700/api/remote-ui/module-assets/concat/builder/app.html
```

- **Library** (left): your `UserLibrary`. Drag a folder or a sample onto the corpus column, or use **+**. Click to select, Ctrl/Shift-click for more, **Ctrl+A** for every sample in the folder, then drag them all or press **Add**. **↑/↓** steps through samples and plays each one.
- **Playing**: **▶** turns into **⏸** while a sound plays. ⏸ pauses, ▶ resumes from there, **■ Stop** or **Esc** goes back to the start. **Space** plays or pauses the selected sample.
- **Corpus sources** (middle): click a source to edit it. A sample shows its waveform: drag on it to keep only that part, click a slice to hear just that slice. A folder arrives collapsed: open it (**▸**) to see its samples, play them one by one, and leave some out with **✕** (**+** puts them back). **↑/↓** steps through them, **→/←** opens and closes a folder. **Trim on its own** pulls a sample out of its folder so you can trim it. A folder also takes **Only** / **Skip** name patterns (`*Kick*`, `*click*`) and a subfolders switch. **✕** removes a source from the corpus; the file stays on the Move.
- **Slicing** (bottom): the same onset/fixed slicing as the Corpus page, saved into the file.
- **Map** (right): every slice of the corpus. Two views, the same as Concat's **Map Mode**: **Raw** (pick X and Y from Concat's menu: Bright, Pitch, Loud, Noisy, Rolloff, MFCC1-3; Concat's default is Bright × Pitch) and **Learned** (PCA of the descriptors you keep, in 3-D; **Move view** shows the plane Concat uses). Colour the dots by source or by a descriptor. Click a source in the list and its slices light up. Click a dot to hear it.
- **Play the map** (or hold Shift and drag, or press **P**): press and move over the map to play it like Concat's Map mode: grains at **Density** per second, each a random slice among the **Spread** nearest dots, **Grain** long. Defaults match Concat (20/s, 120 ms, 4).
- **Pad clusters**: set Colour to **Pad clusters** to see Concat's 16 k-means clusters, the ones a held pad jumps to in Map mode, with a pad grid (Move pad layout, notes 68-99). Hold a pad in the grid to hear its cluster. The page uses Concat's own k-means, seeds and folder order; on a 9214-slice test corpus the cluster sizes matched the Move to within 2 slices.
- **Record** (**● Rec** or **R**): records what you play on the map and pads. Listen, download the WAV, or save it to `UserLibrary/Recordings/Corpus Builder` on the Move and add it straight back into the corpus.
- **Pool meter**: the corpus size against Concat's Pool Cap (128 MB default, 512 MB max), with the Pool Cap to set when it is bigger.
- **Descriptors** tab: min, median, IQR and a histogram for each of the 36 descriptors, a checkbox per group, and how much each one shapes the map.
- **Save** writes `UserLibrary/Corpora/<name>.concat`. **Open** loads one back. Unsaved work survives a page reload.

The page runs Concat's own decoder, slicer and descriptor code, compiled to WebAssembly, in your browser. The Move does no work while you edit. Checked on real samples: same slice boundaries as the Move, descriptors within 0.06% of each descriptor's spread. A saved test corpus of 9214 slices built to exactly 9214 slices on the device. Analysis results are cached in the browser, so reopening a corpus is instant.

### The page and the Move

- The header shows which track Concat is on and what it plays, with live progress while it analyses (`Track 2 Builder Test: analysing 12/40`).
- **Load on track N** saves the corpus and makes Concat on that track play it: no trip to the Source browser. With Concat on several tracks, pick the track first.
- In schwung-manager's **Remote UI**, Concat's track shows a small panel: the corpus playing and its status, a menu of your corpus files to load, and a button to open the Corpus Builder. For every knob, switch **Interface** to **Default** at the top of the track.

How: the page uses the manager's Remote UI WebSocket (which module is on each track, and setting Concat's `corpus_file`). Progress comes from small status files Concat's analysis thread writes to `/data/UserData/schwung/concat-cache/status/`. Concat saves its settings as a JSON object, so the manager reads them in one go; sets saved before this still load.

### On the Move

- If the loaded `.concat` file changes (saved from the page, or edited by hand), Concat rebuilds it in the background while the old corpus keeps playing. Only new or changed files are analysed again: an edit to a 9170-slice corpus went live in 8.6 s instead of 76 s.
- A broken or missing file keeps the old corpus and says why on the View page (`bad JSON: ..., kept old`). Missing sources are skipped and counted (`2 sources missing`).

### The file format

```json
{
  "concat": 1,
  "name": "Night Market",
  "sources": [
    { "path": "Samples/Ahuntsic_Ambiances" },
    { "path": "Samples/MELLOTRON/M300SaxB-20.wav", "start": 0.4, "end": 6.2 },
    { "path": "Samples/DRUMS", "include": ["*Kick*"], "recurse": false }
  ],
  "exclude": ["*click*"],
  "descriptors": { "use": ["loud", "centroid", "flatness", "pitch", "mfcc1-13"] },
  "slicing": { "mode": "onset", "min_ms": 40, "max_ms": 600, "gate": 0.02 }
}
```

Paths are relative to `/data/UserData/UserLibrary`. `start`/`end` are seconds. Patterns match file names, ignoring case; the top-level `exclude` applies to folder sources. Descriptor groups: `loud`, `centroid`, `flatness`, `zcr`, `pitch`, `mfcc`, `mfccN`, `mfccA-B`. `slicing` overrides the Corpus page knobs. Every key except `sources` is optional.

---

## Play modes

Set on the **Play** page, under **Play Mode**.

| Mode | What it does |
|------|--------------|
| **Notes** *(default)* | Polyphonic. Each held pad is its own voice with its own amp envelope. **Note** decides how a note maps to the corpus (below). Good for playing the corpus like an instrument. |
| **Map** | Free navigation. The X/Y knobs move a point through the timbre map; grains play the nearest slices. Hold a **pad** to jump to a k-means timbre cluster; release to return to the knobs. Add the **LFO** (Mod page) to auto-travel. |
| **Mosaic** | Reads the live line/mic input, extracts its timbre about every 23 ms, and plays the nearest corpus slices. Real-time audio mosaicking (beatbox into a corpus). Blend the dry input with **Dry Mix** (Out page). |

**Note** (Notes mode):
- **Zone** *(default)*: each pad maps to a different position on the map (a different part of the corpus), at natural pitch. Pads become different sounds.
- **Pitch**: picks the nearest-pitched slice and retunes it to the played note. Plays the corpus chromatically.
- **Off**: plays the current map point, retuned to the note (a chromatic sampler of one spot).

---

## The pages: full knob reference

Pages are reached from the Corpus (root) menu: **Map · Mod · Grain · Shape · Play · Out**.

### Corpus (root)
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **Source** | file browser (`.wav/.flac/.mp3`) | (none) | Pick an audio file. What becomes the corpus depends on **Scope** below. |
| **Scope** | Folder / File | Folder | **Folder**: the picked file's folder, recursively (the usual corpus). **File**: that one sample becomes the whole corpus, still onset-sliced into many pieces. Changing it re-analyses the same pick. |
| **Pool** | 16 to 512 MB | 128 | RAM budget for pooled audio. Guarded against system OOM. |
| **Rescan** | trigger | (none) | Force a fresh re-analysis (rebuilds the disk cache). Turn right to fire. |
| **Slice** | Onset / Fixed | Onset | Onset uses spectral-flux slicing; Fixed uses fixed-length grains. |
| **Min Sl** | 20 to 500 ms | 60 | Minimum slice length. |
| **Max Sl** | 100 to 2000 ms | 400 | Maximum slice length. |
| **Analyze** | 0 to 100 % | (live) | Progress read-out (counts up during analysis). |
| **Sil Gate** | 0 to 1 | 0.02 | Drops slices quieter than this (skips silence). |

### Map
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **X Desc** | Bright / Pitch / Loud / Noisy / Rolloff / MFCC1-3 | Bright | X axis in **Raw** map mode. |
| **Y Desc** | same list | Pitch | Y axis in Raw map mode. |
| **X Pos** | 0 to 1 | 0.5 | Position along X. A good host-LFO target. |
| **Y Pos** | 0 to 1 | 0.5 | Position along Y. |
| **Map Mode** | Raw / Learned | Raw | **Learned** uses a PCA timbre plane (the two axes of greatest variation): a spread-out, coherent map instead of two hand-picked descriptors. |
| **Radius** | 0 to 1 | 0.15 | How far around the target grains may wander (variety). |
| **K Near** | 1 to 16 | 4 | Neighbourhood size for selection. |
| **Rpt Avoid** | 0 to 16 | 4 | Excludes the last N played slices (CataRT "forbidding"). |

### Mod
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **Follow** | Manual / LFO / Sequence | Manual | Manual is knobs only. **LFO** is free-running auto-travel. **Sequence** locks to the MIDI clock. |
| **LFO Rate** | 0.01 to 10 | 0.2 | Hz in LFO mode; cycles per beat in Sequence mode (0.25 is 1 bar, 1 is a 1/4 note, 2 is a 1/8). |
| **LFO Shp** | Circle / SineX / SineY / S&H | Circle | Circle is a Lissajous sweep; S&H does beat-synced random jumps (with Sequence). |

### Grain
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **Density** | 1 to 100 g/s | 20 | Grains per second. |
| **Grain Size** | 10 to 2000 ms | 120 | Grain length. Grains loop the slice (with a seam crossfade) when longer than it. |
| **Grn Env** | Hann / Tukey / Tri / Gauss / Expo / RevExpo / Trapez / Black | Hann | Window shape. |
| **Pitch** | -24 to +24 st | 0 | Transpose the grains. |
| **Pan Sprd** | 0 to 1 | 0.3 | Random stereo spread per grain. |
| **Grn Rand** | 0 to 1 | 0 | Randomises each grain's size (organic clouds). |
| **Rev** | 0 to 1 | 0 | Probability a grain plays backwards. |
| **Pch Jit** | 0 to 1 | 0 | Random per-grain detune. |

### Shape (per-note envelopes, Notes mode)
| Knob | Range | Default | Notes |
|------|-------|---------|-------|
| **Amp Atk / Dec / Sus / Rel** | 0 to 1 | 0.01 / 0.2 / 1.0 / 0.2 | Per-note amplitude ADSR (each pad gets its own). |
| **Filt Env** | 0 to 1 | 0.5 | Filter-envelope amount (bipolar; 0.5 is no effect). |
| **Filt Atk / Dec / Sus** | 0 to 1 | 0.01 / 0.3 / 0 | Filter-envelope stages (they modulate the DJ filter). |

### Play
| Knob | Range / options | Default | Notes |
|------|-----------------|---------|-------|
| **Play Mode** | Map / Notes / Mosaic | Notes | See [Play modes](#play-modes). |
| **Note** | Off / Pitch / Zone | Zone | How pads map to the corpus (Notes mode). |
| **Pressure** | Off / Proximity / Density / Bright / Sat / Pitch / Grain / Jitter | Proximity | Which parameter pad pressure (aftertouch) modulates. |
| **Press Amt** | 0 to 1 | 1.0 | Depth of the pressure modulation. |
| **Vel Amt** | 0 to 1 | 1.0 | 0 ignores velocity (always full); 1 lets velocity fully scale volume. |
| **Poly** | 1 to 128 | 48 | Grain-voice ceiling. |

### Out
| Knob | Range | Default | Notes |
|------|-------|---------|-------|
| **Filter** | 0 to 1 | 0.5 | DJ-style bipolar filter: centre is bypass, left is a low-pass sweeping down, right is a high-pass sweeping up. |
| **Reso** | 0 to 1 | 0 | Filter resonance. |
| **Sat** | 0 to 1 | 0 | tanh drive (all modes). |
| **Dry Mix** | 0 to 1 | 0 | Blends the live input (Mosaic mode). |
| **Vol** | 0 to 1 | 0.8 | Output level. |
| **Width** | 0 to 1 | 1.0 | Stereo width. |

---

## Pads and expression

- **Notes mode:** pads play polyphonic voices (up to 16 held notes, 128 grain voices total). Zone spreads pads across the corpus; Pitch plays it chromatically.
- **Map mode:** hold a pad to jump the query to one of 16 k-means timbre clusters; release to return to the X/Y knobs or LFO.
- **Pad pressure (aftertouch):** routed to the **Pressure** destination on the Play page. Squeeze a pad to open the filter (Bright), thicken the cloud (Density), pull nearby sounds (Proximity), and so on.
- **Velocity:** scales volume, governed by **Vel Amt**.
- **MIDI:** pitch bend (transpose), MIDI clock (drives Sequence mode and the tempo-locked LFO), CC123 (all notes off).

---

## Recipes

- **Play a field-recording library melodically:** Notes mode, Note Match = Pitch, a tonal corpus, a little **Amp Rel** on the Shape page.
- **Self-playing timbre drift:** Map mode, Map Mode = Learned, Mod → Follow = LFO, Shape = Circle, LFO Rate around 0.1 Hz. Let it wander.
- **Beat-synced glitch:** Map mode, Follow = Sequence, LFO Shape = S&H, LFO Rate = 2 (1/8-note jumps) with the Move's clock running.
- **Live mosaic:** Play Mode = Mosaic, beatbox or sing into the input; raise **Density** and lower **Grain Size** for a tighter reconstruction; **Dry Mix** to taste.
- **Smooth pad / drone:** long **Grain Size** (800 to 2000 ms), higher **Density**, low **Radius**, a soft **Grn Env** (Gauss). The concatenation-cost keeps consecutive grains flowing.
- **Percussive stabs:** short **Grain Size** (20 to 60 ms), fast **Amp Atk/Rel**, **Grn Rand** up for variation.

---

## How it works

Three cooperating threads (the Mello async-loader pattern):

1. **Render** (`render_block`, 128 frames at 44.1 kHz): schedules and plays the grain cloud, runs the nearest-neighbour query, and in Mosaic mode reads the live input and extracts its descriptors. Resident memory only: no disk, no allocation, no locks.
2. **Analyst** (`SCHED_OTHER`, cores 0 to 2): walks the folder, decodes (dr_libs), normalises to mono / 44.1 kHz / int16, onset-slices, describes each slice, fits normalisation, k-means clusters and the PCA map, then publishes an atomic index swap.
3. **Loader / cache:** the built index (pool, descriptors, clusters, PCA) is written to an on-disk cache under `/data/UserData/schwung/concat-cache/`, keyed by a signature over the folder's file mtimes/sizes and slice settings. Re-picking an unchanged folder loads in milliseconds.

**Signal path.** dr_libs decode, spectral-flux onset slicing (adaptive local threshold), per-slice MultiStats descriptors (mean and std over MFCC 1 to 13, centroid, flatness, ZCR, plus loudness and YIN pitch), RobustScaling (median/IQR), a linear-scan weighted kNN with repetition-avoidance and a concatenation-cost term (it favours grain-to-grain continuity), then the grain cloud (looping read, seam crossfade, per-window declick, per-note live envelopes).

**The DSP is clean-room and first-party**, MIT-licensed: in-house radix-2 FFT (never FFTW). Algorithms were studied from FluCoMa (BSD-3) and Diemo Schwarz's CataRT papers, then reimplemented from scratch.

**Memory.** Fixed pool at load (default 128 MB, max 512), guarded against system OOM (`min(pool_mb, MemAvailable minus 350 MB)`). If a corpus exceeds the pool, it decimates a representative subset across the whole library rather than truncating to the first files.

---

## Build from source

Needs Docker (for the aarch64 cross-compile) and SSH access to the Move.

```bash
./scripts/build.sh      # Docker ARM64 cross-compile -> dist/concat/dsp.so + concat-module.tar.gz
./scripts/install.sh    # scp to the Move, fix ownership, chmod +x
./scripts/test.sh       # cross-compile and run the unit test suite on-device
./scripts/build_wasm.sh # rebuild src/builder/cb.js (Corpus Builder analysis core) after changing analysis/corpus/ingest
```

After deploying, reload with `restart-move.sh` on the device, then power-cycle the Move (the module manifest is cached at startup).

---

## Project layout

```
src/
  module.json          # metadata, capabilities, ui_hierarchy, chain_params
  dsp/
    concat.c           # main DSP: params, threads, MIDI, render, all play modes
    analysis.{c,h}     # FFT, MFCC/mel/DCT, centroid/flatness/flux, YIN, Welford
    corpus.{c,h}       # onset slicing, per-slice descriptors, RobustScaling
    corpus_engine.{c,h}# folder -> pool + index; disk cache; k-means; PCA; OOM eviction
    query.{c,h}        # kNN selection, repeat-avoid, concatenation-cost
    grain.{c,h}        # grain cloud: scheduling, windows, looping read
    ingest.{c,h}       # dr_libs decode (streamed), resample, recursive folder scan
    corpus_file.{c,h}  # .concat corpus files: sources, ranges, patterns, descriptor mask
    json.{c,h}         # small JSON reader for .concat files
    vendor/            # dr_wav / dr_flac / dr_mp3 (public domain)
  wasm/cb_wasm.c       # Corpus Builder analysis entry points (built to WebAssembly)
  builder/             # Corpus Builder web page (app.html, builder.js, map3d.js, worker.js, cb.js)
scripts/               # build.sh, install.sh, test.sh, Dockerfile
tests/                 # unit tests (analysis, corpus, corpusfile, engine, ingest, query), session simulator, builder parity
```

---

## Troubleshooting

- **Analyze stuck at 0%:** the folder has no decodable audio at that path, or the pick did not register. Re-select a file (not a folder) in the Source browser. The View page's status line says what went wrong (`no audio files`, `out of memory, kept old`, ...).
- **Slow first analysis:** expected for big libraries. Lower **Pool** or pick a sub-folder. It is cached afterward, so it is instant on re-pick. Hit **Rescan** to force a rebuild after changing files.
- **Source shows "float 0.00-1.00" or "None":** the module metadata did not load. Power-cycle the Move.
- **No sound:** check Play Mode and Volume, confirm Analyze reached 100%, and that a note/pad is held (Notes) or X/Y is on a populated region (Map).

---

## Credits and references

- Built on **[Schwung](https://github.com/charlesvestal/schwung)** by Charles Vestal and contributors.
- Decoding via **[dr_libs](https://github.com/mackron/dr_libs)** (public domain).
- Algorithms studied (not vendored) from **[FluCoMa](https://www.flucoma.org/)** (BSD-3) and Diemo Schwarz's **CataRT** papers on corpus-based concatenative synthesis.
- Instrument and DSP by **Filliformes**, MIT licensed (see `LICENSE`).

Not affiliated with or endorsed by Ableton.
