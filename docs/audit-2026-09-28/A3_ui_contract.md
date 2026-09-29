# A3: Concat UI contract, manifest, canvas and docs audit

Audited 2026-09-28 against the working tree (includes the uncommitted `level_norm` / `pos_jitter` change) and Schwung main (v1.5.0 docs + `src/shared/param_pages/*`). Read-only; nothing in the repo was edited.

Method:
- `audit/run_plan.mjs` reconstructs the DSP-served `chain_params` and `ui_hierarchy` straight from `concat.c` (PARAMS[], UI_HIERARCHY, the chain_params serializer). It then runs Schwung's own `planPages()` and `validateContract()` on them, and on `module.json` for comparison. Outputs: `audit/dsp_chain_params.json`, `audit/dsp_ui_hierarchy.json`.
- `audit/labels.mjs` runs Schwung's `labelForCell()` over every param name.
- `audit/helpwidth.cjs` is Schwung's `test_help_content_width.sh` logic pointed at Concat's help.json.
- `audit/tw.cjs` gives device-font text widths for the canvas strings.
- `tools/param-pages/validate.mjs` only reads the checked-in fleet fixture (captured 2026-08-28, which does not include Concat). So I called `validateContract()` directly. That is the same code path validate.mjs uses.

---

## 0. Reconstructed page list (what the user actually sees, in jog order)

Identical for the DSP-served contract and for module.json. Planner warnings: none. Realigned pages: none.

| # | Kind | Page name | Level | Knobs 1-8 | Graphics |
|---|------|-----------|-------|-----------|----------|
| 1 | knobs | **Main** | root | X Pos, Y Pos, Density, Grain Size, Amp Atk, Amp Rel, Poly, Filter | AR envelope over Amp Atk + Amp Rel (declared, slots 5-6) |
| 2 | preset + canvas | **View** | view | X Pos, Y Pos, Radius, Density, Grain Size, K Near, Smooth, Filter | canvas.js `drawPage` is the body. This page IS the recipe browser (jog inert until click) |
| 3 | knobs | **View - 2** | view | same 8 as page 2 | none |
| 4 | knobs | **Corpus** | corpus | Source, Pool Cap, Rescan, Slice Mode, Min Slice, Max Slice, Analyze, Silence Gate | `sample` waveform inferred on Source |
| 5 | knobs | **Map** | map | X Desc, Y Desc, X Pos, Y Pos, Map Mode, Radius, K Near, Repeat Avoid | none |
| 6 | knobs | **Mod & Set** | mod_set | Follow, LFO Rate, LFO Shape, Smooth, Pos Jitter, Scope, -, - | **none** (the declared LFO group is silently dropped, see F5) |
| 7 | knobs | **Grain** | grain | Density, Grain Size, Grain Env, Pitch, Pan Spread, Grain Rand, Reverse, Pitch Jitter | none |
| 8 | knobs | **Shape** | shape | Amp A/D/S/R, Filt Env, Filt A/D/S | ADSR over slots 1-4 (declared) |
| 9 | knobs | **Play** | play | Play Mode, Note Match, Pressure, Press Amt, Vel Amt, Poly, -, - | none |
| 10 | knobs | **Out** | out | Filter, Resonance, Sat, Dry Mix, Volume, Width, Level Norm, - | fader inferred on Volume **and** Level Norm |
| 11 | menu | My Presets | (host) | | |
| 12 | menu | Module (+ "Module Help" row, because help.json has children) | (host) | | |

The recipe browser / map canvas page is **"View"** and comes **before** the grid, which is **"View - 2"**. `page_plan.mjs:854-899` emits the PAGE_PRESET first, and `claimName` gives the canvas's `name` ("View") to it. The grid then claims "View" again and gets "View - 2". MODULES.md says it outright: "it is emitted first, so it is what you land on". The comment at `concat.c:188-193` has the order backwards.

Validator result on the DSP contract: **0 errors, 0 warnings, 5 info.** The five: `opaque-on-knobs` (Source), `viz-inferred` sample on corpus_file, `viz-inferred` fader on volume, `viz-inferred` fader on level_norm, `viz-declared` 2 graphics. No unreachable params, no duplicate keys, no level over 8 knobs, and the preset triple is well formed. chain_params is about 5.2 KB. With the old ~4 KB cap gone that is fine, but only on Schwung 1.3.0 or later.

---

## 1. Ranked findings

Severity: H = user-visible bug or wrong behaviour, M = misleading or degraded UX, L = cosmetic or hygiene. Confidence is on the finding itself.

### H1. canvas trail fade is inverted: the newest played slices are not drawn (H, conf high)
`src/canvas.js:59-64`. The DSP emits the trail newest-first (`concat.c:804-812`, `ring[(rh-1-t)]`, t=0 newest, at indices 3,4). The loop walks from the end (oldest) to the start, which is right for z-order. But it computes `age = (live.length - 1 - i) / 2 | 0`, so the **oldest** pair gets age 0 and a filled circle, and the newest gets the largest age. With 8 points the newest 4 are not drawn at all. With the default Repeat Avoid 4, the newest point is a single pixel and the oldest is a blob.
Fix: `const age = (i - 3) / 2 | 0;`.
Related: trail length is `min(repeat_avoid, 8)`, because the trail reads the repeat-avoid ring (`query.c:40-47`). So Repeat Avoid = 0 (the Stutter recipe) shows no trail at all. Either document that or keep a separate "last played" ring for the view.

### H2. The View map's query dot is in a different space from the scatter behind it (H, conf high)
- The scatter (`map_pts`, `concat.c:684-723`) and the trail are projected by `map_project()`. It uses the PCA plane whenever `ix->pca_ok`, **regardless of `map_mode`**, and otherwise uses centroid vs loudness **regardless of `x_desc`/`y_desc`**.
- The dot (`map_live` qx,qy, `concat.c:798-800`) is the raw `x_pos`/`y_pos` knob, which drives Bright/Pitch by default (`nav_target`, `concat.c:916-919`).
- With the defaults (Map Mode = Raw, Y Desc = Pitch), the dot has no geometric relationship to the points or the trail. They agree only in Learned mode.
- In the non-PCA fallback, `qn` is RobustScaled (median/IQR, centred on 0; `corpus_engine.h:25`), not 0..1. The clamp in `map_project` therefore piles roughly half the corpus onto the x=0 / y=0 edges. The query itself min-max normalises through `nav_min/nav_max` (`query.c:48-54`).
- `map_pts` is built once per corpus load (`concat.c:499`), so changing X/Y Desc or Map Mode never redraws the scatter.

Fix: project the scatter with the same axes the query uses:
- **Learned**: the PCA plane.
- **Raw**: `(qn[x_desc] - nav_min) / (nav_max - nav_min)` for X, and the same for Y.
- Rebuild `map_pts` when x_desc, y_desc or map_mode changes. Do this on the worker (arm a cheap re-projection), not in set_param.

### H3. The dot does not show where the engine is actually querying (H, conf high)
`map_live` always reports the knob values (`concat.c:798-799`). It ignores:
- the Follow=LFO/Sequence offset (`xpos/ypos` are computed locally in `render_block`, `concat.c:975-998`);
- the Notes + Zone target (x comes from the note, `concat.c:1024`);
- Map-mode pad cluster jumps (`concat.c:1049-1052`);
- the Mosaic live target.

`canvas.js:9` claims the view "animates smoothly (trail, LFO, pad moves)". Only the trail moves. Default Play Mode is Notes/Zone, so out of the box the dot never reflects what is playing.
Fix: store the effective `xpos/ypos` (and a cluster or zone flag) into instance fields in `render_block` and serve those in `map_live`. Schwung's `live: true` + `<key>:effective` contract (MODULES.md "A parameter the module drives itself") is the matching host-side feature. It would also make the X/Y Pos dials on Main and Map follow the LFO. Caveat from the doc: a silent slot is barely rendered, so compute from state set_param already holds where possible.

### H4. `min_host_version` "0.9.10" and README "v0.9.x or later" are wrong: Concat needs Schwung 1.3.0+ (H, conf high)
`src/module.json:12`, `README.md:57`. The DSP's own comment (`concat.c:731-735`) says it needs 1.3.0, for the 64 KB SHADOW_PARAM_VALUE_LEN. The contract also depends on `as_page`, `preset_browser`, canvas `extra_keys` and `drawPage`. On an older host, chain_params (~5.2 KB) truncates and the View page does not exist.
Fix: set `"min_host_version": "1.3.0"` (or "1.5.0" if you adopt the items in section 5) and update the README Requirements and Troubleshooting ("Source shows float 0.00-1.00" is also the symptom of a pre-1.3 host).

### H5. Analyze is a turnable knob that does nothing (M-H, conf high)
`concat.c:123` declares `analyze` as a plain int 0..100. set_param ignores writes (`concat.c:677`). On the Corpus page (knob 7) the cell turns, shows a fake value, then snaps back on the next read. The DSP also drops module.json's `"unit":"%"`.
Fix: in the serializer, emit `"access":"read"` (dotted readout, never turnable) plus `"unit":"%"` for `analyze`. With `max:100` declared, the `%` formatter shows the raw value (MODULES.md "`%` units and `max`"). Optionally add `"display":"big"`.

### M1. Rescan should be `access:"write"` (M, conf high)
`concat.c:119`, `634-639`. It is a Pattern-3 int 0..127. Every detent with v != 0 re-arms a forced full rebuild, and turning left from 1 to 0 does nothing. Schwung now has a real trigger contract. `access:"write"` makes the knob fire once per gesture in either direction (`page_controller.mjs` `fireTrigger`/`TRIGGER_KNOB_GESTURE_GAP_MS`), a jog click fires it, and it writes `"1"`, which the existing `atoi(val)!=0` path already handles. The README text "Turn right to fire" and the "counts up" behaviour then change.

### M2. Slicing / pool knobs have no effect until Rescan (M, conf high)
`arm_analyst` is only called from corpus_file, src_scope and rescan (`concat.c:620-639`). The stale TODO at `concat.c:302-303` still says so. Changing Slice Mode, Min Slice, Max Slice, Silence Gate or Pool Cap is silent until Rescan or a re-pick. Neither help.json nor README says this.
Fix: re-arm the analyst (debounced, force=0, so the cache signature including slice params decides) when these change, or document "press Rescan after changing slicing".

### M3. Amp envelope knobs (incl. Main K5/K6) are inert in Map and Mosaic modes, and recipes set dead values (M, conf high)
- The amp ADSR is per-note and only runs in Notes mode (`concat.c:1002-1036`). The "global amp" fields (`concat.c:262-263`) are never used.
- Main's Amp Atk and Amp Rel therefore do nothing in Map or Mosaic.
- The Cloud, Drone, Scatter, Mosaic and Sequence recipes set `amp_atk/amp_sus/amp_rel` in Map/Mosaic (`concat.c:318-331`), so those values are dead.
- `amp_rel` also sets the **filter** envelope release (`concat.c:1067`), which is undocumented. In Map/Mosaic the filter gate is always 1, so it never releases anyway.

Fix: either apply a global amp env in Map/Mosaic (gate = transport or pads) or document it. Mention the shared release in help and README.

### M4. `"LFO Rate"`/`"LFO Shape"` viz group is silently dropped (M, conf high)
`concat.c:136-137` declares `{group:"lfo", role:"rate"}` and `{group:"lfo", role:"shape"}` with no `kind`. `viz.mjs inferKindFromRoles` only infers `lfo` when **rate AND depth** are present, and `collectDeclared` does `if (!kind) continue;`. So no graphic, no warning, and no `viz-declared-not-adjacent` either. The planner output confirms Mod & Set draws no graphic.
Fix: add `"kind":"lfo"` to one member. Circle, SineX and SineY draw as a sine and S&H as S&H (`viz_draw.mjs lfoShapeIdOf`). Circle is really a 2-D path, so if the 1-D picture reads wrong, use `viz:false` instead. module.json has the same declaration.

### M5. canvas: placeholder and dot flash on page entry and on every recipe step (M, conf med-high)
`canvas.js:40-53`.
- After each recipe detent the controller clears its value cache (`page_controller.mjs stepPreset`: `s.values = Object.create(null)`).
- On page entry, `map_pts`/`map_live` arrive only when the read rotation reaches them. That is 8 knobs + 2 extra keys, one per tick.
- Until then `vals.map_pts` is `undefined`, so the canvas prints "load a corpus", and `map_live` falls back to `'0,0,0'`, which draws the query dot at the bottom-left corner.

Fix:
- Keep the last good strings in `o.state` (drawPage receives a per-slot `state`).
- Show the placeholder only when `vals.map_pts === ""`. The DSP returns an empty string for "no corpus", and the host keeps `""` distinct from a missing read.
- Skip the dot until `map_live` has arrived.

### M6. canvas: no analysing state (M, conf high)
During the first analysis the page says "load a corpus", which is wrong. Add `analyze` as a third `extra_keys` entry (cap 4, so 3 used) and draw "analysing NN%" while it is between 1 and 99.

### M7. canvas: the recipe strip permanently hides the top 18% of the map, and "8/8" is truncated (M/L, conf high)
- The body band on a PAGE_PRESET is 120 x 45 px (`MENU_FRAME_*`, footer on). `canvas.js:80` clears rows 0-7 on **every** frame because `pr.count` is always 8. The loudest or top-PCA slices sit under an opaque strip.
- `canvas.js:82` places the counter with `idx.length * 5`. The device font is proportional: `ctx.textWidth("8/8") = 18` vs the assumed 15. frameCtx `print` truncates to the budget (17 px at x=103), so the 8th recipe (Sequence) reads **"8/"**. Same for "2/8"..."7/8" with wide digits.

Fix: `const iw = ctx.textWidth(idx); ctx.print(W - iw, 1, idx, 1);`. Draw the strip only while `pr.entered` (or briefly after a change), or lay the scatter out in `H-9` rows below it.

### M8. README is substantially stale (M, conf high). Full list in section 3.
Wrong landing page, wrong page list, wrong defaults (Scope, Pool, Min/Max Slice, Filt Env, Pressure, Vel Amt), wrong host requirement, and no mention of View, recipes, auto-load, Level Norm, Pos Jitter or Smooth.

### M9. help.json stale in 4 topics and missing the new features (M, conf high). Full list in section 3.
Help is now one jog from the controls: Schwung shows a "Module Help" row on the Module page when help.json has children. That makes accuracy more important.

### L1. Page naming: "View" (map) then "View - 2" (grid) (L, conf high)
Consistent with the deliberate two-page decision, but "View - 2" is the host's collision suffix and reads like a mistake. The canvas param `name` names the browser page (`page_plan.mjs:883`). Rename `map_view.name` to e.g. "Explore" or "Recipes" (`concat.c:745`, module.json). The pages then become "Explore" (map + recipes) and "View" (grid). Also fix the reversed comment at `concat.c:188-193`.

### L2. Browsing recipes on the map page resets the whole play-scope (L/design, conf high)
Clicking the jog on the View page enters the browser, and each detent LOADS a recipe (`apply_recipe` resets every non-Corpus param to its default first, `concat.c:342-343`). That includes Play Mode, Volume and Pitch. `stepPreset` also wraps, so one left detent from Init lands on Sequence. This is by design, but a user who clicks the map expecting to interact with it loses their tweaks. At minimum, say so in help.

### L3. Radius circle is symbolic and scaled by the short axis (L, conf high)
`canvas.js:56` uses `qr/1000 * min(W,H) * 0.5`. In the engine, radius is not a distance. It widens the random pick pool among the K nearest: `ceil(1 + radius*(k-1))` (`query.c:112-114`), so at K=1 it does nothing. The README ("how far around the target grains may wander") is also wrong about this. Either draw the ring as a K/radius "spread" indicator and say so, or drop it.

### L4. canvas perf: 360-point string parse + 360 clipped fillRects every frame (L, conf med)
`canvas.js:40-47`: `split(';')` creates 360 strings, the per-point `split(',')` creates about 720 more, and each `setPixel` goes through frameCtx's clip plus a host binding. At 120x45 many of the 360 points share a pixel. Parse once per distinct `map_pts` string into a de-duplicated `Uint16Array` of pixel coords cached in `o.state`, then blit. Same for `map_live` (cheap anyway).

### L5. Out page: Level Norm drawn as a fader (L, conf high)
The fader detector claims both `volume` and `level_norm`. Level Norm is a blend amount, not a level. Add `"viz":false` to `level_norm` and `"viz":{"kind":"fader"}` to `volume` so neither is a guess (clears two `viz-inferred` infos).

### L6. Corpus page: `sample` waveform inferred on Source (L, conf med)
The detector draws the picked file's waveform. That is useful for WAV, but `wav_peaks.mjs` only parses WAV/AIFF, so .flac and .mp3 picks draw the error or empty state. With Scope=Folder it shows one file of many. Declare `"viz":{"kind":"sample"}` to keep it on purpose, or `"viz":false`.

### L7. Cell labels squeezed into non-words, and one collision (L, conf high; from `labelForCell`)
`PJITTER` is both Pitch Jitter (Grain) and Pos Jitter (Mod & Set). Other squeezed labels: `POOCAP`, `MISLICE`/`MSLICE` (min vs max nearly identical), `KNEARE`, `XDSCRP`/`YDSCRP`, `FOLLO`, `LNORM`, `VEAMT`, `SMODE`/`MMODE`/`PMODE`, `SIGATE`, `GRNRND`, `PRSAMT`.
Fix: add `short_name` in chain_params, for example Pool, Min, Max, K, X Ax, Y Ax, Mode, P.Jit / Pos J, Norm, Vel, Gate. Enum `short_options` would help too: Pressure Proximity -> Prox, Follow Sequence -> Seq, Map Mode Learned -> PCA, Grain Env RevExpo/Trapez -> RExp/Trap, Play Mode Mosaic -> Mos, Scope Folder -> Dir. These names also show on the new E16/EC4 surface screens (1.5.0).

### L8. module.json is out of sync with the DSP in dozens of fields (L, conf high)
A sound generator's module.json `ui_hierarchy` is never read (MODULES.md "Where the hierarchy must live"), and the DSP chain_params wins at runtime, so this is hygiene only. Keys, names, types, ranges, enum option strings, knobs/params per level and nav order all **match**. Differences:
- **Enum defaults missing**: `src_scope` (DSP default File, module.json implies Folder), `play_mode` (Notes vs Map), `note_match` (Zone vs Off), `pressure` (Density vs Off), `y_desc` (Pitch vs Bright).
- **DSP omits** every `default` (harmless: the host uses `default` only for child-instance clear, `page_controller.mjs:4583`, which Concat does not have), the `display_format:"%.0f"` on min/max_slice, density, grain_size and pitch, and `analyze.unit:"%"`.
- **module.json omits** `step:0.01` on 19 floats.
- Array order differs (map_live/map_pts vs the preset triple; grain_rand position; level order). Nav order is identical.
- `version` 0.1.0 equals the existing `v0.1.0` tag, but the code is far past it. Bump before the next release.

Fix: generate module.json's chain_params and hierarchy from the same table (a tiny host-side dump of `get_param("chain_params")`/`("ui_hierarchy")`) instead of hand-syncing.

### L9. Stale comments and dead serving code (L, conf high)
- `concat.c:14-19` says "render_block currently outputs SILENCE".
- `concat.c:179-184` says "Root is the landing page (Corpus/setup) with the Source cell first".
- `concat.c:188-193` has the page order reversed (L1).
- `concat.c:302-303` is a TODO that is half done.
- `get_param("map_scatter")` and `scatter_bmp[600]` (`concat.c:791-794`, `corpus_engine.h:34`) are legacy and unused.
- `CLAUDE.md` still says kissfft (the code is an in-house radix-2 FFT, `analysis.c:15`), mentions `quick_pick` (not implemented) and the old page set.

### L10. Auto-pick nits (L, conf med)
- `auto_pick_corpus` (`concat.c:405-435`) picks the name-smallest full path among the first 256 files of a **recursive** listing (`ingest.c:101-115`). That can come from a nested sub-sub-folder, and the 256 cap is taken in readdir order, so the true alphabetical first can be missed.
- The analyst thread writes `inst->corpus_file` (`concat.c:480`) while `get_param("corpus_file")`/`("state")` can read it on the SPI thread: a benign but real torn-read race. (DSP-audit territory; noted for completeness.)

---

## 2. Contract checks that PASS

- 8-field API, `get_param` returns the snprintf count and -1 for unknown keys. `ui_hierarchy` and `chain_params` are both DSP-served.
- Every knob key on every level exists in chain_params. No level has more than 8 knobs (max 8; mod_set 6, play 6, out 7). No orphan params: all 50 PARAMS sit on a page. `preset_count`, `preset_name`, `map_live` and `map_pts` are `access:"read"` and correctly not flagged. `preset_index` is a selector and correctly kept off knobs.
- Enum option strings are identical in PARAMS, get_param (returns names) and set_param (accepts name or index), and in module.json.
- Canvas: `type:canvas`, `as_page:true`, `preset_browser:true`, `extra_keys` = 2 (cap 4), no `enterable` (correct: it is mutually exclusive with preset_browser), `show_value:false` harmless. drawPage uses only `o.values/keys/touched/preset`. There is no getParam on the draw path, and nothing escapes the frame (frameCtx clips).
- Preset triple: `list_param` index is 0-based, `count_param` = 8, `name_param` serves `RECIPES[preset_idx].name`. The host writes the index on each detent (`stepPreset`), which LOADS it (matches the design), and re-reads the name. The state restores only the label (`preset_idx`), so a reload does not re-apply.
- Amp ADSR group is contiguous on one row (Shape slots 1-4). The partial AR on Main (slots 5-6) also draws.
- help.json: all 128 lines fit (widest right edge x=123 of 127), all glyphs are in the font, and top-level `children` is present.
- No em dashes in README or help.json.
- build.sh ships help.json and canvas.js.

---

## 3. help.json and README mismatches (each one)

### help.json (`src/help.json`)
1. **First sound, lines 25-37**: "Dive into Corpus from the root menu" and "Back to root". On the grid there is no root menu. Corpus is page 4 (Main, View, View - 2, Corpus); you jog to it. It also misses that a fresh instance auto-loads the first file of the first `UserLibrary/Samples` sub-folder, so pads may already play.
2. **Scope, lines 99-114**: "Corpus > Scope" is wrong. Scope is on **Mod & Set** (knob 6). "Folder (default)" is also wrong: the default is **File**.
3. **Map + Mod, line 127**: "Mod page" should be "Mod & Set".
4. **Note Match, line 81**: "Play page > Note" should be **Note Match** (cell NMATCH).
5. **Filter, line 138**: "Out or root > K8" should be Out K1 / Main K8.
6. **Pressure, lines 154-166**: the "Vel Amount" label is "Velocity Amt". The default (Density) is not stated.
7. **Missing topics**:
   - the View map page and recipe browser (click to browse, each step loads a recipe and resets play params, 8 recipes);
   - Level Norm, Pos Jitter, Smooth;
   - Rescan needed after slicing changes (M2);
   - amp env only in Notes mode, and Amp Rel also sets the filter-env release (M3).

### README.md
1. **L10**: Version 0.1.0 (tagged). The code is well past the tag.
2. **L57**: "Schwung v0.9.x or later" should be **1.3.0+** (H4).
3. **L81**: "On the Corpus page (the landing page), select Source (knob 1)". The landing page is **Main** (live knobs), and Corpus is page 4. "By default Concat analyses that file's whole folder; set Scope = File (just below Source)" is wrong on two counts: the default **is** File, and Scope lives on **Mod & Set**.
4. **L82, L219**: "default 128 MB pool" should be **512 MB**.
5. **L111**: "Pages are reached from the Corpus (root) menu: Map, Mod, Grain, Shape, Play, Out". Actual: Main, View (map + recipes), View - 2, Corpus, Map, Mod & Set, Grain, Shape, Play, Out, then My Presets and Module.
6. **L113-124**, the Corpus table:
   - The heading "(root)" is wrong.
   - Scope is listed but is not on this page.
   - Labels differ: Pool is really Pool Cap, Slice is Slice Mode, Min Sl / Max Sl are Min Slice / Max Slice, Sil Gate is Silence Gate. On device they draw POOCAP / SMODE / MISLICE / MSLICE / SIGATE.
   - Defaults: Scope **File** (not Folder), Pool **512** (not 128), Min Slice **20** (not 60), Max Slice **2000** (not 400).
7. **Missing Main page table**: X Pos, Y Pos, Density, Grain Size, Amp Atk, Amp Rel, Poly, Filter.
8. **Missing View section**: the map canvas, what the dot, ring and trail mean, the recipe browser and its 8 recipes, and View - 2.
9. **L134**: Radius "how far around the target grains may wander". Actually it widens the random pick among the K nearest (no effect at K=1) (L3).
10. **L138-143**: page "Mod" is "Mod & Set". Smooth, Pos Jitter (default 0.15) and Scope are missing. "LFO Shp" is labelled LFO Shape (LFOSHP). LFO depth is fixed at 0.45 (worth a line).
11. **L157-162**, Shape: Filt Env default **0.45** (not 0.5), so by default the filter env pulls slightly toward LP. Filter-env release is **Amp Release** (undocumented).
12. **L169**: Pressure default **Density**, not Proximity.
13. **L171**: Vel Amt default **0.8**, not 1.0.
14. **L168**: "Note" should be Note Match.
15. **L174-182**, Out: missing **Level Norm** (0 to 1, default 0.5, pulls each grain's level toward the corpus median loudness, gain clamped 0.25x to 4x).
16. **L196-203**, "Recipes": these are user tips, not the 8 factory recipes. Rename to "Tips" and add a real Recipes section: Init, Cloud, Stutter, Drone, Scatter, Pitched, Mosaic, Sequence, each a play-scope preset that never touches Corpus.
17. **L233**: "reload with restart-move.sh, then power-cycle" contradicts install.sh ("power-cycle if module.json changed"). Pick one.
18. **L237-253**, Project layout: missing `src/canvas.js` and `src/help.json`.
19. **L261**: the Troubleshooting "float 0.00-1.00 / None" entry should also say "or Schwung older than 1.3.0".
20. Behaviour worth adding: the auto-load of a default corpus on a fresh instance, and "slicing changes need Rescan" (M2).

Verified correct in README: 16 held notes and 128 grain voices; 16 k-means clusters; ~23 ms Mosaic hop (1024 @ 44.1k); in-house radix-2 FFT; 128 MB per-file skip; recursive folder scan; OOM guard formula; cache path; install commands (match install.sh); LFO Rate cycles-per-beat examples.

---

## 4. Schwung releases after 1.3.0: breaking or changing for Concat

| Release | Relevant? | Notes |
|---|---|---|
| 1.3.1 | No | Widget-load latch fix for `canvas.js` custom *widgets*. Concat ships a page drawer, not a `widgetKind`. |
| 1.3.2 | No | USB-C audio routing hotfix. |
| 1.3.3 | No (indirect) | Pads stuck after leaving a module editor. Concat never claims pads, so nothing to change. |
| 1.4.0 | No | Boot targets, Spkr EQ, utility install path. |
| 1.5.0 | Check | **Line In is no longer gated after a second of silence (#515)** and **Resample on Mix with a Line In slot no longer feeds back (#547)**: re-test Mosaic + Dry Mix on 1.5.0, especially quiet-input behaviour. **Knob pages: faster re-planning, Filter and LFO pictures corrected (#511)**: re-check the Main AR and Shape ADSR graphics, and the LFO group if you add `kind`. **My Presets / Module pages on module-drawn grids**: already present in Concat's plan. |

Nothing in 1.3.1 to 1.5.0 breaks how Concat is built, declared or rendered. The contract uses no removed field. (`visible_if` is still supported, and Concat removed it deliberately. Automation lanes were pulled from main in 1.5.0, and Concat does not use them.)

## 5. New capabilities worth adopting (doc reference, then the concrete Concat use)

1. **`access:"read"` / `access:"write"`** (MODULES.md "`access`"): Analyze becomes a dotted readout (H5), Rescan a one-shot trigger (M1).
2. **`live:true` + `<key>:effective`** (MODULES.md "A parameter the module drives itself"): X Pos / Y Pos dials follow the LFO, Zone and cluster target. Serve the same effective point in `map_live` (H3).
3. **`short_name` / `short_options`** (MODULES.md "`short_name`", "Naming a parameter"): fixes L7. The names also show on the new 1.5.0 **E16/EC4 surfaces**.
4. **`display:"big"`** (MODULES.md "`display: \"big\"`"): Analyze %, K Nearest, Polyphony, Repeat Avoid.
5. **Explicit `viz`** (MODULES.md "Parameter visualisations"): `kind:"lfo"` for the LFO pair (M4), `viz:false` on Level Norm, explicit fader on Volume, explicit sample or false on Source (L5, L6).
6. **Canvas `extra_keys` 3rd/4th slot** (MODULES.md "Custom UI pages"): add `analyze` for an analysing state (M6), and optionally a `map_axes` string ("Bright/Pitch" or "PCA") to label the axes.
7. **`pad_layout`** (MODULES.md "Declaring your performance surface"): declare `"chromatic"` (Notes is the default mode) so movy-style sequencers seat Concat correctly. It is a top-level `ui_hierarchy` key, so it goes in the DSP string.
8. **Gates via `visible_if` on a derived key** (MODULES.md "The gate does not need a cell"; 1.5.0 "A module's own mode can choose which pages show", #533): optional. For example, hide Note Match in Map/Mosaic, or Dry Mix outside Mosaic. Listed only because the release names it. Concat removed visible_if deliberately, so this is not a recommendation to reverse that.
9. **Controller CC map (Shift+Vol+Sample) and Controls page in Schwung Manager** (1.5.0): no code needed. Worth a README line: X Pos / Y Pos / Radius / Density are good learn targets. Replaces the "host-LFO target" advice.
10. **Enterable canvas pages** (CANVAS_PAGES.md, MODULES.md "`enterable` on an `as_page` canvas", 1.5.0 #520/#534): not usable on the current View page, because it is mutually exclusive with `preset_browser`. If you ever want the jog to *move the map point* or step clusters on the map, you would split the recipe browser back out into a plain preset page and make the map an enterable `as_page`. That is a reversal of the current two-page design, so it is only an option.
11. **Module Presets** (MODULES.md "Making a module compatible with Module Presets"): already compatible (`state` is self-contained and includes corpus_file). Worth a README line: My Presets saves the corpus path too, and a preset recalled on another Move whose path does not exist falls back to... nothing (the corpus_file is kept and the analyst finds no audio). Consider falling back to auto-pick when the restored path is missing.
