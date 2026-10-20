# Concat

**Corpus concatenative-synthesis instrument for Ableton Move**, built on the [Schwung](https://github.com/charlesvestal/schwung) framework.

Concat cuts a collection of samples into slices, measures each slice (brightness, noisiness, loudness, pitch, timbre: 36 numbers), and lays them out on a map. You play the map: a cloud of grains comes from the slices nearest your position. Move through timbre across a whole library, play it from the pads, or sing into it and hear your voice rebuilt from the corpus.

I made it because the granular modules on the Move each read one buffer. Concat reads a sample library at once, and comes with a web page to build, trim and hear a corpus before you load it.

**[Manual and quickstart](https://filliformes.github.io/concat-move/)** · [Full reference (Markdown)](docs/MANUAL.md) · [Changelog](docs/CHANGELOG.md) · [Latest release](https://github.com/filliformes/concat-move/releases/latest)

- **Type:** `sound_generator` · **Module ID:** `concat` · **Version:** 0.2.0
- **Requires:** Schwung 1.3.0 or later (tested on 1.5.0)
- **Author:** Filliformes · **License:** MIT

---

## Contents
- [Status](#status)
- [Install](#install)
- [Quickstart](#quickstart)
- [The Corpus Builder](#the-corpus-builder)
- [Corpus files](#corpus-files)
- [Playing it](#playing-it)
- [How it works](#how-it-works)
- [Build from source](#build-from-source)
- [Project layout](#project-layout)
- [Credits](#credits)

---

## Status

Version 0.2.0. New since 0.1.0: corpus files (`.concat`) with live reload and a per-file analysis cache, the Corpus Builder web page, Concat's panel in the manager's Remote UI, the View page with its live map and recipes, and a reliability pass on the engine (switching modules mid-analysis took up to 72 s and now takes a few ms; a 25,000-slice corpus no longer overloads the audio thread). Tested on a Move running Schwung 1.5.0.

Still rough, worth knowing:

- The filter and the Shape filter envelope are global, not per voice.
- `Follow = Sequence` needs the Move's MIDI clock running.
- The first analysis of a large library can take a minute or more. It is cached afterward, per file.
- Nothing cleans the analysis caches yet: on the test Move `concat-cache` grew to about 350 MB. Delete `/data/UserData/schwung/concat-cache` to reclaim it; it rebuilds as needed.
- In the manager's Remote UI, Concat's panel replaces the automatic knob list for its track. **Interface → Default** brings the knobs back until the page reloads.
- Installing or updating the module needs a power-cycle (Schwung reads module manifests at startup).

---

## Install

1. Your Move runs [Schwung](https://github.com/charlesvestal/schwung) 1.3.0 or later.
2. Open schwung-manager at **`http://move.local:7700`** and install a custom module from this repo, or upload `concat-module.tar.gz` from the [latest release](https://github.com/filliformes/concat-move/releases/latest).
3. Power-cycle the Move.

Manual install over SSH:
```bash
scp -r dist/concat root@move.local:/data/UserData/schwung/modules/sound_generators/
ssh root@move.local 'chown -R ableton:users /data/UserData/schwung/modules/sound_generators/concat && chmod +x /data/UserData/schwung/modules/sound_generators/concat/dsp.so'
```

---

## Quickstart

1. In Schwung, add **Concat** to a track. A new Concat loads the first sample of your first Samples folder by itself.
2. To choose your own, open **Corpus** from the root menu, select **Source** (knob 1), press the jog wheel, and pick a sample (`.wav .aif .aiff .flac .mp3`) or a `.concat` file from `Corpora`.
3. **Scope** (on **Mod & Set**) decides what a picked sample means: **File** (default) makes that sample the whole corpus, cut into many slices; **Folder** takes its whole folder.
4. Wait for **Analyze** to reach 100 %. A big folder takes a minute the first time; after that it loads from the cache.
5. Play the pads. Each pad plays a different part of the corpus (Play Mode Notes, Note Match Zone).
6. Open **View** to see the map, and turn the jog wheel through the **Recipes** (Cloud, Stutter, Drone, Scatter, Pitched, Mosaic, Sequence).

---

## The Corpus Builder

A web page that runs on your Move, served by schwung-manager. With the Move on your network (or USB), open it in a desktop browser:

```
http://move.local:7700/api/remote-ui/module-assets/concat/builder/app.html
```

It is also linked from Concat's panel in the manager's **Remote UI**.

- **Gather:** drag folders and samples from your UserLibrary onto the corpus. **Ctrl+A** selects a whole folder; **↑/↓** step through samples and play each one.
- **Listen and prune:** folders open into their samples; play them one by one, leave some out, or filter by name (`*Kick*`).
- **Trim:** drag on a sample's waveform to keep part of it; click a slice to hear just that slice.
- **Choose descriptors:** statistics and a histogram for each of the 36, a checkbox per group.
- **See and play the map:** Concat's Learned (3-D) or Raw X/Y map, coloured by source, descriptor or pad cluster. Click a dot to hear a slice; **Play the map** fires grains the way Concat does; hold a pad in the pad grid to hear what that pad reaches on the Move.
- **Record** what you play and save the WAV to the Move.
- **Save** to `UserLibrary/Corpora`, and **Load on track N** to make Concat play it. After that, every save reloads it on the Move while the old version keeps playing.

The page runs Concat's own decoder, slicer and descriptor code compiled to WebAssembly: on real samples it gives the same slice boundaries as the Move and descriptors within 0.06 %. A saved 9214-slice corpus built to exactly 9214 slices on the device.

---

## Corpus files

A `.concat` file is a corpus described in JSON: several folders and samples, start and end points, name patterns, slicing, and which descriptors Concat uses.

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

Paths are relative to `/data/UserData/UserLibrary`. Everything except `sources` is optional. When the loaded file changes, Concat rebuilds it in the background and only re-analyses what changed (a 9170-slice corpus went live in 8.6 s instead of 76 s). A broken file keeps the old corpus and the View page says why. Every key is described in the [manual](docs/MANUAL.md#6-corpus-files-concat).

---

## Playing it

| Play Mode | What it does |
|-----------|--------------|
| **Notes** (default) | Polyphonic: each pad is a voice with its own envelope. **Note Match** Zone (each pad somewhere else on the map), Pitch (nearest pitch, retuned), Off (current position, retuned). |
| **Map** | Move through the map with X/Y, the LFO, or the MIDI clock. Hold a pad to jump to one of 16 timbre clusters. |
| **Mosaic** | Answers the line/mic input with the nearest slices, about every 23 ms. |

Pages: **root** (8 performance knobs) · **View** (live map, recipes) · **Corpus** · **Map** · **Mod & Set** · **Grain** · **Shape** · **Play** · **Out**. Every parameter, range and default is in the [manual](docs/MANUAL.md#3-the-pages).

---

## How it works

- **Analysis thread** (cores 0 to 2): resolves the sources, decodes in 60 s windows (dr_libs, then Concat's cubic resampler to mono 44.1 kHz), cuts slices at onsets (spectral flux), describes each slice (MFCC 1 to 13, centroid, flatness, ZCR, as mean and variation, plus loudness and YIN pitch), scales them (median/IQR), then computes 16 k-means clusters and the PCA map, and swaps the new corpus in atomically. It can be cancelled at any point, so switching modules never waits on it.
- **Audio thread** (128 frames at 44.1 kHz): picks slices near the target (weighted nearest neighbours over the descriptors in use, repeat avoid, a continuity term), and plays a grain cloud. No allocation, no disk, no locks. Large searches are spread over several blocks.
- **Memory:** a fixed pool (128 MB by default, 512 max) checked against free RAM with 350 MB kept for the Move. A corpus that does not fit keeps an even subset of slices across all of it.
- **Caches** in `/data/UserData/schwung/concat-cache`: the whole corpus (instant reload) and each file (edits re-analyse only what changed). Status files there let the Builder and the Remote UI panel follow the analysis.

The DSP is written from scratch and MIT licensed. The algorithms were studied from FluCoMa (BSD-3) and Diemo Schwarz's CataRT papers.

---

## Build from source

Needs Docker and SSH access to the Move.

```bash
./scripts/build.sh       # ARM64 cross-compile -> dist/concat + concat-module.tar.gz
./scripts/install.sh     # copy to the Move (staged, atomic), fix ownership
./scripts/test.sh        # build and run the unit tests on the Move
./scripts/session_sim.sh # drive a full session against a copy of the installed dsp.so
./scripts/build_wasm.sh  # rebuild src/builder/cb.js after changing analysis, corpus or ingest code
```

Power-cycle the Move after installing a new build if `module.json` changed; a restart is enough otherwise.

---

## Project layout

```
src/
  module.json            metadata, capabilities, chain_params (version = release tag)
  help.json              on-device help
  canvas.js              the View page map (as_page canvas)
  web_ui.html            Concat's panel in the manager's Remote UI
  dsp/
    concat.c             params, analysis thread, MIDI, render, play modes, state, status files
    corpus_engine.{c,h}  sources -> pool + index; caches; k-means; PCA; memory guard
    corpus_file.{c,h}    .concat files: sources, ranges, patterns, descriptor mask
    json.{c,h}           small JSON reader
    ingest.{c,h}         streamed decode (dr_libs), resampler, folder listing
    analysis.{c,h}       FFT, mel/MFCC, centroid, flatness, flux, YIN
    corpus.{c,h}         onset slicing, per-slice descriptors, robust scaling
    query.{c,h}          nearest-neighbour selection, repeat avoid, continuity
    grain.{c,h}          grain cloud
    vendor/              dr_wav, dr_flac, dr_mp3 (public domain)
  wasm/cb_wasm.c         Corpus Builder analysis entry points (built to WebAssembly)
  builder/               Corpus Builder page (app.html, builder.js, map3d.js, worker.js, cb.js)
docs/
  index.html             the web manual (GitHub Pages)
  MANUAL.md              full reference
  CHANGELOG.md
  corpus-builder-design.md, audit-2026-09-28/, releases/
scripts/                 build, install, test, session simulator, WASM build
tests/                   unit tests, session simulator, Builder parity harness
```

---

## Credits

- Built on **[Schwung](https://github.com/charlesvestal/schwung)** by Charles Vestal and contributors.
- Decoding via **[dr_libs](https://github.com/mackron/dr_libs)** (public domain).
- Algorithms studied (not vendored) from **[FluCoMa](https://www.flucoma.org/)** (BSD-3) and Diemo Schwarz's **CataRT** papers on corpus-based concatenative synthesis.
- Instrument, DSP and Corpus Builder by **Filliformes**, MIT licensed (see `LICENSE`).
