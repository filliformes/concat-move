# Concat: simulated user test (5 participants)

Build tested: working tree of `concat-move` on `main` (b57ca11 plus the uncommitted Level Norm and Pos Jitter changes). This is a code-grounded simulation, not a session on real hardware. Every behaviour below is traced to source. Items marked **[verify]** depend on host behaviour I could not confirm from code alone.

## 0. What the user actually meets (verified from code)

**Page sequence.** The planner (`page_plan.mjs`) walks root, then the sub-levels in root's `params` order. A preset-browser canvas comes before its level's grid.

| # | Header | Contents (encoders 1-8) |
|---|---|---|
| 1 | **Main** (landing) | X Position, Y Position, Density, Grain Size, Amp Attack, Amp Release, Polyphony, Filter |
| 2 | **View** (map canvas, also the recipe browser) | x_pos, y_pos, radius, density, grain_size, k_nearest, smooth, cutoff (no cells drawn, touch readout only) |
| 3 | **View** grid (second page, the same 8 knobs as cells) | same as #2 |
| 4 | **Corpus** | Source (filepath), Pool Cap, Rescan, Slice Mode, Min Slice, Max Slice, Analyze, Silence Gate |
| 5 | **Map** | X Descriptor, Y Descriptor, X Position, Y Position, Map Mode, Radius, K Nearest, Repeat Avoid |
| 6 | **Mod & Set** | Follow, LFO Rate, LFO Shape, Smooth, Pos Jitter, **Scope** |
| 7 | **Grain** | Density, Grain Size, Grain Env, Pitch, Pan Spread, Grain Random, Reverse, Pitch Jitter |
| 8 | **Shape** | Amp A/D/S/R, Filter Env, Filter A/D/S |
| 9 | **Play** | Play Mode, Note Match, Pressure, Pressure Amt, Velocity Amt, Polyphony |
| 10 | **Out** | Filter, Resonance, Saturation, Dry Mix, Volume, Width, Level Norm |

**Defaults that shape first contact:**
- Play Mode = **Notes**, Note Match = **Zone**, **Scope = File** (the code default; `help.json` and `README.md` both claim Folder).
- Pool Cap = **512 MB** (README says 128).
- Silence Gate = 0.02 absolute RMS, about -34 dBFS.
- Density = 20 g/s, Grain Size = 120 ms, Repeat Avoid = 4, Pos Jitter = 0.15.
- Filter Env = 0.45. On a bipolar amount, 0.45 is a small *negative* sweep.

**No saved corpus:** the analyst auto-picks the first file (name order) of the first sub-folder of `/data/UserData/UserLibrary/Samples`. With Scope = File, that single file becomes the corpus.

**Behaviours that matter for users:**
- **Notes mode:** silent until a pad is held. Each held note schedules grains at Density, and a note-on resets the accumulator to 0 (`nv->accum = 0.0`). **The first grain therefore arrives 1/Density after the hit:** 50 ms at the default, 200 ms at Density 5.
- **Map mode:** a continuous drone with no gate. `grain_schedule(..., amp=NULL)`, so **Amp Attack and Amp Release (Main knobs 5-6) do nothing in Map or Mosaic.** Holding a pad jumps to k-means cluster `note % 16`.
- **Zone:** `x = (note-36)/60`, and Y stays at Y Position. The pads are a 1-D strip. With Move's pad layouts, 32 pads cover roughly half the X range, and pads that share a note share a zone.
- **Polyphony** is the *grain-voice cap* (1-128, default 48), not note polyphony. Note polyphony is fixed at 16.
- **Changing Slice Mode, Min/Max Slice, Silence Gate or Pool Cap does nothing until Rescan.** `apply_value` has a TODO, and only `corpus_file`, `src_scope` and `rescan` arm the analyst.
- **Rescan** is a 0-127 int that fires a forced, cache-ignoring rebuild on every non-zero detent.
- **Any failed build replaces the working corpus with NULL.** `__atomic_exchange_n(&inst->index, ni, ...)` runs even when `ni == NULL`, so the old sound is freed and output goes silent. Analyze reads 0 with no message, and `get_error` is NULL. A build fails when:
  - there is no decodable file,
  - every file is over 128 MB (`INGEST_MAX_BYTES`, skipped silently), or
  - every slice falls under the Silence Gate.
- **View map vs knobs:** the scatter is always the PCA plane when `pca_ok`, but the default Map Mode is **Raw**, where X/Y drive Bright and Pitch. **The dot does not correspond to the picture in the default mode.** The dot also shows the knob position, not the LFO-animated, Zone or cluster target actually being played. Only the trail shows the truth.
- **Recipes:** selecting one in the browser applies it immediately on every jog detent, resetting all play-scope params to defaults first. There is no revert. Recipes also switch Play Mode (Cloud/Drone/Scatter/Sequence use Map, which drones on its own; Stutter/Pitched use Notes, which falls silent until a pad).
- **Mod wheel (CC1):** stored but unused (TODO).
- **Aftertouch:** a single global value, last writer wins. Any note-off zeroes it even while other pads are still held.
- **`raw_midi: true`** disables the host's knob-touch-note filter and aftertouch dead-zone (API.md). **[verify]** If internal MIDI reaches the slot, touching an encoder sends notes 0-9 into `on_midi`, which in Notes mode plays a Zone voice at x=0 and in Map mode starts a cluster jump.

---

## 1. Participant walkthroughs

### P1: first-time Move owner, no reading, wants sound in under a minute

1. **Install** from the Schwung Manager and add Concat to a chain slot. This takes about 30-60 s of menu work. A power cycle may be needed per `help.json` "Deploy notes" **[verify for fresh installs]**.
2. **Load.** The OLED shows **Main**: `X Position 0.50 | Y Position 0.50 | Density 20 | Grain Size 120 ms | Amp Attack | Amp Release | Polyphony 48 | Filter 0.50`. Nothing sounds, which is correct for Notes mode, but P1 does not know a mode exists.
   - **Branch A, Samples has content** (they have sampled or imported something): auto-pick loads, for example, `Samples/A-folder/01 kick.wav` as a **single-file** corpus. That is 1-3 slices, about 1-2 s to analyse. P1 hits a pad and hears a short grainy kick smear. Every pad sounds almost the same because 3 slices give no zones to speak of. **Time to first sound is about 5-15 s after load**, but it reads as "a broken sampler" rather than "a timbre-space instrument".
   - **Branch B, fresh Move with an empty `UserLibrary/Samples`:** nothing is picked. **Silence, with no message on Main.** "load a corpus" appears only on the View page, 1 jog step away, and P1 never reaches it. Time to first sound is effectively failed: P1 gives up or reads help. Help's "First sound" says "Dive into Corpus from the root menu", which requires knowing that jog-click opens a menu.
3. **Twists knobs.** X/Y on a 1-3 slice corpus change nothing audible. Density and Grain Size are audible and satisfying. "Polyphony": turning it down to 4 thins and chokes the sound, and P1 does not know why. Filter is a DJ filter, fine, but it shows `0.50`, not "Off", "LP" or "HP".
4. **Jogs to the next page:** the map. Branch A shows a few dots. P1 may jog-click, which enters the recipe browser (underline appears), and scroll. "Cloud" starts a *self-running drone with no pads held*. P1 is surprised but delighted. Scrolling on to "Stutter" returns to silence until a pad. It feels random.

**Outcome:** success only in Branch A, and the sound undersells the product. **Goal: partial or fail.**
**Empty states hit:** no corpus (silent Main), single-file corpus (no timbre variety).

### P2: sound designer, own field-recordings folder, evolving texture

1. **Main → jog to Corpus** (page 4, past two View pages). Touching encoder 1 shows `Source`, with no file name visible until entered. Jog-click opens the file browser rooted at `UserLibrary`. They navigate to `Samples/Field/…`. **They want a folder, but the browser picks files.** They pick `rain_01.wav`.
2. **Scope is File by default, so only `rain_01.wav` is analysed.** The map shows one file's slices. P2 wants the whole folder and looks for a "Folder" option on the Corpus page. **It is not there; Scope lives on "Mod & Set" (page 6).** Most P2s never find it. The rest find it after about 2-4 min of paging.
3. **Scope → Folder** re-analyses `dirname(rain_01.wav)` **recursively**. This is fine for `Field/`. If they had picked a file sitting directly in `Samples/`, the whole Samples tree (up to 2048 files) would be analysed instead.
4. **Waiting.** Analyze counts up per *file* as `0…100`, with no `%` and no file count. A folder of a few long 24/96 files moves in coarse jumps, sitting at `0` for tens of seconds on the first big file. **Any file over 128 MB is silently skipped**, which at 24-bit/96 kHz stereo means anything longer than about 3.7 min. That is typical for field recordings. If the whole folder is long takes, the build fails: silence, `Analyze 0`, no reason given. Only the README's troubleshooting mentions it.
5. **Silence Gate:** ambient beds at -40 to -50 dBFS RMS fall below the default 0.02 (-34 dBFS), so **every slice is gated and the build returns NULL**. The result is silence again and indistinguishable from "still analysing". P2 lowers Silence Gate to 0.005. **Nothing happens**, because slice and gate edits need Rescan. They find Rescan, turn it one detent, and the build runs (forced, ignoring cache). The knob now reads `Rescan 1`. The next detent rebuilds again.
6. **Explore the map.** Default Play Mode is Notes, so nothing drones. They go to Play and set Mode = Map to get a continuous texture. On the View map they turn X/Y. **The dot moves across a PCA scatter, but in Raw mode (the default) X/Y address Bright/Pitch, so the trail of played grains lands elsewhere.** P2 concludes "the map is decorative". Setting Map → Map Mode = Learned fixes it, but nothing hints at that.
7. **Evolving texture.** They find Follow = LFO on Mod & Set. LFO depth is hard-coded at 0.45 around the knob position, with no depth control. The dot does not move with the LFO; only the trail does. Level Norm (Out) and Pos Jitter (Mod & Set) are exactly what a texture builder wants, but they sit on unrelated pages.
8. **Save:** there is no Concat preset save. They save the Move set, which persists state including `corpus_file`. That works.

**Outcome:** success after about 10-20 min, **only if** their files are under 128 MB and above the gate. Otherwise fail with no diagnosis.
**Time to first sound on their own material:** about 3-8 min.

### P3: beatmaker / live performer

1. Loads the "Stutter" recipe (Notes, Zone, 40 ms grains) and programs Move steps on the Concat track.
2. **Latency:** each hit's first grain arrives 1/Density later (33 ms at Stutter's 30 g/s). Hits sound flammed and late against Move drums. Lowering Density for sparse hits makes it worse (200 ms at 5 g/s), and a short 16th at low density may produce one faint grain in the release tail.
3. **Repeatability:** the same pad does not give the same sound. The system picks a random slice among K in the radius; Repeat Avoid = 4 forbids the last 4 slices, so even with K=1 it alternates between neighbours; Pos Jitter moves the read point; pan is random. Making a pad deterministic takes 5 edits across 4 pages:
   - K=1 and Repeat Avoid=0 on Map,
   - Pos Jitter=0 on Mod & Set,
   - Pan Spread=0 and Grain Random=0 on Grain.

   No recipe does this.
4. **Pad layout:** Zone is 1-D and depends on Move's note layout, so different pads can share a sound and there are no pad LEDs to show zones. P3 cannot build a "kit" mental model. There is also no one-shot slice playback: holding a pad retriggers grains, so a kick hit becomes a buzz if held.
5. **Sync:** Follow = Sequence syncs only the map LFO. Grain rate is always in grains/s, never note divisions.
6. **Switching modules:** state is kept in the set. If the track was left in Map mode, the drone keeps sounding while P3 works on other tracks; only track mute stops it. Returning lands on Main **[verify]**.
7. **Expression:** pressure → Density works, but it is global and zeroed by any note-off. Holding a chord and releasing one pad kills the modulation on the others. Velocity → gain only. CC1 does nothing.

**Outcome:** usable for glitch textures; **fails the "rhythmic and repeatable" goal.**

### P4: returning user, reopens a saved set next day

1. **The set loads** and `state` restores params, then `corpus_file` arms the analyst with the cache.
   - **Happy path:** a cache hit loads in about 1-3 s (reading up to hundreds of MB of pool from flash). There is silence during that time with no indication, and the sound is identical afterwards.
2. **Drift paths, all silent:**
   - **The folder changed.** This is common if the corpus is `Samples/Recordings`, where Move keeps adding resamples. The signature differs, so the corpus is fully re-analysed at set load (up to minutes). **k-means clusters, PCA and min-max are recomputed, so pads, zones and X/Y positions now play different material.** Yesterday's performance does not reproduce.
   - **Less RAM free** (heavier set tonight): the cap becomes `MemAvailable-350` and is smaller than the cached pool. `cache_load` rejects (`pool_len > pool_cap`), the corpus is rebuilt, decimated differently, and the **cache is overwritten** with the smaller subset.
   - **The file was moved or renamed:** the build fails, giving permanent silence. The Source cell still shows the old path, nothing is auto-picked (`corpus_file` is non-empty), and there is no error.
   - **Two Concat instances in one set:** both arm at once. Each reads MemAvailable before either allocates, so both think the room is theirs, which is OOM risk with a 512 MB default.
3. The recipe name label is restored and tweaks are kept (good). The page position is not.

**Outcome:** success when nothing changed; **unreliable in any of the common drift cases, with zero feedback.**

### P5: melodic player, Notes mode + pitch matching

1. Default Note Match is **Zone**, so pads play different sounds at *natural pitch* with no melody. They must find Play → Note Match → Pitch, or load the "Pitched" recipe (index 5, if they find the browser).
2. **Pitch mode:**
   - The target weights only `D_PITCH`, and retuning applies only when `sl->pitch > 0`. **Unvoiced or noisy slices (pitch 0) win on low notes and play untuned.** Low pads sound off-key.
   - YIN confidence is not used to filter out unvoiced slices.
   - Large retune ratios chipmunk or droop the timbre because rate = pitch.
3. **"Polyphony" is a trap.** Setting it to 4 for "4-note chords" caps *grain voices* at 4, so chords stutter and starve. Actual note polyphony is 16 and not adjustable.
4. **Controls missing or misplaced:**
   - No glide or mono/legato.
   - Bend is fixed at ±2 st.
   - CC1 is dead.
   - Transpose ("Pitch") sits on the Grain page, not Play or Main.
   - The Amp envelope works here (Notes is the only mode where it does).
   - Filter Env at 0.45 closes the filter slightly on each note, subtly and unexplained.
5. **Sustain** needs Density × Size overlap. The "Pitched" recipe is fine, but Init (20 g/s × 120 ms = 2.4 overlap) sounds grainy and tremolo-like on held notes.

**Outcome:** plays melodies on a strongly pitched corpus (such as a vocal or instrument folder) after about 5 min of setup; **unreliable on mixed corpora.**

---

## 2. Controls people reach for vs where they are

Reach frequency is summed across P1-P5 in the simulation.

| Rank | Control reached for | Who | Where it is now | Verdict |
|---|---|---|---|---|
| 1 | **Pick corpus (folder!)** | P1 P2 P4 | Corpus p4 k1 (file only) + Scope on Mod & Set p6 | Misplaced, split across pages |
| 2 | **"Is it working / analysing / why silent"** | all | Analyze int on Corpus k7 (0 on failure) | Missing status |
| 3 | X / Y position | P1 P2 P3 | Main k1-2, View, Map | OK (but the dot misleads) |
| 4 | Density | all | Main k3, View, Grain | OK |
| 5 | Grain Size | all | Main k4, View, Grain | OK |
| 6 | Play Mode (Map vs Notes) | P1 P2 P5 | Play p9 k1 | Buried; decides "does it sound" |
| 7 | Recipe browse | P1 P3 P5 | View p2, jog-click | Hidden, and destructive on scroll |
| 8 | Radius / "spread" | P2 P3 | View, Map (not Main) | Should be on Main |
| 9 | Filter | P1 P3 | Main k8, Out | OK; show LP/HP/Off |
| 10 | Pitch (transpose) | P1 P5 | Grain p7 k4 | Should be on Main or Play |
| 11 | Note Match | P5 | Play k2 | OK, but its default hurts melodic users |
| 12 | Scope | P2 | Mod & Set k6 | Wrong page |
| 13 | Follow / LFO | P2 | Mod & Set | OK; LFO depth missing |
| 14 | Repeat Avoid / K (determinism) | P3 | Map | Should be one "Variation" macro |
| 15 | Level Norm | P2 | Out k7 | Belongs with corpus/grain |
| 16 | Pos Jitter | P2 P3 | Mod & Set k5 | Belongs on Grain |
| 17 | Amp Attack / Release | P5 | Main k5-6 | Dead in Map/Mosaic, which is where P1/P2 are |
| 18 | Polyphony | nobody on purpose | Main k7 | Misnamed; should not be on Main |
| — | Pool Cap, Min/Max Slice, Slice Mode, Map Mode, X/Y Desc | experts | Corpus / Map | Fine as deep settings |

### Proposed layout (8 knobs per page)

**Main (landing, performance):**
`X · Y · Spread (radius) · Density · Size · Pitch · Filter · Release`
- Release must be made to work in every mode (see I-6).
- If a built-in space FX is added (I-15), knob 8 becomes Space and Release moves to Shape.
- Polyphony, Attack and Grain Voices move off Main.

**View (map + recipe browser):** give View **exactly root's knob list**. The planner's duplicate-signature rule then suppresses View's grid page, leaving *one* map page with the Main knobs live on it and removing a page from the jog path. Better still, move the `map_view` as_page and the recipe triple onto **root**, so the map sits directly beside Main. Test where the planner lands; the preset page is emitted first. The canvas shows:
- the recipe name, plus a mode tag such as `Cloud · Map`,
- the *effective* target(s) (LFO, Zone, cluster) instead of the knob position,
- analysis status and errors when there is no corpus (I-2).

**Corpus:**
`Source · Scope · Slice Mode · Min Slice · Max Slice · Silence Gate · Pool Cap · Status`
- Status is read-only text: `Analyzing 3/12 files 41%`, `412 slices · 9 files`, `No audio found`, `All below gate`, `2 files >128MB skipped`, `Pool full: 60% kept`.
- Rescan becomes a Corpus menu action (jog-click) or disappears, because edits auto-re-arm after a 1.5 s debounce.

**Map:**
`Map Mode · X Desc · Y Desc · Spread · K Nearest · Repeat Avoid · Smooth · Variation/Seed`
- The duplicate X/Y are dropped, since they are on Main.
- The default Map Mode becomes **Learned**, so the picture and the knobs agree.

**Motion** (rename "Mod & Set"):
`Follow · LFO Rate · LFO Shape · LFO Depth · Sync Div · Freeze · Drift (slow random walk) · Pad Mode`

**Grain:**
`Density · Size · Envelope · Size Rand · Pos Jitter · Reverse · Pan Spread · Pitch Jitter`

**Shape:** unchanged (amp ADSR + filter env). Filter Env default 0.50 = off.

**Play:**
`Play Mode · Note Match · Transpose · Glide · Vel Amt · Pressure · Press Amt · Grain Voices` (renamed from Polyphony)

**Out:**
`Filter · Resonance · Saturation · Width · Level Norm · Dry Mix · Space · Volume`

Page count goes from 10 to 9, with Corpus moved to page 3 (Main, View, Corpus, ...), because Corpus is where every first session starts.

---

## 3. Consolidated friction log (ranked)

Severity: **S1** = blocks the goal / silent failure, **S2** = major confusion or wasted minutes, **S3** = annoyance.

| # | Sev | Step | What goes wrong | Who | Code location |
|---|---|---|---|---|---|
| F1 | S1 | Any failed analysis | Working corpus replaced by NULL, giving silence, `Analyze 0`, no reason | P2 P4 | `analyst_thread` swaps `ni` even if NULL; `get_error` NULL |
| F2 | S1 | First load, empty Samples | No corpus, silent Main, empty state only on View | P1 | `auto_pick_corpus`; `canvas.js` "load a corpus" |
| F3 | S1 | Field recordings | Files >128 MB silently skipped (24/96 stereo >3.7 min) | P2 | `ingest.c INGEST_MAX_BYTES` |
| F4 | S1 | Quiet ambience | Absolute Silence Gate 0.02 removes every slice, so silence | P2 | `corpus.c` rms ≥ sil_gate |
| F5 | S1 | Next-day reload | Folder changes or less free RAM cause re-analysis and a *different* corpus; moved file means permanent silence | P4 P3 | signature + `cache_load` cap check |
| F6 | S1 | Beat programming | First grain arrives 1/Density after note-on: late/flammed hits | P3 | `nv->accum = 0.0` |
| F7 | S2 | Folder vs file | Browser picks files; Scope defaults to File and lives on Mod & Set | P2 | `PARAMS[P_SRC_SCOPE].def=1`, hierarchy |
| F8 | S2 | Slice/gate/pool edits | No effect until Rescan; Rescan fires on every detent and reads `Rescan 7` | P2 | `apply_value` TODO; `rescan` handler |
| F9 | S2 | View map | Default Raw mode: dot ≠ picture; dot ignores LFO/Zone/cluster | P2 P1 | `map_project` uses PCA; `map_live` uses `pv[]` |
| F10 | S2 | Recipes | Scroll = apply (tweaks lost, no revert); mode switches make sound start or stop unexpectedly | P1 P3 P5 | `preset_index` → `apply_recipe` |
| F11 | S2 | "Polyphony" | It is the grain-voice cap, so users choke the sound | P5 P1 | `P_POLY` label, on Main k7 |
| F12 | S2 | Main k5-6 | Amp Attack/Release dead in Map and Mosaic | P1 P2 | `grain_schedule(..., NULL, ...)` |
| F13 | S2 | Repeatability | Random-in-K + Repeat Avoid 4 + Pos Jitter + random pan: no deterministic pad | P3 | `query_select`, grain defaults |
| F14 | S2 | Pitch match | Unvoiced slices (pitch 0) win low notes and play untuned | P5 | `render_block` Pitch branch; `spawn_grain` retune guard |
| F15 | S2 | Zone pads | 1-D, layout-dependent, pads collide, about half the X range; no LED feedback | P3 P5 | `zx = (note-36)/60` |
| F16 | S2 | Docs | help/README say Scope default Folder, Pool 128, Corpus landing page, "Corpus > Scope" | readers | `help.json`, `README.md` |
| F17 | S2 | Big analysis | Progress is per-file (0 then jumps), no `%`, no file count, silence while waiting | P2 P4 | `progress` per file |
| F18 | S3 | Aftertouch | Global value; any note-off zeroes it for held pads; `raw_midi` bypasses dead-zone | P3 P5 | `on_midi` 0x80 branch |
| F19 | S3 **[verify]** | Knob touch | With `raw_midi:true`, touch notes 0-9 may reach `on_midi` and trigger voices or cluster jumps | all | module.json + API.md |
| F20 | S3 | Paging | 10 pages; Corpus is 3 jogs away; View appears twice | all | hierarchy |
| F21 | S3 | Filter display | Bipolar Filter reads `0.50`, not Off/LP/HP; Filter Env 0.45 = slight negative | P1 P5 | `viz:false`, default |
| F22 | S3 | Misplaced params | Level Norm on Out, Pos Jitter and Scope on Mod & Set, Pitch on Grain | P2 P5 | hierarchy |
| F23 | S3 | Missing MIDI | CC1 no-op; bend fixed ±2 st; no glide | P5 | `on_midi` TODO |
| F24 | S3 | Memory | Pool default 512 MB (docs say 128); two instances race the MemAvailable guard | P4 | `P_POOL_MB.def`; `arm_analyst` |
| F25 | S3 | Recursive scope | Picking a file at the Samples root analyses the entire library (up to 2048 files) | P2 | `set_corpus_path` + recursive `scan_audio` |
| F26 | S3 | Mosaic | Built-in mic + speaker can feed back; nothing indicates input level | (Mosaic users) | Mosaic branch |

---

## 4. Improvement proposals

Value: H/M/L. Effort: S (hours), M (1-3 days), L (a week+). Feasibility on Move/Schwung is noted where it is a constraint.

### A. Reliability and empty states (do first)

| ID | Proposal | Value | Effort | Feasibility |
|---|---|---|---|---|
| I-1 | **Keep the old corpus on failure.** Never swap in NULL; record a `last_error` code instead. | H | S | Pure DSP |
| I-2 | **Status string** (`corpus_status` param, read-only): Analyzing n/N files %, slices/files count, No audio, All below gate, N files too large, Pool full x% kept, File not found. Show it on the Corpus page cell *and* as the View canvas empty state (add to `extra_keys`). Implement `get_error`. | H | S-M | chain_params string cell + as_page `extra_keys`: supported in 1.3.0 |
| I-3 | **Auto-pick fallback chain:** Samples → Recordings → any UserLibrary folder with ≥ 10 files. Default Scope = Folder on auto-pick. If still nothing, OLED on Main/View shows "No audio found: Corpus > Source". | H | S | Worker thread only |
| I-4 | **Ship a small factory corpus** (for example 20-40 MB of CC0 mixed material) inside the module dir as last-resort default, so P1 always gets a map. | H | M (curation) | Rootfs is full: install under `/data/UserData/schwung/concat-corpus/`, not the module dir |
| I-5 | **Silence Gate relative to the file's peak/median** (for example -40 dB below the file's 95th-percentile RMS), in dB units. Never gate a whole file to zero: keep the loudest N slices. | H | S | DSP |
| I-6 | **Streamed decode for big files:** chunked dr_wav/dr_flac reads, and slice while decoding, replacing the 128 MB skip. At least surface "skipped" in status. | H | M | Keeps memory bounded; worker thread |
| I-7 | **Reproducible reload:** store the *pool selection* and clusters/PCA in the cache keyed by the set, and on re-open prefer the existing cache even if the folder gained files (offer "Corpus changed, Rescan?" in status). If RAM is short, load the cache partially instead of rebuilding. | H | M | Disk under `/data/UserData`; fine |
| I-8 | **Auto-apply slice/gate/pool edits** with a 1.5 s debounce, and remove Rescan from knobs (make it a menu action). | M | S | Worker already supports re-arm |
| I-9 | **Remember the path relative to UserLibrary**, and if missing, show "File not found" and fall back to the auto-pick. | M | S | DSP |

### B. Playability and interaction

| ID | Proposal | Value | Effort | Feasibility |
|---|---|---|---|---|
| I-10 | **Immediate first grain on note-on** (`accum = 1.0`), plus an optional *sample-accurate* first grain at the note's block offset. Fixes flams. | H | S | DSP |
| I-11 | **Slice / One-shot pad mode:** a pad plays its nearest slice once, full length, with a declick envelope, choke-able. A Pad Mode enum: Grains / One-shot / Hold-loop. Turns Concat into a descriptor-sorted slicer for beatmakers. | H | M | DSP |
| I-12 | **2-D pad map:** in Zone/Map, place the 32 pads on an 8×4 grid over the map (column → X, row → Y). Use a `pad_map` enum: Zone-1D / Grid-2D / Clusters. Needs pad identity: either raw pad notes 68-99 (`raw_midi` already on) or a declared chromatic layout. | H | M | **[verify]** which notes reach the slot in shadow mode; no LEDs, so show the pressed cell on the View canvas |
| I-13 | **Clusters sorted by a descriptor** (for example brightness ascending along pads) instead of `note % 16`, so pad neighbourhoods mean something. | M | S | Computed at analysis |
| I-14 | **Variation macro** (0 = deterministic: K=1, repeat avoid 0, pos jitter 0, pan 0, fixed seed per pad; 1 = current randomness) plus a **Seed** that makes random choices repeatable per pad hit. | H | S-M | DSP |
| I-15 | **Freeze/Hold:** latch the current target (and optionally the last K selected slices) so the texture stays while you move X/Y. Also a "Hold pads" latch in Map mode. Shift+pad could toggle **[verify: `claims_ccs` for Shift]**. | H | S | DSP; Shift via 1.3.0 `claims_ccs` |
| I-16 | **Tempo-synced grain rate:** in Follow = Sequence (or a Sync toggle), Density becomes divisions (1/4…1/32, triplets) locked to MIDI clock, with grain onsets on the clock. | H | M | Clock already parsed in `on_midi` |
| I-17 | **Amp envelope in Map/Mosaic:** gate with "any pad held OR Map drone on", and apply Release on mode change and stop. Add a Drone on/off (or Map = "held pads only" option) so Map is not always sounding in a live set. | M | S | DSP |
| I-18 | **Per-note pressure** (poly AT per voice) and do not zero AT on other voices' note-off. | M | S | DSP |
| I-19 | **CC1 → Spread/Density macro, bend range param, glide/legato** for Notes. | M | S | DSP |
| I-20 | **Voiced-only pitch matching:** weight by YIN confidence, exclude pitch=0 slices in Pitch mode, and add a "Pitch tolerance" so near slices need less retune. | H | S | Descriptors already stored |
| I-21 | **Rename Polyphony → "Grain Voices"** and move it off Main; add real "Note Voices" (1-16) if wanted. | M | S | Label + hierarchy |

### C. UI and navigation

| ID | Proposal | Value | Effort | Feasibility |
|---|---|---|---|---|
| I-22 | **The layout in section 2:** Main = performance 8, one View map page (dedupe via identical knob list, or put the map on root), Corpus as page 3, fix misplaced params. | H | S | ui_hierarchy only; keep module.json in sync |
| I-23 | **Map truthfulness:** default Map Mode = Learned. In Raw mode, rebuild `map_pts` on the chosen X/Y descriptors (worker, on change). `map_live` sends the effective target(s): LFO-animated point, each held Zone/cluster, Mosaic target. | H | M | Build on analyst thread when `x_desc`/`y_desc` change |
| I-24 | **Recipe browser safety:** index 0 = "Current (unsaved)" snapshot taken on first browse move, so scrolling back restores tweaks; or keep the audition-on-scroll but add Undo (Undo button via `claims_edit_ccs`) **[verify gesture scope]**. Tag recipe names with their mode ("Cloud · Map"). | H | S-M | DSP-side snapshot |
| I-25 | **User recipes:** save and recall play-scope snapshots to `/data/UserData/schwung/concat-recipes/`, listed after the factory ones in the same browser. | H | M | Browser list is DSP-served; count/name dynamic |
| I-26 | **Filter display:** serve Filter as text "LP 1.2k / Off / HP 300" via an enum-like formatted value or a unit trick; Filter Env default 0.5 labelled bipolar. | M | S | get_param formatting **[verify]** host shows float strings |
| I-27 | **Onboarding on the View canvas:** when there is no corpus or Notes mode is silent and idle, draw a two-line hint ("Hit pads to play" / "No corpus: Corpus > Source"). | M | S | canvas.js only |
| I-28 | **Folder-level picker:** a quick-pick enum of scanned folders (the `quick_pick` idea from CLAUDE.md, already scaffolded as `ingest_scan_folders`) next to Source, so "use this folder" is one knob turn. | H | S-M | DSP-served dynamic enum |

### D. Feature set and effects

| ID | Proposal | Value | Effort | Feasibility |
|---|---|---|---|---|
| I-29 | **Record into corpus:** capture N seconds of line/mic in, or Concat's own output (resample), to `/data/UserData/UserLibrary/Samples/Concat/` and hot-append to the pool (the bounded-arena v2). "Sample your room, play it as a map." | H | L | Audio-in readable (`audio_in_offset`); write on a worker; arena growth is designed for |
| I-30 | **Built-in Space** (small FDN reverb + tempo delay) as one Space knob plus page. Chain audio FX slots already exist, but a grain cloud wants a per-instrument wash, and one knob beats loading a second module live. | M | M | CPU: a compact FDN at 44.1k is OK on CM4; denormal guards |
| I-31 | **Better recipe set (8-12):** Init, Cloud, Drone, Scatter (keep); add **Kit** (Notes/Grid-2D, one-shot, Variation 0), **Tape Wash** (long Gauss grains, low density, sat 0.3, LFO slow), **Glass** (Pitched, bright X, Hann, width 1), **Rhythm** (sync 1/16, Expodec, clusters), **Freeze Pad**, **Mosaic Voice** (Mosaic, dry 0.2, gate on input). Every recipe sets Variation/Pos Jitter/Level Norm explicitly. | H | S | Table edit |
| I-32 | **Macro knobs** (Chaos: spread + jitter + rand + K; Motion: LFO rate + depth; Texture: size + density inversely). Map to Main in an alternate "Macro" page. | M | M | DSP |
| I-33 | **Mosaic safety:** input level meter on canvas, input gate threshold, a warning or dry-mix clamp when the built-in mic is the source. | M | S | DSP + canvas |
| I-34 | **Concatenation over time ("Path" follow):** record X/Y knob moves as a loopable path synced to the clock (Follow = Path). | M | M | DSP ring buffer of positions |
| I-35 | **Docs sync:** fix help.json/README defaults (Scope, Pool, landing page, Scope location), add a "Why is it silent?" help page. | M | S | Text |

---

## Top 10 (ordered by value/effort)

1. I-1 + I-2: keep the old corpus on failure, and add a status string (OLED + `get_error`).
2. I-10: immediate first grain on note-on.
3. I-5: relative silence gate, never gate everything.
4. I-3 + I-4: auto-pick fallback plus a factory corpus, Scope = Folder by default.
5. I-22 + I-28: layout (Main performance 8, one map page, Corpus page 3 with Scope and folder quick-pick).
6. I-23: truthful map (Learned by default, effective-target dot).
7. I-14 + I-11: Variation/Seed determinism and One-shot slice pad mode.
8. I-24 + I-25 + I-31: safe recipe browsing, user recipes, better recipe set.
9. I-7 + I-6: reproducible reload and big-file streaming.
10. I-15 + I-16: Freeze and tempo-synced grain rate.
