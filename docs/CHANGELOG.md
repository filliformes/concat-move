# Changelog

## v0.2.0 (2026-10-20)

Corpus files, the Corpus Builder web page, the Remote UI panel, and a reliability pass on the engine. Requires Schwung 1.3.0 or later; tested on 1.5.0.

### Playing
- **View** page: the corpus map on the screen (slices, your position and radius, a trail of played slices) with the **Recipes** browser on the jog wheel: Init, Cloud, Stutter, Drone, Scatter, Pitched, Mosaic, Sequence.
- **Scope** (Folder / File): one sample can be the whole corpus. Default File.
- **Smooth** (grain-to-grain continuity), **Pos Jitter** (random start inside a slice), **Level Norm** (evens out slices from different recordings).
- Grains that loop their slice cross the seam with an equal-power fade.
- A new Concat loads a first corpus by itself (the first sample of your first Samples folder).
- Saturation is audible again.
- Pages folded into **Mod & Set**; full parameter names (needs Schwung 1.3.0).

### Corpus files
- `.concat` files: several folders and samples, start/end per sample, include/skip patterns, slicing settings, and the descriptors in use. Pick one in Source.
- Live reload: an edited file rebuilds in the background while the old corpus plays. A broken file keeps the old corpus and the View page says why.
- Per-file analysis cache: after an edit only new or changed files are analysed (a 9170-slice corpus went live in 8.6 s instead of 76 s).
- AIFF support. Folder contents load in name order, the same on every device.

### Corpus Builder
- Web page served from the Move by schwung-manager: library browser, drag and drop, Ctrl+A, audition with play/pause, folders that open into their samples, trimming on a waveform, slice audition.
- Descriptor statistics and selection; 3-D Learned map and Concat's Raw X/Y map; colour by source, descriptor or pad cluster.
- Play the map with Concat's grain rule; hold a pad in the pad grid to hear a cluster; record takes and save them to the Move.
- Sees which track Concat is on; Load on track N with live progress.
- Runs Concat's own analysis code compiled to WebAssembly: same slice boundaries as the Move.

### Remote UI
- Concat's panel in schwung-manager's Remote UI: corpus playing and status, load a corpus file, open the Builder.

### Engine
- Switching modules during an analysis took up to 72 s; now a few ms (cancellable analysis, bounded teardown).
- Big corpora: worst-case CPU per block on 25,000 slices went from 19 ms to under 2 ms (searches only over the descriptors in use, cached candidates, budgeted search).
- Long files are decoded in 60 s windows instead of whole.
- A failed analysis keeps the playing corpus; status line on the View page.
- Pool Cap default 128 MB (was 512).
- Settings saved as JSON (sets from 0.1.0 still load).
- Status files in `concat-cache/status` for the Builder and the Remote UI panel.

## v0.1.0 (2026-09)

First release: onset slicing and 36 descriptors per slice, RAM pool with an OOM guard, kNN grain cloud with repeat avoid and concatenation cost, Notes / Map / Mosaic play modes, PCA Learned map, k-means pad clusters, LFO and clock-synced Sequence, per-note envelopes, DJ filter, saturation, on-disk index cache.
