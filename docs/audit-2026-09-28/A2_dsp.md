# A2: Concat DSP engine audit

Scope: `src/dsp/{grain,query,corpus_engine,corpus,analysis,ingest,concat}.c/.h`, including the uncommitted diff (Level Norm, Pos Jitter, equal-power seam). This was a read-only review. I did not build, run, or ssh anything.

CPU figures assume a Cortex-A72 at 1.5 GHz (CM4 default), so 1 ms is about 1.5 M cycles. The usable budget is about 2.37 ms (3.5 M cycles) per 128-frame block. Build flags are `-O2 -ffast-math` (scripts/build.sh:14).

Severity scale: BLOCKER / HIGH / MED / LOW. Confidence is given as H, M or L.

---

## Part 1: Bugs

### B1. BLOCKER (conf H): the kNN query can blow the render budget, and Notes-mode chords run 16 queries in one block
- `grain.c:85` calls `query_select` for **every** spawn. `query.c:86-98` does a full linear scan over N slices. Each slice costs `in_ring()`, up to 16 compares (`query.c:22-25`, run *before* the distance test), plus a 36-dim `wdist2` (`query.c:60-64`). That is roughly 60–170 cycles per slice, depending on vectorisation and FMA latency chains.
- In Map mode the weights are zero on 34 of 36 dims (`query.c:48-54`). The scan still multiplies all 36.
- N is large in practice. A 512 MB pool (the default, `concat.c:118`) holds about 268 M samples. With onset slices of 100–300 ms that is 20k–60k slices. Fixed mode stores overlapping 50%-hop slices *twice* (`corpus.c:162`, and each slice is copied separately at `corpus_engine.c:426-431`).
- At N = 20k, one query costs about 1.2–3.4 M cycles, i.e. 0.8–2.3 ms. That is a full block by itself.
- **Notes mode:** each held note has its own scheduler (`concat.c:1035`). Note-on sets `nv->accum = 0.0` (`concat.c:590`), so a chord pressed in one block has phase-aligned schedulers. All N held notes then spawn, and therefore query, in the **same** block, every ~3.4 blocks at density 100.
  - 8 notes × N=10k gives about 8 × 0.5 ms = 4 ms, which means repeated xruns.
  - Pressure→Density (`concat.c:959`) makes this worse.
- **Fix (combine these):**
  1. In `query_select`, test `in_ring` only for candidates that would enter the top-K (`if (nb<k || d<bd[k-1]) { if (in_ring(q,i)) continue; ...insert }`). Better still, keep a per-slice `uint32_t last_played_stamp[]` array so the ring test is O(1).
  2. Build an active-dims list from `w` once per call: `int dims[NDESC], nd=0; for d: if (w[d]!=0) dims[nd++]=d;`. Map mode then costs 2 MACs per slice instead of 36.
  3. For Learned mode, precompute per-slice `(p1,p2)` at load (2×N floats) and query in 2-D (see B9).
  4. Randomise the scheduler phase at note-on (`nv->accum = rand01();`) and cap spawns per block globally (e.g. `max_spawns_per_block = 4`, carrying the remainder in `accum`).
  5. Optional: cache the K-best list per target per block. Map and Mosaic targets are constant within a block, so only the random pick needs redoing.

### B2. HIGH (conf H): the grain bus is hard-clipped to int16 *before* the filter, saturator and dry mix, with no overlap-gain compensation
- `grain_mix` sums every grain in float, applies width and volume, then `clamp16()` into `out_lr` (`grain.c:166-170`, `grain.c:229-231`). The post chain then re-reads that int16 (`concat.c:1095`).
- Consequences:
  - (a) Any grain sum above 1.0 is hard-clipped. The tanh "saturation" (`concat.c:1104`) can only ever see a pre-clipped signal, so it cannot soften anything.
  - (b) Resonant LP/HP (Q up to 10, `concat.c:1084`) boosts *after* the clip, then hard-clips again (`concat.c:1106`).
  - (c) The signal is quantised twice.
- There is no normalisation by expected overlap. Per note, overlap ≈ `density × grain_s`:
  - "Cloud" recipe: 60 × 0.4 s = 24 grains.
  - "Drone": 18 grains.
  - Notes mode multiplies by the number of held notes.
  - Level Norm (default 0.5) adds up to ×2.5 on top (see B4).
- Hann-windowed uncorrelated slices at about −14 dBFS RMS already sum past 0 dBFS at around 16–24 overlaps.
- **Fix:**
  - Make `grain_mix` write `float mixbuf[2*frames]` (a static or instance scratch of 256 floats) and run the post chain in float. Convert to int16 once, at the end.
  - Apply `gain_comp = 1/sqrtf(fmaxf(1, density*grain_s*nvoices_active))`, smoothed per block.
  - Put a soft limiter or tanh safety stage last, not a hard clamp.

### B3. HIGH (conf H): Pos Jitter makes "fitting" grains wrap mid-grain, and reverse jitter is inverted. A pre-existing wrap bug when `rate > 1` has the same cause
- The loop/crossfade decision uses **output length only**: `if (glen > g->len)` (`grain.c:132`). It runs *before* the rate is computed (`grain.c:142-146`). The span actually read is `glen*rate`.
- Pre-existing bug:
  - With `glen ≤ len` but `rate > 1` (pitch up, Pitch-mode retune, jitter), `xfade=0` and `period=len`.
  - The read runs past `len` and wraps (`grain.c:224`) with no crossfade. That produces a hard discontinuity in the middle of the window, where the envelope is ~1.
  - Example: grain 120 ms, slice 150 ms, +7 st. The read span is 180 ms, so it wraps at 100 ms into the grain.
- New with Pos Jitter:
  - Default 0.15 moves `start` up to 0.15·len into the slice (`grain.c:153-157`). Any fitting grain with `start + glen*rate > len` now wraps mid-grain.
  - With 120 ms grains this hits every slice between about 120 and 141 ms at rate 1, and far more when pitched up.
- Reverse is wrong:
  - `start = (period-1) + j*period` is reduced by fmod to about `j*period - 1`. A reverse grain now starts near the **head** and immediately wraps to the tail, which is a hard jump.
  - The jitter should *subtract* for reverse.
- **Correct behaviour:** decide looping from the read span, and jitter only inside the slack.
  ```c
  /* compute g->rate FIRST (move grain.c:142-146 above the xfade block) */
  double span = (double)glen * g->rate;
  if (span <= (double)(g->len - 2)) {                 /* fits: never wraps */
      g->xfade = 0; period = g->len;
      double slack = (double)(g->len - 2) - span;      /* ≥ 0 */
      double off = (gp->pos_jitter > 0.f) ? g_rand(ge) * gp->pos_jitter * slack : 0.0;
      start = g->reverse ? (double)(g->len - 1) - off : off;
  } else {                                            /* loops: seam xfade */
      int xf = (int)(0.020f * sr); if (xf > g->len / 2) xf = g->len / 2;
      g->xfade = xf; period = g->len - xf;
      double off = g_rand(ge) * gp->pos_jitter * period;
      start = g->reverse ? (double)(period - 1) - off : off;
      start = fmod(start, (double)period); if (start < 0) start += period;
  }
  ```
  - `-2` guards the `ip1` interpolation sample and rounding.
  - In the non-loop branch, clamp `pos` to `[0, len-1]` instead of wrapping. That is a belt-and-braces guard so a fitting grain can never wrap.
  - Scale jitter by `slack`, not `len`, so Pos Jitter never forces a loop.

### B4. HIGH (conf M): Level Norm can push transients about 12 dB into the hard clamp, and the linear lerp is biased toward boost
- The data exists and survives the cache.
  - `median[]`/`iqr[]` are written at `corpus_engine.c:125-126` and read back at `corpus_engine.c:90-91`.
  - The inversion `raw = qn*iqr + median` (`grain.c:113`) is exact up to float rounding, including when the IQR is floored to 1e-6.
  - The concern raised in the brief, that an index loaded from cache would lack these fields, does **not** apply.
- Problems:
  1. **Peak blind.** `D_LOUD_MEAN` is the mean of per-frame RMS. An onset slice with a sharp attack and a long quiet tail has a low mean, so it gets up to ×4 (+12 dB), and so does its attack peak. Combined with B2, that is hard clipping.
     - Fix: also invert `D_LOUD_PEAK` and cap it: `float pk = qn[..PEAK]*iqr[PEAK]+median[PEAK]; factor = fminf(factor, 0.9f / fmaxf(pk,1e-6f));`
  2. **Interpolation in the linear domain** (`grain.c:117`). At level_norm 0.5, factor 4 gives ×2.5 (+8 dB) and factor 0.25 gives ×0.625 (−4 dB). The half-way setting is not half-way in dB, and it favours boosting.
     - Fix: `gnorm = powf(factor, gp->level_norm);`. This runs once per spawn, so the cost is nothing.
  3. **Clamp range.** ±12 dB is wide for a default-on feature. Suggest cut to 0.25 (−12 dB) and boost to 2.0 (+6 dB). Boosting near-gate slices mostly raises noise.
  4. **RMS is not perceived loudness.** Bass-heavy slices get over-boosted relative to bright ones. Acceptable for v1. A K-weighted RMS computed at analysis time and stored per slice is the proper fix.
  5. **Mosaic interaction.** Mosaic matches `D_LOUD_MEAN` (weight 1, `concat.c:892`), then Level Norm pulls every grain back to the corpus median. This flattens the performer's dynamics. In Mosaic, scale toward the *input's* raw RMS instead (`ref = live_rms`).
  - Simpler and more robust overall: store raw `rms` and `peak` in `cslice_t` rather than inverting. This needs a cache-version bump.

### B5. HIGH (conf H): the RobustScaling IQR floor of 1e-6 lets degenerate dims (pitch above all) dominate every full-space distance, the PCA and the k-means
- `corpus.c:177`: `iqr = max(q3-q1, 1e-6)`.
  - `D_PITCH` is 0 for unvoiced slices (`corpus.c:73-76`). In a mostly unvoiced corpus (drums, noise, speech), Q1 = Q3 = 0, so iqr = 1e-6.
  - A voiced slice at 440 Hz then gets qn ≈ **4.4e8**.
  - The same can happen to `D_PITCH_CONF`, `*_STD` of fixed-mode drones, and `D_FLATNESS_*` on synthetic material.
- Effects:
  - The concatenation cost `slice_dist2` (`query.c:67-72`, all dims) becomes a pitch-only metric. Other dims fall below float resolution (1e17 + 10 ≈ 1e17).
  - `pca_target` (`concat.c:909-913`, w=1 on all dims) and the pad cluster jump (`concat.c:1052`) are dominated by that dim too.
  - PCA axis 1 collapses onto pitch (`corpus_engine.c:218-259`), so the "Learned" map is a voiced/unvoiced split.
  - k-means (`corpus_engine.c:154-194`) clusters by pitch only.
  - Notes→Pitch mode skips the dim entirely when `iqr ≤ 1e-6` (`concat.c:1018`). The target is then all-zero weights, so every distance is 0 and it always plays the first K slices in file order.
- **Fix:**
  - In `concat_robust_fit`, if `q3-q1 < 1e-3*(max-min)` fall back to `(max-min)/2`, else 1.0. This is sklearn's `_handle_zeros_in_scale` behaviour.
  - Clamp qn to ±8 after applying.
  - Store pitch as MIDI (`69+12*log2(f/440)`) with unvoiced slices set to the voiced median, plus conf. The Pitch-mode target then uses semitones.
  - This changes analysis output, so bump `CACHE_VER` to 5.

### B6. HIGH (conf H): ingest can OOM the device. Peak memory is not bounded by the pool cap or the 350 MB reserve
- `ingest.c:160-165` caps **file bytes** at 128 MB. Decoded peak is `frames·ch·4` for interleaved f32 plus `frames·4` for mono (`ingest.c:172-188`), all held at once. Then `resample_mono` adds another buffer.
  - 30-min 320 kbps DJ-mix MP3 (72 MB): about 635 MB f32 + 318 MB mono ≈ **950 MB**.
  - 128 MB MP3 at 128 kbps (about 2.2 h): about **2.8 GB**. dr_mp3's growing realloc makes transients worse.
  - 128 MB 8-bit mono WAV: 537 + 537 MB ≈ 1.07 GB.
- This sits on top of the pool (default cap 512 MB, `concat.c:118`, versus CLAUDE.md's 128 MB) and the old index, which stays resident until the swap (`concat.c:500`).
- With no swap, the OOM killer can take MoveOriginal. The guard in `arm_analyst` (`concat.c:450-455`) only sizes the pool.
- **Fix:**
  - Stream-decode in chunks (`drwav_init_file` / `drflac_open_file` / `drmp3_init_file` + `*_read_pcm_frames_f32(…, 4096, chunk)`), downmixing into mono as you go.
  - Pre-check `totalPCMFrameCount` (dr_mp3: `drmp3_get_pcm_frame_count` or a duration cap).
  - Cap decoded mono per file at, say, `min(10 min, remaining_pool_samples*base_stride)`.
  - Re-read MemAvailable in the analyst before each file and stop when it falls under the reserve.
  - Restore the `pool_mb` default to 128.

### B7. MED (conf H): per-note amp ADSR is block-stepped, which zippers and clicks
- `adsr_advance` updates `amp_level` once per block (`concat.c:897-906`, `concat.c:1007`). Grains multiply by `*g->amp` per sample (`grain.c:220`), so every grain of a note sees a staircase at 344 Hz.
- With short times (ENVT min 2 ms, `concat.c:1000`):
  - Attack jumps 0 → 1 in one block.
  - Release at 2 ms multiplies by `exp(-3·2.9/2) ≈ 0.013` in one step.
  - Both are audible clicks on note-off and note-on.
- The filter envelope has the same problem, in coefficients (B12).
- **Fix:** keep `amp_prev` and `amp_level` per note. Pass both through `grain_amp_t` and interpolate in `grain_mix` with `a = prev + (cur-prev)*(i+1)/frames`. Alternatively run the ADSR per sample for active notes (16 × 128, trivial cost).

### B8. MED (conf H): hard grain cut-offs from voice steal, same-note retrigger, and corpus swap
- **Steal** (`grain.c:92-99`): when poly is saturated it takes the voice with the smallest remaining length and restarts it immediately.
  - With density × size > poly (e.g. 100 g/s × 2 s = 200 wanted vs 48), victims still have most of their window left, so every steal clicks. That is up to 100 clicks/s.
  - Fix: mark the victim `fading` with a 64-sample linear fade multiplier (a separate `fade_left` counter, *not* by editing `grain_len`, which warps the window). Then either drop the new grain or put it in the next free slot.
  - Better: size the effective density so that `density*grain_s ≤ poly`.
- **Retrigger or steal of a note voice:** `nv->gen++` (`concat.c:590`) makes `grain_mix` set `active=0` at once on all of the old note's grains (`grain.c:187`). That is an instant cut at arbitrary amplitude.
  - Fix: on gen mismatch, switch the grain to a 64-sample fade from its last amp value instead of muting.
  - Note-slot steal is always slot 0 (`concat.c:587`). Steal the quietest releasing voice instead.
- **Corpus swap:** `grain_flush` (`grain.c:20-23`, called at `concat.c:929`) kills all grains in one sample.
  - The old pool is guaranteed valid for about 60 ms (`concat.c:501`). On detecting the swap, render one more block of the *old* grains with a 1-block linear fade-out, then flush.

### B9. MED (conf H): Learned-map queries are biased against off-plane slices and cost the full 36 dims
- `pca_target` (`concat.c:909-913`) reconstructs a 36-D point on the PCA plane with w=1 everywhere.
  - Distance² = in-plane distance² + that slice's orthogonal residual², and the residual is independent of the target.
  - Slices far from the plane (the interesting outliers) are never selected, whatever the X/Y.
  - With B5, the residual is dominated by pitch.
- **Fix:** at load (worker thread), store `float proj[N][2]` normalised 0..1 (`map_project` already computes this, `concat.c:684-703`). In Learned mode, run kNN on the 2-D projection. This is correct and about 18× cheaper.

### B10. MED (conf H): the loop-seam crossfade also runs at grain **start**, which replaces onset transients
- `grain.c:198` applies the blend whenever `p < xf`, including the first pass from `pos=0`. At `p=0` the output is 100% **tail** (`cosf(0)=1`).
- A looping grain therefore spends its first 20 ms playing the slice's end, not its attack. For onset-sliced material that removes the transient that defines the slice.
- **Fix:** add `int wrapped` to `grain_t`, set it in the wrap loops, and apply the seam only when `wrapped`.
  - Reverse grains start at `period-1` and hit the seam naturally before wrapping, which is correct as it is.
  - Alternatively use the conventional end-of-loop form: blend in `[period-xf, period)` with the head.

### B11. MED (conf M): equal-power seam (new) is +3 dB for correlated material, and costs two transcendentals per sample
- This is the brief's point 5. Head `[0,xf)` versus tail `[period,len)`:
  - Onset slices are mostly uncorrelated, so equal-power is right there.
  - Fixed-mode slices of drones or tones and short periodic slices are often highly correlated. sin+cos peaks at √2 (+3 dB) mid-seam.
  - With short slices (`xf = len/2`), the seam covers half of every loop period, so the bump becomes periodic AM at the loop rate: a buzz. Anti-correlated seams dip.
- CPU: `cosf`+`sinf` per voice-sample in the seam (`grain.c:208-209`). At 128 voices and 50% seam occupancy that is about 0.4 ms.
- **Fix:**
  - At analysis time, compute the normalised correlation `r` of head versus tail for `xf = min(882, len/2)` and store it per slice (`cslice_t.seam_r`).
  - Use the correlation-adaptive law (solve `g1²+g2²+2r·g1·g2 = 1` along a sine/cos shape). A pragmatic version: `r > 0.5` uses linear, `r < 0` uses equal-power, and in between interpolate the exponent.
  - Read the fade shape from a 256-entry quarter-sine table instead of `cosf`/`sinf`.

### B12. MED (conf H): DJ filter state goes stale across the bypass zone, and coefficients change per block
- In bypass (`cutpos` in 0.48..0.52, `concat.c:1076-1078`) the SVF states are frozen. On re-entry, `svf_ic1/ic2` hold values from the last time the filter was active, possibly seconds ago, and that causes a click.
- This happens on **every note** with default settings: cutoff 0.5 + filt_amt 0.45 sweeps into LP and back to bypass.
- LP at the 0.48 edge is 16 kHz (and resonant), not unity, so the bypass boundary is itself a small step.
- Coefficients (`g = tanf(...)`) update once per block. A 2 ms filter attack sweeps several octaves in one step, which zippers.
- **Fix:**
  - Always run the SVF. Compute LP, HP and dry, then crossfade dry↔LP over `cutpos ∈ [0.40,0.50]` and dry↔HP over `[0.50,0.60]`.
  - Interpolate `g` per sample, or per 16-sample sub-block, from the previous block's value.
  - Filter release reuses `P_AMP_REL` (`concat.c:1067`). Intentional? Document it or add `filt_rel`.

### B13. MED (conf H): `MAX_SLICES_FILE = 4096` silently truncates long files
- The `ranges` buffer is fixed at 4096 (`corpus_engine.c:15, 366`). Onset and fixed slicers stop at `max_slices` (`corpus.c:141-143`, `corpus.c:162`).
- A 15-minute file at about 150 ms per slice gives about 6000 slices, so the last third is never analysed. This also defeats the "spread across the corpus" decimation.
- **Fix:** grow `ranges` with realloc inside the slicers (return a count and a pointer), or slice in windows of 4096.

### B14. MED (conf M): stale cache when `pool_mb` increases
- `corpus_signature` (`corpus_engine.c:40-57`) hashes the slicing params but not the pool cap. `cache_load` rejects only `pool_len > cap` (`corpus_engine.c:74`).
- A corpus decimated under a 64 MB cap is reused unchanged after the user raises Pool Cap to 512 MB. They never get the fuller corpus without pressing Rescan.
- **Fix:** store the build-time `pool_cap_samples` and `base_stride` in the header. Reject when the stored cap < current cap and the stored build was decimated (`stride>1` or dropped slices).

### B15. MED (conf M): I/O and locking in `set_param`
- `arm_analyst` (`concat.c:439-459`) does `open`/`read` of `/proc/meminfo` and takes `inst->lock` from `set_param`. It is also called from `create_instance` via state restore.
- Code comments say `set_param` "runs on the SPI callback" (`concat.c:344`). If so, this is a syscall plus possible mutex contention on the RT thread.
- **Fix:** snapshot only the params under the lock (or use a lock-free seqlock). Move `mem_available_mb()` and the cap computation into `analyst_thread` after dequeue.

### B16. MED (conf M): Zone mode maps Move pads (notes 68–99) to only the right half of the map
- `zx = clamp((note-36)/60)` (`concat.c:1024`). Pads 68–99 give 0.53–1.05, so the top four pads all clamp to x = 1.
- This assumes pads arrive as 68–99, as design-spec.md:174 says. Verify on device.
- **Fix:** `zx = (note - 68) / 31.0f` when the note is in the pad range, or map by `note % 32`.

### B17. LOW–MED (conf M): ABA use-after-free window on corpus swap
- Swap detection is by pointer compare (`concat.c:929`). If render does not run between two swaps (for example two quick cache-hit loads while the slot is not rendering), the third index can be `calloc`'d at the freed address of the first.
- Then `ix == ge_ix`, no flush happens, and grains read the freed pool.
- **Fix:** add a `uint32_t gen` to `corpus_index_t` (a global counter) and compare generations.
- Related (LOW): a failed rebuild (`ni == NULL`, `concat.c:500-502`) discards the working corpus. Keep the old one unless the user selected a new path.

### B18. LOW (conf H): cache validation gaps
- `pool_off + len` is computed in `int` and can overflow on a corrupt header (`corpus_engine.c:106-107`). Use `(long)`.
- Floats (`qn`, `median`, `iqr`, `pca_*`, `slices[].pitch`) are not validated.
  - A NaN in `slices[].pitch` or `iqr` flows into `g->rate` and `g->gain`. `grain.c:145-146` clamps do not catch NaN, and under `-ffast-math` NaN comparisons are undefined.
  - It cannot hang, because `while (pos >= period)` is false for NaN, but it produces garbage.
  - Validate with a bit-level finite check, since `isfinite()` may be folded away under `-ffinite-math-only`: `((u>>23)&0xff)!=0xff`. Or compile corpus_engine.c with `-fno-finite-math-only`.
- `fclose()` return is ignored (`corpus_engine.c:136`). An ENOSPC at the final flush still renames a truncated file. It is safe because the next load's size checks reject it, but it gets rebuilt every time.
- Two instances analysing the same folder write the same `.tmp`. Use `%s.%d.tmp` with the pid or instance pointer.

### B19. LOW (conf H): smaller correctness items
- **YIN floor:** `tau_max` is capped at `W-1 = 511` (`analysis.c:171`), so the effective min pitch is 86 Hz, not the requested 50 Hz. Bass slices get octave errors, and so does the Pitch-mode retune. Also clamp the retune ratio to ±2 oct (`grain.c:144`) so misdetections cannot drive rate 32.
- **Rate cap 32** (`grain.c:146`): linear interpolation at rate > 2 aliases heavily. Cap at 8 unless mip-mapped (see S3).
- **Non-PCA `map_project`** (`concat.c:697-701`): raw robust qn is clamped to [0,1] instead of being normalised by `nav_min/max`, so the fallback scatter piles up at the edges.
- **`get_param("state")`** (`concat.c:843-847`): the last `n += snprintf` can return `n > buf_len`. Clamp the return to `buf_len-1`.
- **Aftertouch:** note-off zeroes the global `aftertouch` even while other pads are pressed (`concat.c:599`).
- **Pitch bend** only affects Notes Pitch/Off (`concat.c:1012-1013`). Map, Zone and Mosaic ignore it, contrary to CLAUDE.md "bend (±transpose grains)".
- **Sat threshold** (`concat.c:1104`): switching from bypass to `tanh(x·~1)·1.082` at sat = 0.001 is a +0.7 dB step. Crossfade from sat 0 to 0.05.
- **Race:** `analyst_thread` writes `inst->corpus_file` (`concat.c:480`) while `get_param` and `set_param` read or write it on other threads. Use a benign copy under `lock`.
- **Mel filterbank rows** are not normalised by mel spacing, unlike dsp-notes §1 and FluCoMa. This is a constant log offset per band, so it is harmless after RobustScaling. Noted only for parity.
- **Resampler:** Catmull-Rom with no anti-alias pre-filter when downsampling (`ingest.c:135-155`). Sources at 88.2k/96k/192k fold ultrasonic content into the audible band. Put a pre-lowpass at 0.45·out_sr before the Hermite when `ratio < 1`.

### Verified OK (no action needed)
- Integer ranges:
  - Pool offsets are `int`, and a 512 MB pool is 268 M samples, under 2^31.
  - `a_pool_samples` ≤ 268 M.
  - `glen` ≤ 4000 ms × 44.1k = 176k.
  - `raw`/`qn` index math uses `size_t` casts.
- **Wrap loops** (`grain.c:224-225`): `period ≥ 1` is guaranteed (`grain.c:189`), rate ≤ 32, and `pos` is finite, so there are at most 32 iterations per sample. Worst case: 128 voices × 128 samples × 32 ≈ 0.35 ms. Bounded. Use `fmod` when `rate > period` if you want it tighter.
- **Denormals:**
  - SVF has a +1e-20 guard.
  - ADSR release snaps to 0 below 5e-4.
  - Grains have no feedback.
  - NaN cannot persist into `svf_ic*`, because its input comes from int16. B2's float bus would remove that firewall, so add the B18 finite checks at the same time.
- `query_set_repeat_avoid` keeps at least 1 slice selectable. The `nb == 0` fallback terminates.
- The `map_live` ring index is non-negative and `cap`-guarded.
- `build_map_pts` stays within 4096 bytes.
- `chain_params` truncation is guarded.
- dr_mp3 0.7.4 skips ID3v2 and handles APE/ID3v1 via `streamLength`. The `frames > INT_MAX` and `ch == 0` guards are present.

---

## Part 2: CPU budget, worst case per 128-frame block

| Stage | Worst case | Estimate |
|---|---|---|
| `grain_mix`, no seam | 128 voices × 128 samples ≈ 16.4k voice-samples at about 55 cycles each (int `%`, 2 fdiv, `floor`, 2 loads, branches) | about 0.9 M cycles ≈ **0.6 ms** |
| `grain_mix`, seam cost | +cosf/sinf/tail interpolation at about 50% occupancy for short looping slices | +0.6–0.9 M ≈ **+0.5 ms** |
| `query_select`, per spawn | N=10k: 0.6–1.7 M cycles. N=30k: 1.8–5 M cycles | **0.4–1.1 ms / 1.2–3.4 ms** |
| Spawns per block | Map/Mosaic ≤ 1 (density ≤ 100 gives 0.29/block). Notes: up to 16 aligned (B1) | **×16** |
| Mosaic target | FFT 1024 + dense 40×513 mel (double) + 512 `log` every 8 blocks | about 0.1 ms per 8 blocks |
| Post chain | 256 × (SVF + tanhf) | < 0.03 ms |

Verdict:
- Map mode with a large corpus: about 1.1 ms (mix) + up to 3.4 ms (one query) goes **over budget** above about 15k slices.
- Notes mode with chords: over budget even at N ≈ 2k.
- After the B1 fixes, a query costs about 2–4 cycles per slice in Map/Learned mode (2-D). With Mosaic's 17 dims and early-out, 30k slices take about 0.1–0.3 ms.

Cheap mix-loop wins:
- Precompute `inv_glen` per grain.
- Replace `(int)p % len` with a plain `(int)p`, since `pos < period ≤ len` is already guaranteed.
- Replace seam `cosf`/`sinf` with a table.
- Loop voice-outer into a float accumulator so per-voice constants stay in registers.

---

## Part 3: Sound-quality improvements (not bugs)

- **S1. Overlap-normalised output.** Apply `1/sqrt(overlap)` auto-gain (B2), plus a final soft limiter. Without this, density and grain-size knobs are also volume knobs.
- **S2. Equal-power pan.** Pan is linear (`grain.c:160-162`, centre 0.5/0.5 = −6 dB per channel). For uncorrelated grains, centred grains are 3 dB quieter in power than panned ones, so Pan Spread changes loudness. Use `panl = cosf((pan+1)*π/4)`, `panr = sinf(...)`, computed at spawn at no per-sample cost.
- **S3. Interpolation.** Linear interpolation (`grain.c:195-196`) is fine at rate ≤ 1, but aliases above that, and badly at the +24 st knob range and the Pitch-mode retunes. Use 4-point Hermite at rate ≤ 2. For higher rates, build a half-band decimated copy of the pool at load (+50% memory for one octave level) and read from it.
- **S4. DC blocker.** Add a 5 Hz one-pole HP on the output. Looping DC-offset slices and asymmetric tanh leave DC, which eats headroom.
- **S5. Mosaic.**
  - Gate grains when the input RMS is below a threshold. Right now silence still queries, and plays the quietest slices at a normalised level.
  - Consider onset-triggered re-query ("fence", dsp-notes:102).
  - Follow input loudness instead of Level Norm (B4.5).
- **S6. Nav axes.** Min-max over robust qn (`query.c:30-36`) leaves skewed maps mostly empty. Rank or quantile normalisation per axis spreads slices evenly under the X/Y knobs.
- **S7. k-means.** Init is evenly spaced by file order (`corpus_engine.c:163-164`). k-means++ (deterministic RNG) gives more distinct pad regions. Re-seeding empty clusters on the last iteration leaves an arbitrary slice as a centroid, so skip the reseed when `it == ITERS-1`.
- **S8. Fixed-mode memory.** 50%-overlap fixed slices duplicate audio. Store them as overlapping views (`pool_off` of slice k+1 = `pool_off_k + hop`) when stride == 1. This doubles audible corpus per MB.
- **S9. Level Norm default.** Defaulting to 0.5 silently changes existing presets and states. It is appended at the end of the table, so old states load 0.5. Consider default 0 for back-compat, or bump the recipes.
- **S10. "Rolloff" label.** "Rolloff" is actually ZCR (`concat.c:371`). Rename it, or implement a real 85% rolloff.

---

## Suggested fix order
1. B1: query cost (early-out ring, sparse dims, 2-D learned query, stagger and cap spawns).
2. B2: float bus, overlap gain, soft limit.
3. B3: span-based fit/loop decision and correct jitter. This is small, contained in `spawn_grain`, and fixes a pre-existing click.
4. B6: streaming decode and memory guard.
5. B5: robust scaling floor, MIDI pitch, `CACHE_VER` 5.
6. B4 (powf law, peak cap), B7/B8/B12 (click sources), B10/B11 (seam).
