# Concat Manual

*Corpus concatenative-synthesis instrument for Ableton Move (Schwung sound generator), v0.2.0*

This is the full reference. The [README](../README.md) is the short tour, and the [web manual](https://filliformes.github.io/concat-move/) has the same content with pictures.

---

## 1. The idea

Concat plays a **corpus**: a collection of sounds cut into short slices. Every slice is measured (how bright, how noisy, how loud, what pitch, what timbre) and placed on a map of those measurements. You don't play time within one sample. You move a point on the map, and Concat plays a cloud of grains from the slices nearest that point.

A corpus can be one sample, one folder, or a `.concat` file that gathers many folders and samples, trimmed and filtered. The Corpus Builder web page makes those files and lets you hear the map before you load it.

Three ways to play it (**Play Mode**):

| Mode | What it does |
|------|--------------|
| **Notes** *(default)* | Polyphonic. Each pad is a voice with its own amp envelope. **Note Match** decides how a note maps to the corpus. |
| **Map** | Move a point on the map with **X Position / Y Position** (or the LFO). Hold a pad to jump to one of 16 timbre clusters; release to come back. |
| **Mosaic** | Listens to the line/mic input about every 23 ms and answers with the nearest slices of the corpus: beatbox into a drum corpus, sing into a field-recording corpus. |

**Note Match** (Notes mode):
- **Zone** *(default)*: each pad plays a different place on the map at the slices' own pitch. Pads become different sounds.
- **Pitch**: picks the slice nearest the played pitch and retunes it to the note. The corpus plays chromatically.
- **Off**: plays the current map point, retuned to the note.

---

## 2. Quickstart

1. In Schwung, add **Concat** to a track.
2. Open **Corpus** from the root menu, select **Source** (knob 1) and press the jog wheel. Pick a sample, or a `.concat` file from `Corpora`.
3. **Scope** (on **Mod & Set**) decides what a picked sample means: **File** *(default)* makes that sample the whole corpus, cut into many slices; **Folder** takes its whole folder (and subfolders).
4. Watch **Analyze** reach 100 %. A big folder can take a minute the first time; after that it loads from the cache.
5. Play the pads. Each pad plays a different part of the corpus (Notes, Zone).
6. Open **View** to see the map and browse the factory **Recipes** with the jog wheel. **Cloud** or **Drone** are good first stops.

---

## 3. The pages

The root page holds 8 performance knobs. The menu below it opens the other pages: **View · Corpus · Map · Mod & Set · Grain · Shape · Play · Out**.

### Root (performance knobs)

| Knob | Param | Notes |
|------|-------|-------|
| K1 | X Position | Where you are on the map, across. |
| K2 | Y Position | Where you are on the map, up. |
| K3 | Density | Grains per second. |
| K4 | Grain Size | Grain length. |
| K5 | Amp Attack | Per-note attack (Notes). |
| K6 | Amp Release | Per-note release (Notes). |
| K7 | Polyphony | Grain voice ceiling. |
| K8 | Filter | Bipolar DJ filter. |

### View

The corpus map on the screen, plus the **Recipes** browser.

- Each dot is a slice. The ring is where you are (X/Y Position) and how wide grains may wander (Radius). The last played slices leave a short trail.
- The jog wheel browses **Recipes**; the name and `i/n` show at the top. A recipe sets play and sound parameters and never touches the Corpus page.
- The bottom line names the knob you touch, and shows what the analysis is doing (`analysing 3/40`, `out of memory, kept old`, `2 sources missing`).
- Knobs: X Position, Y Position, Radius, Density, Grain Size, K Nearest, Smooth, Filter.

### Corpus

| Param | Range | Default | Notes |
|-------|-------|---------|-------|
| **Source** | file browser | (auto) | A sample (`.wav .aif .aiff .flac .mp3`) or a `.concat` corpus file under `UserLibrary`. A new Concat picks the first sample of your first Samples folder. |
| **Pool Cap** | 16 to 512 MB | 128 | RAM for the corpus audio. 128 MB holds about 25 minutes of mono audio. Free RAM can lower it; then Concat keeps an even subset of the slices across the whole corpus. |
| **Rescan** | trigger | | Turn right to re-analyse from scratch, ignoring the caches. |
| **Slice Mode** | Onset / Fixed | Onset | **Onset** cuts at attacks (spectral flux); **Fixed** cuts even grains of Max Slice. |
| **Min Slice** | 20 to 500 ms | 20 | Shortest slice. |
| **Max Slice** | 100 to 2000 ms | 2000 | Longest slice (and the grain length in Fixed mode). |
| **Analyze** | 0 to 100 % | | Progress, read only. |
| **Silence Gate** | 0 to 1 | 0.02 | Slices quieter than this are dropped. |

A `.concat` file's own slicing settings replace Slice Mode, Min/Max Slice and Silence Gate.

### Map

| Param | Range | Default | Notes |
|-------|-------|---------|-------|
| **X Descriptor** | Bright, Pitch, Loud, Noisy, Rolloff, MFCC1, MFCC2, MFCC3 | Bright | X axis in Raw mode. |
| **Y Descriptor** | same | Pitch | Y axis in Raw mode. |
| **X Position** | 0 to 1 | 0.5 | |
| **Y Position** | 0 to 1 | 0.5 | |
| **Map Mode** | Raw / Learned | Raw | **Raw**: the two descriptors above. **Learned**: the two directions along which this corpus varies most (PCA), so it spreads evenly. |
| **Radius** | 0 to 1 | 0.15 | How far from the point grains may pick (variety). |
| **K Nearest** | 1 to 16 | 4 | How many nearest slices a grain chooses from. |
| **Repeat Avoid** | 0 to 16 | 4 | Skips the last N played slices. |

### Mod & Set

| Param | Range | Default | Notes |
|-------|-------|---------|-------|
| **Follow** | Manual / LFO / Sequence | Manual | **LFO** moves the point by itself. **Sequence** locks the LFO to the MIDI clock. |
| **LFO Rate** | 0.01 to 10 | 0.2 | Hz with LFO; cycles per beat with Sequence (0.25 = one bar, 1 = a quarter note, 2 = an eighth). |
| **LFO Shape** | Circle / SineX / SineY / S&H | Circle | S&H with Sequence jumps on the beat. |
| **Smooth** | 0 to 1 | 0.35 | How much each grain follows on from the previous one (continuity) rather than only the target. |
| **Pos Jitter** | 0 to 1 | 0.15 | Random start offset inside the slice per grain. |
| **Scope** | Folder / File | File | What a picked sample means: just that sample, or its whole folder. |

### Grain

| Param | Range | Default | Notes |
|-------|-------|---------|-------|
| **Density** | 1 to 100 g/s | 20 | Grains per second. |
| **Grain Size** | 10 to 2000 ms | 120 | Grains longer than their slice loop it with a crossfade. |
| **Grain Env** | Hann, Tukey, Tri, Gauss, Expo, RevExpo, Trapez, Black | Hann | Window shape. |
| **Pitch** | -24 to +24 st | 0 | Transpose. |
| **Pan Spread** | 0 to 1 | 0.3 | Random stereo position per grain. |
| **Grain Random** | 0 to 1 | 0 | Random grain length. |
| **Reverse** | 0 to 1 | 0 | Chance that a grain plays backwards. |
| **Pitch Jitter** | 0 to 1 | 0 | Random detune per grain. |

### Shape

| Param | Default | Notes |
|-------|---------|-------|
| **Amp Attack / Decay / Sustain / Release** | 0.01 / 0.2 / 1 / 0.2 | Per-note ADSR (each pad its own). In Map and Mosaic the amp stays open. |
| **Filter Env** | 0.45 | Envelope amount on the Filter (bipolar, 0.5 = none). |
| **Filter Attack / Decay / Sustain** | 0.01 / 0.3 / 0 | Filter envelope, gated by held notes in Notes mode. |

### Play

| Param | Range | Default | Notes |
|-------|-------|---------|-------|
| **Play Mode** | Map / Notes / Mosaic | Notes | See section 1. |
| **Note Match** | Off / Pitch / Zone | Zone | Notes mode. |
| **Pressure** | Off, Proximity, Density, Bright, Sat, Pitch, Grain, Jitter | Density | What pad pressure changes. |
| **Pressure Amt** | 0 to 1 | 1 | How much. |
| **Velocity Amt** | 0 to 1 | 0.8 | 0 ignores velocity; 1 lets it set the level fully. |
| **Polyphony** | 1 to 128 | 48 | Grain voice ceiling. |

### Out

| Param | Range | Default | Notes |
|-------|-------|---------|-------|
| **Filter** | 0 to 1 | 0.5 | Centre is off; left is a low-pass going down, right a high-pass going up. |
| **Resonance** | 0 to 1 | 0 | |
| **Saturation** | 0 to 1 | 0 | tanh drive with make-up gain. |
| **Dry Mix** | 0 to 1 | 0 | Live input in the mix (Mosaic). |
| **Volume** | 0 to 1 | 0.8 | |
| **Width** | 0 to 1 | 1 | Stereo width. |
| **Level Norm** | 0 to 1 | 0.5 | Pulls each slice toward the corpus's median level, so a corpus of mixed recordings sits evenly. |

---

## 4. Pads and MIDI

- **Notes:** up to 16 held notes, each with its own envelope, sharing up to 128 grain voices.
- **Map:** hold a pad to jump to one of 16 timbre clusters (cluster = note number mod 16). Last pressed wins; release to return.
- **Pressure** (aftertouch) goes to the Pressure destination; **velocity** scales level by Velocity Amt.
- **Pitch bend** transposes ±2 semitones. **MIDI clock** drives Follow = Sequence. **CC 123** releases all notes.

---

## 5. Recipes

Browse them on the **View** page with the jog wheel. They set play and sound parameters, never the corpus.

| Recipe | What it sets up |
|--------|-----------------|
| **Init** | Defaults. |
| **Cloud** | Map + slow circular LFO, dense 400 ms grains, wide stereo, smooth. |
| **Stutter** | Notes/Zone, short 40 ms grains, tight neighbourhood, no repeat avoid: rhythmic repeats. |
| **Drone** | Map + very slow LFO, 1.5 s Gauss grains, long attack and release. |
| **Scatter** | Map + S&H LFO, random sizes, detune, reverse, the whole map in play. |
| **Pitched** | Notes/Pitch: the corpus played chromatically. |
| **Mosaic** | Mosaic mode with 80 ms grains and some dry input. |
| **Sequence** | Map + clock-locked LFO (one cycle per bar). Needs the MIDI clock running. |

---

## 6. Corpus files (`.concat`)

A `.concat` file is a JSON description of a corpus. Pick it in **Source** like a sample. The Corpus Builder writes them to `UserLibrary/Corpora`; you can also write them by hand.

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

| Key | Meaning |
|-----|---------|
| `sources` | Folders and samples. Paths are relative to `/data/UserData/UserLibrary` (absolute paths work too). A plain string is a path. |
| `start`, `end` | Seconds: only that part of the sample enters the corpus. |
| `include`, `exclude` | File-name patterns for a folder (`*` and `?`, case ignored). |
| `recurse` | `false` keeps a folder to its own files. Default `true`. |
| `exclude` (top level) | Patterns applied to every folder source. |
| `descriptors.use` | Which descriptor groups Concat uses: `loud`, `centroid`, `flatness`, `zcr`, `pitch`, `mfcc`, `mfccN`, `mfccA-B`. Leaving one out removes it from the search, the clusters and the Learned map. A Raw axis set to a left-out descriptor goes flat. |
| `slicing` | `mode` (`onset`/`fixed`), `min_ms`, `max_ms`, `gate`. Replaces the Corpus page settings. |

Everything except `sources` is optional. A missing source is skipped and counted, never fatal.

**Live reload.** When the loaded file changes (saved from the Builder, or edited by hand), Concat rebuilds it in the background while the old corpus keeps playing. Only new or changed files are analysed again. A broken file keeps the old corpus and the View page says why.

---

## 7. The Corpus Builder

A web page that runs on your Move, served by schwung-manager. Open it in a desktop browser on the same network (or USB):

```
http://move.local:7700/api/remote-ui/module-assets/concat/builder/app.html
```

It is also linked from Concat's panel in the manager's **Remote UI**.

### Building a corpus

- **Library** (left): your UserLibrary. Drag folders or samples onto the corpus column, or use **+**. Ctrl/Shift-click selects several, **Ctrl+A** selects every sample in the folder; drag any of them or press **Add**. **↑/↓** steps through samples and plays each one.
- **Corpus sources** (middle): click a source to edit it.
  - A **sample** shows its waveform. Drag to keep only part of it; click a slice to hear that slice.
  - A **folder** arrives collapsed. Open it (**▸**) to see its samples, play them one by one, leave some out (**✕**, and **+** to bring them back). **Only / Skip** take name patterns. **Trim on its own** pulls one sample out of the folder so you can trim it.
- **Slicing** (bottom): onset or fixed, min/max length, gate. Saved into the file.
- **Play**: ▶ becomes ⏸ while a sound plays; ⏸ pauses, ▶ resumes, Stop or Esc goes back to the start. Space plays or pauses the selected sample.

### Seeing and hearing the map

- **Map** tab: every slice as a dot. **Learned** (3-D PCA; **Move view** flattens it to the plane Concat uses) or **Raw** (Concat's X/Y descriptor menu).
- **Colour** by source, by a descriptor, or by **Pad clusters** (Concat's 16 clusters, with a pad grid: hold a pad to hear what it reaches in Map mode).
- Click a dot to hear the slice. Click a source and its slices light up.
- **Play the map** (button, **P**, or Shift+drag): press and move to play it like Concat's Map mode, with **Density**, **Grain** and **Spread** (K Nearest).
- **● Rec** (or **R**) records what you play. Save the take to the Move (`UserLibrary/Recordings/Corpus Builder`) and add it straight back into the corpus.
- **Descriptors** tab: min, median, IQR and a histogram for each of the 36 descriptors, a checkbox per group, and each one's share of the map.
- The summary shows slices, files, length, and the pool size against Concat's Pool Cap.

### Saving and loading

- **Save** writes `UserLibrary/Corpora/<name>.concat` (Ctrl+S). **Open** loads one back. Unsaved work survives a page reload.
- The header shows which track Concat is on and what it plays, with live progress while it analyses.
- **Load on track N** saves and makes Concat on that track play the corpus. After that, every Save reloads it on the Move.

The page runs Concat's own decoder, slicer and descriptor code compiled to WebAssembly. It shows what the Move will compute: same slice boundaries, descriptors within 0.06 % on real files, pad clusters within a couple of slices.

---

## 8. Remote UI panel

In schwung-manager's Remote UI, Concat's track shows a small panel instead of the automatic knob list: the corpus playing and its status, a menu of your corpus files to load, and **Open the Corpus Builder**. Switch **Interface** to **Default** at the top of the track to get every knob (the manager forgets that choice when the page reloads).

---

## 9. Memory and caches

- Pool Cap is checked against free RAM at every load, keeping 350 MB for the Move. If the corpus is bigger than what fits, Concat keeps an even subset of slices across the whole corpus instead of the first files.
- Long files are decoded in 60 s windows, so file length does not matter for memory.
- Analysis is cached in `/data/UserData/schwung/concat-cache`: a whole-corpus cache (instant reload) and a per-file cache (edits re-analyse only what changed). Nothing cleans it yet; delete the folder to reclaim space, it rebuilds as needed.

---

## 10. Troubleshooting

- **The View page says `no audio files`:** the pick has nothing Concat can read. Pick a sample (not a folder) in Source.
- **`out of memory, kept old`:** the corpus did not fit the free RAM. Lower Pool Cap or trim the corpus.
- **`bad JSON: ...`:** the `.concat` file has a mistake; the old corpus keeps playing. Save it again from the Builder, or fix the file.
- **No sound:** check Play Mode and Volume, that Analyze reached 100 %, and that a pad is held (Notes) or the point sits where there are dots (Map).
- **Source shows a plain number:** the module metadata did not load. Power-cycle the Move.
- **The Builder shows `Concat is not on a track`:** add Concat to a track; the badge updates by itself.
- **Updating Concat:** power-cycle the Move after installing (Schwung caches the manifest at startup).
