# Concat audit, part B: research, papers, competitors, recommendations

Scope: read-only review of `C:/Users/filli/Projects/concat-move` (HEAD b57ca11 plus uncommitted Level Norm / Pos Jitter work), five local DAFx papers, and a web survey of the concatenative / corpus-instrument landscape. Goal from the author: "the best, most usable and good-sounding concatenative synthesizer for open platforms".

Paper 1 (the design-phase PDF) is **Schwarz, Beller, Verbrugghe, Britton, "Real-Time Corpus-Based Concatenative Synthesis with CataRT", DAFx-06, Montreal** (pp. 279-282). Note: paper 2 is labelled DAFx21 by the caller; its header reads DAFx20in21 (Vienna, 2021), pp. 238-244.

CPU figures below marked "est." are back-of-envelope estimates for a Cortex-A72 at 1.5 GHz, scalar code. They are not measurements.

---

## 1. Design intent vs implemented

Legend: DONE / PARTIAL / MISSING / DIVERGED (implemented differently from spec) / AHEAD (v2 item already shipped).

| Area | Spec / CLAUDE.md intent | State | Notes (file refs) |
|---|---|---|---|
| Plugin shell | sound_generator, plugin_api_v2, 8-field struct, get_error NULL, -1 for unknown keys, DSP-served ui_hierarchy + chain_params | DONE | `concat.c:1111-1138`, `get_param` returns -1 at end. |
| Three threads (render / analyst / loader) | Separate analyst and loader, atomic swap | DIVERGED (fine) | One analyst thread does decode+slice+describe+pool build, then `__atomic_exchange_n` swap (`concat.c:461-506`). Old index freed after `usleep(60000)`, grains flushed on swap (`render_block` line 929). Works; the 60 ms grace is a heuristic, not a handshake. |
| Worker scheduling | SCHED_OTHER, cores 0-2 | DONE | `analyst_thread` lines 463-466. |
| create_instance does nothing heavy | arm workers only | DONE | Auto-pick default corpus runs on the analyst (`auto_pick_corpus`). |
| Ingest | dr_libs WAV/FLAC/MP3, mono, 44.1k cubic resample, int16 | DONE | `ingest.c` Hermite resample; files >128 MB skipped. |
| Corpus control | File Browser `corpus_path` (folder) primary + `quick_pick` dynamic enum | DIVERGED / PARTIAL | Primary is `corpus_file` (a file) + `src_scope` Folder/File. `quick_pick` enum (corpora/ + recordings/) is MISSING. `corpus_path` still served read-only. |
| Pool cap | default 128 MB, max 512, guard `min(pool, MemAvail-350)`, floor 32 | DIVERGED | Code default is **512 MB** (`PARAMS[P_POOL_MB]`, module.json line 330); README says 128. Guard implemented in `arm_analyst`. Two Concat instances armed together can each pass the guard (race on MemAvailable). |
| OOM behaviour | evict least-used slices' audio, keep index | DIVERGED | Implemented as stride decimation at build time (`corpus_engine.c:383-440`): a representative subset, index of dropped slices is not kept. No usage-based eviction. Reasonable v1 substitute. |
| Index cache | under module dir, hash(path)+mtimes, rescan changed files only | PARTIAL | Cache at `/data/UserData/schwung/concat-cache` (better than module dir). Signature is all-or-nothing: any changed file triggers a full re-analysis, not incremental. |
| Slicing | spectral-flux onset, adaptive, clamp 60/400 ms, ~20 ms floor, fixed-grain fallback | PARTIAL / DIVERGED | Flux + median subtract + local mean+std threshold + debounce: DONE (`corpus.c:83-153`). Code defaults are **20 / 2000 ms** (README says 60 / 400). Onset position is quantised to the analysis hop (512 samples, 11.6 ms) at frame *start*, so slice starts can land up to ~23 ms before the true transient. No sample-accurate refinement, no zero-crossing snap. |
| Silence gate | drop low-loudness slices | DONE | `silence_gate` RMS floor. |
| Descriptors | 36-d MultiStats: MFCC1-13, centroid, flatness, ZCR mean+std, loud mean/peak, YIN pitch+conf | DONE with caveats | Layout `corpus.h`. MFCC index 0 is energy (duplicates loudness). Centroid and pitch are linear Hz, loudness linear RMS, flatness a ratio: not perceptual scales. YIN window W=512 caps lag at 511 samples, so the effective pitch floor is ~86 Hz, not the 50 Hz asked for (`analysis.c:168-172`). Aperiodicity threshold 0.6 is lenient. "Rolloff" nav option is actually ZCR (`desc_index` case 4). |
| FFT | kissfft (BSD-3), locked | DIVERGED | In-house radix-2 (`analysis.c:17`). README acknowledges. License-clean, fine; CLAUDE.md still says kissfft. |
| Normalisation | RobustScaling for query, min-max for nav axes | DONE | `concat_robust_fit`, `query_init` nav min/max. |
| Query | linear scan, weighted Euclidean kNN, random among K within radius, repeat-avoid ring | DONE / DIVERGED semantics | `query.c`. "Radius" is a fraction of K (pool 1..K, K<=16), not a descriptor-space distance as in CataRT. With 10k+ slices, Radius=1 still means "16 nearest", so the map cannot be sprayed widely. Full 36-d distance computed even when only 2 dims carry weight. |
| Concat cost | v2 | AHEAD | "Smooth" blends target cost with full-space distance to last slice (`query_select`). |
| k-means pads K=16 | pads jump to clusters | DONE | `corpus_kmeans`; pad -> `last_pad_note % K`; cluster order arbitrary; 32 pads map to 16 clusters twice. |
| Learned 2-D map | v2 (UMAP/PCA) | AHEAD | PCA by power iteration + Gram-Schmidt (`corpus_pca`). Query in Learned mode reconstructs a 36-d point on the PCA plane and searches full space, so what plays is not exactly what the View shows under the cursor. |
| OLED map | v2 overtake | AHEAD | 1.3.0 `as_page` canvas (`canvas.js`) with scatter, cursor, radius, played trail. |
| Grain engine | 24-voice steal-oldest, density g/s 1-100, size 10-500 ms, grain-locked env | DONE / extended | 128 voices, cap `poly` default 48, steal nearest-end (better). Size to 2000 ms with looped slice + 20 ms equal-power seam. 8 windows via tables + 32-sample hard declick. Linear interpolation. Grains start on block boundaries (no sub-block offset). Mix hard-clips to int16 inside `grain_mix` before filter/sat. Linear pan law (centre -6 dB per side). |
| Level Norm / Pos Jitter | not in spec | DONE (uncommitted) | `grain.c:105-158`. Good additions. |
| Play modes Map / Notes / Mosaic | yes | DONE | Notes: 16-note poly, per-note ADSR, live env pointer. Mosaic: see below. |
| Note Match Off/Pitch/Zone | yes | DONE / PARTIAL | Pitch uses linear-Hz distance and ignores pitch confidence. Zone maps note to X only: `(note-36)/60`, so 32 pads cover about half of X and Y is the knob. |
| Mosaic target | line/mic via audio_in_offset, onset or synced `mosaic_rate` | PARTIAL | Reads line-in, 1024-sample non-overlapping frames (23 ms), mean-only target, MFCC+centroid+flatness+ZCR+loud weights. `input_src` (Line/Mic) and `mosaic_rate` MISSING. No onset triggering, no input calibration, no loudness following (silence in still produces grains at full density). FFT+MFCC runs on the render thread every 8th block (spiky). |
| Follow Manual / LFO / Sequence | yes | DONE | Sequence = tempo-locked LFO from 24-PPQN clock; not a step sequencer. |
| MIDI | note, vel, bend, CC1, AT, CC123, clock | PARTIAL | CC1 stored but unused ("TODO"). Poly aftertouch collapses into one global value (last pad wins). |
| Filter / env / sat / width / dry | yes | DONE | Global TPT SVF DJ filter, global filter env, tanh sat with mid-level makeup. |
| Factory recipes | not in spec | AHEAD | 8 recipes via preset browser triple. |
| Persisted state | all params + corpus | DONE | key=value lines. |
| Live/arena corpus growth, Move-track mosaic | v2 | MISSING (as planned) | |
| Docs vs code defaults | | DIVERGED | README table disagrees with code on pool (128 vs 512), min/max slice (60/400 vs 20/2000), Scope (Folder vs File), Pressure (Proximity vs Density), Vel Amt (1.0 vs 0.8), Filt Env (0.5 vs 0.45). design-spec.md "Status: Pre-implementation" is stale. |

---

## 2. Per-paper takeaways mapped to Concat

### P1. Schwarz et al., CataRT, DAFx-06
| Idea (section) | Verdict | Concat implementation idea | CM4 cost |
|---|---|---|---|
| Radius is a *distance* around the target; pick randomly among units with d < r (s3.3) | ADAPT | Make Radius a distance in the normalised nav/query space. One linear pass with reservoir sampling of up to 16 candidates inside r, then apply Smooth weighting among them. Keep K as a "max candidates" cap. Fixes "Radius 1.0 is still local" on big corpora. | Same scan cost as now. |
| Trigger modes bow / fence / beat / chain (s3.5) | ADOPT | New `trigger` enum: Cloud (current density), Beat (grain per clock division), Fence (fire when the nearest unit changes: ideal for encoder sweeps and LFO), Chain (next grain when previous ends: non-overlapping concatenation). | Negligible. |
| Unit transformation to match target pitch/loudness (s3.4) | ADOPT | Per-grain retune to a key/scale (slice pitch is known) and loudness matching to target (Level Norm already half of this). | Negligible. |
| Descriptor sequencer + per-descriptor weight automation (s3.5, fig. 2) | ADAPT | Expose descriptor weights as host-LFO targets; Sequence follow could step through stored target points. | Negligible. |
| Duration, source-file id as descriptors (s3.1) | ADOPT | Add duration and file index to the slice record; enables "stay in this file" / filter by file. | Tiny RAM. |
| Uneven distribution; zoom into clusters (s5) | ADOPT | Uniformise the map (see P-extra Lallemand & Schwarz); "zoom" knob that rescales the nav window around the cursor. | Offline, cheap. |
| Markov transitions between clusters to recreate source texture (s5) | ADAPT | "Continuity" knob: probability of playing the slice that followed the last one in its source file (slices are stored in file order, so next = idx+1 if same file). Cheap CatOracle-lite. | Negligible. |
| PCA-based kd-tree index (s3.3) | NOT NOW | Linear scan is fine if the render-side cost is fixed (section 4, R1). | |

### P2. Sanfilippo & Parker, DAFx20in21: ZC + first-order analysis + Lagrange junctions
| Idea | Verdict | Concat implementation idea | CM4 cost |
|---|---|---|---|
| Grains start/end at zero crossings with matching slope polarity (s2.1-2.4) | ADOPT (offline) | At analysis, snap every slice start/end to the nearest upward zero crossing within +-2 ms; store slope sign. Cleaner edges for the Chain trigger mode and for short grains. | Offline, trivial. |
| Select next grain with matching first-derivative polarity at the junction (s2.2) | ADAPT | In Chain mode, add a tiny junction term (sign match of end/start slope, stored per slice) to the Smooth cost. | 2 bytes/slice. |
| Lagrange junction reconstruction instead of crossfade for non-overlapping grains (s2.5) | ADAPT (Chain mode only) | 3rd-order Lagrange over a 16-32 sample junction when a chained grain replaces the previous one; avoids the short-crossfade derivative kink. | Per junction only; negligible. |
| Higher-order interpolation of fractional reads (s1.2, fig. 4: linear vs 3rd-order) | ADOPT | Replace linear read in `grain_mix` with 4-point Hermite/Lagrange. Reduces aliasing and HF noise on transposed grains. | est. 1.5-2x the per-voice inner loop; fine at 48 voices. |
| Rectangular non-overlapping granulation as a mode | ADAPT | This is exactly CataRT "chain": a tight, dry, rhythmic mosaic sound that current overlapping clouds cannot produce. | Negligible. |

### P3. Abate & Hansen, DAFx23: Feature-Based Delay Line
| Idea | Verdict | Concat implementation idea | CM4 cost |
|---|---|---|---|
| Live buffer as corpus, segmented and analysed as it is written (s2) | ADAPT (v2) | "Capture" first (record line-in to a WAV in recordings/ on the worker, auto-analyse), then the bounded-arena live corpus the spec already plans. A delay-like "Echo" mode (target = the segment at now - T) is a natural later extension. | Capture: disk write on worker; arena: M-L. |
| Feature weights via Hadamard product (s4.1) | ADOPT | A "Match" page of macro weights (Timbre=MFCC means, Morph=stds, Bright, Noise, Loud, Pitch). Pre-multiply target and use `w` already in `wdist2`. | Negligible. |
| Target smoothing = mean of last N target frames (s4.2) | ADOPT | Mosaic "Glide/Smooth In" knob: running average of the live target vector. Kills jittery re-selection on sustained input. | Negligible. |
| Tukey window with adjustable flat-top width; overlap = taper (s5.2.2) | ADOPT | Turn Tukey's fixed 25% taper into a "Shape" continuous knob (0 = Hann, 1 = near-rect). With Chain/Beat modes set overlap = taper for near constant power. | Table rebuild off-thread or 16 pre-baked tables. |
| Feature impact depends on corpus variance (s5.1.1) | ADOPT | Auto-weight: down-weight dims whose corpus IQR is tiny relative to others, or show which axes carry variance (PCA loadings) on the View. | Offline. |
| Per-segment parameter mapping, e.g. flatness -> gain for de-essing, feature -> pan/pitch (s5.2.5, s7.2) | ADOPT | Per-grain mappings: centroid -> pan (timbre spatialisation), noisiness -> gain (a "Tonal/Noisy balance" knob), pitch -> key-lock. | Negligible (per spawn). |
| Offset overlapping segments to avoid constructive stacking of identical copies (s5.2.3) | DONE | Pos Jitter + repeat-avoid already cover this. | |

### P4. Abate & Hansen, DAFx26: Concatenation-Driven Convolution
| Idea | Verdict | Concat implementation idea | CM4 cost |
|---|---|---|---|
| SOM map, 2-D lattice with smooth feature gradients (s4.1.2, table 1) | ADAPT | Offline SOM as a third Map Mode ("Grid") at a Move-friendly resolution (e.g. 32x16 nodes). Lattice cells map 1:1 to OLED pixels and 8x4 pads (4x4 nodes per pad). Coalescence proves the UX. | est. N=10k, 512 nodes, 36 d, 20 epochs: ~3.7 G MAC, ~5-10 s once on the analyst; cached. |
| Gestures: drawn paths and regions traversed by input features, fixed rate or LFO (s4.2) | ADAPT | "Path" follow mode: record a knob/pad gesture into a short loop of nav points, replay at clock rate. Region = pad-held radius. | Tiny RAM. |
| Tukey OLA with overlap = taper for constant power (s4.3) | ADOPT | Same as P3 Shape knob. | |
| Live vs baked IR, exponential decay envelope on corpus stream (s4.4-4.5) | NOT NOW | A "Convolve" Mosaic variant (line-in convolved with the current grain stream as IR) is a genuine new instrument but a large project; a 4-8k-tap uniformly partitioned convolver is feasible on core 3's budget but competes with 48 grains. Park for v3. | L. |
| Frequency-domain kernel interpolation, single engine (s5.2) | N/A | Only relevant if Convolve mode is built. | |
| M/S width + per-grain pan (s4.5) | DONE | Width + Pan Spread exist. | |

### P5. Schwarz, Roebel, Yeh, LaBurthe, DAFx-16: concatenative texture synthesis evaluation
| Idea | Verdict | Concat implementation idea | CM4 cost |
|---|---|---|---|
| CSDC: random walk, next grain chosen among timbrally closest to the *last played* grain (s2.1) | ADOPT | "Walk" follow mode: target = descriptors of the last played slice (+ small drift toward the knob point). Self-playing textures that never jump far but traverse the whole corpus. Smooth=1 approximates it today but the target stays fixed. | Negligible. |
| CSDC descriptor set: loudness, f0, noisiness, centroid, spread, slope; validated better than MFCC distance for texture similarity (s2.1, ref. Schwarz & O'Leary SMC 2015) | ADOPT | Add spectral spread and slope (free from the magnitude spectrum). Offer a "Match" preset that weights these 6 over MFCCs; 26 of 36 dims are MFCC today, so timbre distance is MFCC-dominated. | Offline, trivial; +4 dims. |
| CSDC grains 600-1000 ms random, +-200 ms random start, 200 ms equal-power overlap (s2.1) | ADOPT | A "Texture" recipe with those values (Grain Rand, Pos Jitter, Gauss/Hann). Sets a validated baseline for ambience corpora. | None. |
| AudioTexture: segment at energy minima between low-passed (20 Hz biquad) energy maxima, min event duration (s2.3) | ADOPT | Third Slice mode "Event": boundaries at energy minima, not onset frame starts. Good for textures, speech, field recordings; gives natural fades at unit edges. | Offline, trivial. |
| WSOLA-refined crossfade position (s2.3, ref. 21) | ADOPT | At analysis, choose each slice's loop seam offset by maximising head/tail cross-correlation over +-10 ms; store per slice. Improves long looped grains (drones) where the fixed 20 ms seam still flams. | Offline, cheap. |
| Equal-power crossfade | DONE | Loop seam (`grain.c:205-209`). Grain-to-grain overlap sum is still linear-pan, uncompensated. | |
| Listening test: RND baseline nearly as good for stable textures; descriptor control matters for *varying* sounds (s3.3) | NOTE | Value of Smooth/Walk is largest on evolving material; do not over-invest in selection finesse for static drones, invest in edges and levels. | |

### Extra sources that change recommendations
- Lallemand & Schwarz, "Interaction-optimized Sound Database Representation", DAFx-11 (https://hal.science/hal-01161307): uniformise point density so dense clusters become playable. Cheapest version is a per-axis rank (quantile) transform of the PCA coordinates.
- FluCoMa `fluid.grid` (https://learn.flucoma.org/reference/grid/): Jonker-Volgenant assignment of 2-D points to a regular grid, explicitly for grid controllers. Direct template for mapping 32 Move pads.
- Schwarz, "Interacting with a Corpus of Sounds", eContact 16.2 (https://econtact.ca/16_2/schwarz_corpus.html) and NIME'12 (https://nime.org/proc/schwarz2012/): continuous vs instrumental modes; uneven distribution wastes controller surface; recommends distribution algorithms and smaller homogeneous corpora.

---

## 3. Competitive landscape

| Product | Platform | Key strengths | What Concat lacks vs it | What Concat already does better |
|---|---|---|---|---|
| CataRT / MuBu (IRCAM) | Max | Reference design: distance radius, bow/fence/beat/chain triggers, descriptor sequencer, many descriptors, live-recorded corpus, kd-tree | Trigger modes, distance radius, descriptor range filtering, live corpus | Standalone hardware, no computer; RobustScaling; built-in concat cost; one-knob recipes |
| SKataRT (IRCAM, M4L) (https://forum.ircam.fr/projects/detail/skatart/) | Ableton Live | XY or live-input mosaicing **with calibration**, gesture/loop/beat-sync triggers, per-grain filter/transpose/gain randomisation, 8 sub-corpora with own vol/pitch/pan, descriptor weights | Input calibration, beat-sync trigger, sub-corpora layering, descriptor weight UI | Runs on Move, PCA map, pad clusters, poly Notes mode with per-note envelopes |
| FluCoMa (Max/SC/Pd) (https://www.flucoma.org/) | Desktop toolkits | NoveltySlice / AmpSlice / TransientSlice, MultiStats, UMAP/MDS/PCA, KDTree, grid assignment, NMF | Better slicers (novelty, transient), grid assignment, UMAP | It is a toolkit, Concat is an instrument: zero patching |
| SP-Tools (Constanzo) (https://rodrigoconstanzo.com/sp-tools/) | Max / M4L | Very low-latency onset detection, onset-based **and** continuous matching, loudness compensation, descriptor filtering, drum-trigger focus | Onset-triggered mosaic, loudness compensation, ~10 ms analysis windows | Self-contained on hardware; corpus from any folder with caching |
| DataMind Concatenator (https://datamindaudio.ai/concatenator-v1/) | VST/AU/AAX, $149 | Polished, <50 ms latency, "variation / particles / stickiness", 5 LFOs, envelope followers, presets that adapt to any corpus. Users praise drum results; **complain about pitch tracking, CPU cost, closed controls** (KVR thread https://www.kvraudio.com/forum/viewtopic.php?t=616585&start=15) | Envelope follower mapping, "stickiness" (source continuity), polished mosaic | Open source, pitch-aware Notes mode, user-visible selection controls (Radius/K/Smooth/Repeat) that Concatenator users ask for |
| Coalescence (Dillon Bastan, M4L) (https://www.musicradar.com/news/dillon-bastan-coalescence-m4l) | Ableton Live | SOM grid of up to 2500 slices; Point / Rings / Paths modes mapped to MIDI notes; live input matching; 2 LFOs, 2 envs, ladder + formant filters | Grid map with pad-shaped regions, Rings (per-note radius) and Paths (per-note trajectories) | Corpus size (thousands of slices, 512 MB pool), folder-scale ingest, standalone |
| Echobit Mosaic (https://www.kvraudio.com/forum/viewtopic.php?t=499708) | VST/AU | Cheap, light CPU, weights input amplitude/pitch/timbre | Nothing significant | Grain length control (users complained Mosaic lacks it), maintained |
| AudioStellar (https://audiostellar.xyz, NIME) | Desktop, open source | AI 2-D map of a folder, units: explorer, particles (autonomous agents), sequence constellations, OSC/MIDI | Agent/particle traversal, map-drawn sequences | Grain-level playback of slices, mosaicing, runs on a groovebox |
| AudioGuide (Hackbarth) (https://github.com/benhackbarth/audioguide) | Offline Python | Target-file resynthesis with superimposition (several simultaneous units via subtractive spectral matching), temporal envelope matching | Superimposition (multi-unit per target frame) | Real-time, playable |
| CatOracle (IRCAM) | Max | Audio Oracle improvisation over a corpus | Oracle-style continuation | n/a (research tool) |
| Sononym (https://www.sononym.net/) | Desktop browser | Similarity search with an "Aspects dial" (spectrum, timbre, pitch, amplitude) | User-weightable similarity aspects | It plays; Sononym only browses |
| XLN XO / Ableton Live 12 similarity | Desktop | t-SNE drum space with 15-item similarity list; Live 12 Similar Sample Swapping | A "swap to neighbour" gesture | Real-time synthesis rather than sample selection |
| Hardware / open platforms (norns, Organelle, Bela, Daisy) | Embedded | Many granular scripts (Glut/Easygrain/Pappus on norns) | none found | I found **no maintained descriptor-space concatenative instrument** on norns, Organelle, Bela or Daisy. Concat appears to be the only one on an open hardware platform, which is its strongest positioning claim. (Search evidence is negative, so treat as "none found", not proof.) |

---

## 4. Ranked recommendations

Effort: S = under a day, M = 1-3 days, L = a week+. Impact: H/M/L. All are feasible on the CM4 unless noted.

### Tier 1 (do first)

**R1. Make the render-thread query cheap and bounded.** Performance, M, H.
Today every grain spawn scans all N slices x 36 dims plus a ring check per slice. With 20k slices a scan is est. ~0.5-1 ms. Notes mode with 16 held pads x (20 g/s + pressure-to-Density up to +80) reaches ~1600 spawns/s, i.e. ~4-5 scans per 2.9 ms block: a likely xrun. Fixes, in order of payoff:
(a) cache the top-32 candidates per voice keyed by the target vector; rescan only when the target moved by more than epsilon or every ~20 ms, sample from the cache otherwise; (b) weight-masked dims: build the list of nonzero-weight dims once per query (Raw map uses 2 of 36); (c) Learned mode: precompute each slice's 2-D PCA (or grid) coordinates at load and query in 2-D, which also makes what plays match what the View draws; (d) replace `in_ring` with a per-slice "last played stamp" array; (e) cap scans per block and defer the rest to the next block. Source: CataRT s3.3 (efficiency concern), FBDL s5.2.3 (spread queries across callbacks).

**R2. Clean output stage.** Sound, S, H.
Mix grains into a float bus, apply the filter and tanh after, and clip to int16 once at the very end. Today `grain_mix` hard-clips to int16 before the filter/saturation, so dense clouds clip hard and the saturator never sees the overshoot. Add overlap-compensated gain `~1/sqrt(density x grain_len)` (a "Auto Level" toggle), an equal-power pan law, and a gentle output limiter. Add a sub-block start offset per grain so grains do not all start on the same sample (2.9 ms quantisation causes flams/phasing at high density). Source: Schwarz DAFx-16 s2.1 (equal-power overlap), CDC s4.3 (constant-power OLA).

**R3. Sample-accurate slice boundaries.** Sound, S-M, H.
After spectral-flux peak picking, refine each onset within [-512, +1024] samples using a 64-sample RMS envelope (first point above ~50% of the local rise), back off 1-3 ms, then snap to the nearest zero crossing with positive slope (store slope sign). Add an "Event" slice mode (AudioTexture energy-minimum boundaries). Bump `CACHE_VER`. Tighter attacks in Notes/Zone and much better percussive mosaics. Sources: Sanfilippo & Parker s2.1-2.3; Schwarz et al. DAFx-16 s2.3; FluCoMa AmpSlice/TransientSlice.

**R4. Mosaic mode that actually follows the performer.** Musical, M, H.
(a) Calibration: a "Learn Input" action records ~5-10 s of input descriptors, fits input median/IQR, then maps input descriptors onto the corpus distribution (robust re-scale per dim) so a voice drives a drum corpus sensibly (SKataRT has exactly this). (b) Loudness following: output gain = input RMS envelope (with a Gate threshold), silence in = silence out (SP-Tools loudness compensation, Concatenator envelope followers). (c) Onset trigger: time-domain onset detector on the input (HFC or energy flux on 128/256-sample hops) fires a grain at each onset: CataRT "fence", SP-Tools onset matching. Keep continuous density as the alternative (`mosaic_rate` = Onset / Cloud / clock divisions). (d) Smaller analysis hop (1024 window, 256 hop) and FBDL target smoothing. (e) Add `input_src` Line/Mic. Move the Mosaic FFT so it is spread over blocks (one partial stage per block) to avoid the every-8th-block spike.

**R5. Pads as a real 8x4 map.** Usability, S-M, H.
Map the 32 pads to an 8x4 grid over the (uniformised) 2-D map, in Map mode and Note Match = Zone. Assign cells with a small linear-assignment (Jonker-Volgenant, FluCoMa `fluid.grid`) or simply grid-bin the rank-normalised PCA plane; precompute a cell centroid target per pad. Order k-means clusters by map position if clusters stay. Today Zone uses X only and covers about half of X; Map-mode pads hit 16 arbitrary clusters twice. Velocity -> grain gain, pressure -> radius per pad. Sources: fluid.grid; Coalescence Rings; Schwarz NIME'12 on controller surface.

**R6. Uniform, perceptual map axes.** Usability/sound, S, H.
Analyse centroid and pitch in log-Hz (MIDI), loudness in dB, flatness in dB (FluCoMa SpectralShape options, dsp-notes s2). Then rank/quantile-normalise each nav axis (Raw and PCA) so encoder travel is spread evenly across the corpus instead of crowding one corner. Optional stronger version: Lallemand & Schwarz density uniformisation. Fix the "Rolloff" label (compute true rolloff or rename to ZCR). Bump cache version.

### Tier 2

**R7. Trigger and traversal modes.** Musical, M, H.
Add `trigger` = Cloud / Beat (grain per clock division, quantised to MIDI clock: the CataRT "data-driven drumbox" s4.4, RhythmCAT) / Fence (fire on nearest-unit change) / Chain (butt-joined, non-overlapping, ZC-snapped with slope-matched junction per P2). Add Follow = Walk (CSDC random walk, P5 s2.1) and a "Continuity" knob (probability of playing the next slice of the same source file: Markov/CatOracle-lite, DataMind's "stickiness"). Cheap and transformative on a groovebox.

**R8. Match page: descriptor weights and filters.** Usability, S, M-H.
Six macro weights (Timbre, Morph, Bright, Noise, Loud, Pitch) feeding `w[]`, plus spread and slope descriptors (P5 descriptor set). Add range filters: Pitched-only (conf > t), Noisy-only, Loud-only, File lock. Sources: FBDL s4.1, SKataRT descriptor weights, Sononym Aspects dial, CataRT s3.5 range limits.

**R9. Pitch quality and key lock.** Sound/musical, S, M-H.
Hermite interpolation (P2 fig. 4). YIN: use a 2048 window or downsample-by-2 path so the floor reaches ~50 Hz; exclude low-confidence frames from the median; use MIDI distance in Pitch match and weight by confidence. New "Key" + "Scale" params: in Map/Mosaic, retune each voiced grain to the nearest scale degree (CataRT s3.4 target-matched transposition). This answers the main complaint users have about DataMind Concatenator (unusable on pitched material).

**R10. On-device Capture.** Usability, M, H.
Record line-in (and later a resample of Concat's own output) into `/data/UserData/UserLibrary/Recordings/concat-<ts>.wav` on the worker via a lock-free ring, then auto-analyse it with Scope=File. This delivers the "live corpus" experience (CataRT s3.1, FBDL) without the v2 growable arena, and removes the computer from the workflow entirely.

**R11. Radius as a distance.** Musical, S, M.
Reservoir-sample candidates within distance r in the query space (P1 s3.3), keep K as a cap. Makes Radius=1 mean "anywhere", restoring the Scatter recipe's intent.

### Tier 3

**R12. Better joins for long grains and Mosaic.** Sound, M, M. Store per-slice head/tail mini-descriptors (MFCC1-5 + loudness of first/last frame) and use tail(prev) vs head(cand) as the Smooth cost instead of whole-slice distance (true CataRT/Caterpillar concatenation cost, P1 s2). Add WSOLA-chosen loop seam offsets per slice (P5 s2.3).

**R13. Per-grain descriptor mappings.** Musical, S, M. Centroid -> pan (timbre spatialisation), noisiness -> gain ("Tonal/Noisy" balance), loudness -> filter. FBDL s5.2.5; Schwarz et al., "Spatializing Timbre with Corpus-Based Concatenative Synthesis", ICMC 2010.

**R14. Tukey "Shape" knob and Rings/Paths.** Musical, S-M, M. Continuous window flatness (FBDL s5.2.2, CDC s4.3); per-note Rings (radius per pad) and recorded Paths (CDC s4.2, Coalescence).

**R15. SOM map mode.** Usability, M, M. Offline SOM lattice sized to OLED/pads (CDC s4.1.2). Only after R6; rank-normalised PCA may be enough.

**R16. Expression plumbing.** Musical, S, M. Per-note poly aftertouch (store AT per note voice, not global), CC1 wired (Radius or Density per spec), per-note filter later.

**R17. Hygiene.** S, M. Sync README/module.json/code defaults (pool 128 vs 512 is also an OOM-safety question: two instances at 512 MB each); update design-spec status and CLAUDE.md (in-house FFT, file-picker model, v2 items shipped); guard concurrent instances by reserving pool budget in a small shared file under `/data/UserData/schwung/`; replace the fixed `usleep(60000)` free with a render-side ack (render writes "seen index X"; analyst frees when acked).

**R18 (v3). Convolve mode.** L, M. Line-in convolved with the current grain stream as a time-varying IR, single-engine kernel interpolation (CDC s5.2). Distinctive but costly; only after the above.

---

## Sources
- Local PDFs: Schwarz et al. DAFx-06 (CataRT); Sanfilippo & Parker DAFx20in21; Abate & Hansen DAFx23 (FBDL, https://github.com/NiccoloAbate/DelayCat); Abate & Hansen DAFx26 (CDC, https://github.com/NiccoloAbate/concatenation-driven-convolution); Schwarz, Roebel, Yeh, LaBurthe DAFx-16.
- SKataRT: https://forum.ircam.fr/projects/detail/skatart/ , https://support.ircam.fr/docs/skatart/quickstart.html
- FluCoMa: https://www.flucoma.org/ , https://learn.flucoma.org/reference/grid/ , https://learn.flucoma.org/reference/noveltyslice/
- SP-Tools: https://rodrigoconstanzo.com/sp-tools/ , https://github.com/rconstanzo/SP-tools
- DataMind Concatenator: https://www.synthtopia.com/content/2025/05/01/datamind-audio-releases-concatenator-ai-powered-audio-mosaic-tool/ , https://www.kvraudio.com/forum/viewtopic.php?t=616585&start=15
- Coalescence: https://www.musicradar.com/news/dillon-bastan-coalescence-m4l , https://rekkerd.org/coalescence-ai-powered-sampler-max-for-live-device-by-dillon-bastan/
- Echobit Mosaic: https://www.kvraudio.com/forum/viewtopic.php?t=499708
- AudioStellar: https://nime.pubpub.org/pub/dwtopcue/release/1 , https://forum.ircam.fr/article/detail/audiostellar/
- AudioGuide: https://github.com/benhackbarth/audioguide
- CatOracle / IMTR survey: http://imtr.ircam.fr/imtr/Corpus-Based_Sound_Synthesis_Survey
- Sononym: https://www.sononym.net/docs/manual/similarity-search/
- XLN XO: https://www.musicradar.com/reviews/xln-audio-xo ; Ableton Live 12 similarity: https://www.ableton.com/en/live-manual/12/live-concepts/
- Schwarz, eContact 16.2: https://econtact.ca/16_2/schwarz_corpus.html ; NIME'12: https://nime.org/proc/schwarz2012/
- Lallemand & Schwarz DAFx-11: https://hal.science/hal-01161307
- RhythmCAT: https://direct.mit.edu/comj/article/41/2/21/94580/Rhythmic-Concatenative-Synthesis-for-Electronic
- norns scripts (granular only found): https://norns.community/ , https://llllllll.co/t/concatenative-corpus-based-engines-m4l-vst/68739 (403 on fetch; not read)
