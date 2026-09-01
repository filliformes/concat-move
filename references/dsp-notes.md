# Concat — DSP reference notes (clean-room)

> **Clean-room intent.** These are algorithm descriptions in our own words plus our C plan,
> distilled from open sources for a from-scratch reimplementation. No third-party source is
> vendored into this repo. Reference headers studied (FluCoMa, **BSD-3-Clause**,
> `flucoma/flucoma-core`) are kept in the session scratchpad only. CataRT behavior is from
> Diemo Schwarz's published papers (behavioral study, no code). Cite sources in commit messages
> when implementing.
>
> Sources: FluCoMa core (`include/flucoma/algorithms/{public,util}/*.hpp`, BSD-3);
> Schwarz et al., *Real-Time Corpus-Based Concatenative Synthesis with CataRT*, DAFx 2006
> (dafx.de/paper-archive/2006/papers/p_279.pdf); Schwarz, *Corpus-Based Concatenative Synthesis*.

---

## FFT dependency
All spectral descriptors need an FFT. Options (GLIBC-2.29-safe, no libmvec):
- **kissfft** (BSD-3, ~1 file) — vendor for the analyst (offline 1024-pt) and the mosaic render-thread FFT. License-clean.
- Or an in-house iterative radix-2 (float) — trivial for power-of-two 1024, keeps zero deps.
**Locked: kissfft (BSD-3).** Not FFTW (GPL forces GPL → CC BY-NC-SA NC-incompatible); HISSTools
(`flucoma/HISSTools_Library`, BSD-3, SIMD/NEON) held in reserve only if profiling demands it.
Analyst: 1024-pt, Hann, hop 512.

## Per-slice summarisation (FluCoMa MultiStats) + normalisation
A slice is summarised, not reduced to one frame. Compute per-frame descriptors across the slice,
then **mean + std** per descriptor channel (FluCoMa `MultiStats` supports 7 stats + derivatives; we
take mean+std for v1 — std encodes morphology and materially improves match musicality). Result
≈ 36 dims: (MFCC1–13, centroid, flatness, ZCR) × {mean,std} + loudness(mean/peak) + pitch(median
voiced)+conf. Normalise per-corpus: **RobustScaling** (median/IQR, `RobustScaling.hpp`) for the
query distance (outlier-robust > z-score); min-max→0–1 for the nav axes. Store scaler params in the index.
**v2:** UMAP/PCA (`UMAP.hpp`/`PCA.hpp`) projects the full descriptor space → 2-D nav map, precomputed
at analysis; a nav-layer swap, not an engine change.

---

## 1. MFCC = MelBands → DCT-II
Standard pipeline. FluCoMa has no `MFCC.hpp`; it *is* `MelBands` (log) → `DCT`.

**Mel filterbank** (`MelBands.hpp`):
- `hz2mel(x) = 1127.01048 · ln(x/700 + 1)`; inverse `mel2hz(m) = 700·(exp(m/1127.01048) − 1)`.
- Place `nBands+2` points linearly in mel between `hz2mel(lo)`/`hz2mel(hi)`, convert back to Hz.
- Triangular overlapping filters (Slaney-style): filter `i` rises from centre `i` to `i+1`, falls
  to `i+2`; `row_i = min(lowerRamp, upperRamp).max(0)`, normalised by mel spacing `melD`.
- Frame options: `usePower` (square magnitudes), `logOutput` (`20·log10(max(x,eps))`).
- For MFCC: **power + log**. Default `lo≈20 Hz`, `hi≈sr/2`, `nBands≈40`.

**DCT-II orthonormal** (`DCT.hpp`):
- `table[i][n] = cos((π/N)·i·(n+0.5)) · scale`, `scale = (i==0) ? 1/√N : √(2/N)`, `N = nBands`.
- `out = table · in`, keep `outputSize = 13` coefficients. (Coeff 0 ≈ log-energy; keep it or drop
  it — decide during tuning. We'll keep MFCC1–13 and use loudness separately.)

C plan: precompute the `nBands×nBins` filter matrix and the `13×nBands` DCT matrix once at
init (they depend only on sr/fftSize/nBands); per-slice = two matrix·vector products. Cheap.

## 2. Spectral centroid + flatness (`SpectralShape.hpp`)
Band-limited between `minFreq`/`maxFreq`; `amp` = magnitude (optionally squared).
- **Centroid** = `Σ(amp·freq) / Σamp` (freq in Hz; FluCoMa can express in MIDI-cents when `logFreq`).
- **Spread** = `√( Σ amp·(freq−centroid)² / Σamp )` (available; optional descriptor).
- **Flatness** = `exp(mean(log amp)) / mean(amp)` (geometric/arithmetic mean), reported as
  `20·log10(max(flatness, eps))` dB. Low = tonal, high (→0 dB) = noisy. This is our "noisiness" axis.
- Also available: rolloff, crest, skew, kurtosis. v1 uses **centroid + flatness** (maybe rolloff).

## 3. YINFFT pitch + confidence (`YINFFT.hpp`)
FFT-accelerated YIN (de Cheveigné/Kawahara, spectral-domain variant):
1. `squareMag = |X|²` over `nBins`; `squareMagSum = 2·Σ squareMag`.
2. Build symmetric array `[sq0, sq[1..N-1], reverse(sq[1..N-2])]`, FFT it (→ autocorrelation via
   Wiener–Khinchin), `yin = squareMagSum − Re(FFT)` = the YIN difference function `d(τ)`.
3. `yin(0)=1`; **cumulative mean normalisation** (CMNDF): `tmpSum += yin(i); yin(i) *= i/tmpSum`.
4. Peak-pick the minimum of `yin` over lag bins `[minBin = round(sr/maxFreq), maxBin = round(sr/minFreq)]`
   with **parabolic interpolation**; `pitch = sr/(minBin + peakPos)`, `confidence = max(1 + peakVal, 0)`.

C plan: per-slice on the analyst thread only (not render). Reuse the slice's FFT magnitude. Store
`pitch` (Hz→MIDI) and `confidence`; low-confidence slices get a "no pitch" flag so Notes-mode
pitch-match and the pitch axis can skip them.

## 4. Spectral-flux onset (`OnsetDetectionFuncs.hpp` + `OnsetDetectionFunctions.hpp`)
- **ODF (spectral flux):** `flux = mean( max(|Xₜ| − |Xₜ₋₁|, 0) )` — half-wave-rectified magnitude
  difference, meaned. (FluCoMa also has HFC, complexDev, MKL, etc.; flux is our default.)
- **Post-processing:** subtract a running **median filter** of the ODF (adaptive threshold):
  `filtered = flux − median(history, filterSize)`; then peak-pick above a `threshold` with a
  **debounce** = `minSliceLength` frames between onsets.
- **Slicing:** onsets bound the slices; clamp to `min 60 ms / max 400 ms` (§design-spec), floor
  ~20 ms; chunk stable regions longer than max; silence-gate low-loudness slices.

C plan (analyst, offline): stream file → framed FFT (1024/hop 512) → flux → median-subtract →
peak-pick → slice boundaries. Fixed-grain fallback bypasses onset detection.

---

## 5. CataRT playback / scheduling behavior → our engine
From Schwarz DAFx 2006. Maps cleanly onto our params; one addition (repetition avoidance).

- **Selection** = **kNN in descriptor space** by *weighted Euclidean distance* to a target point;
  pick **randomly among a radius/K of candidates**, not always the single nearest → our
  `k_nearest` + `radius`. In Map mode the target is `(x_pos, y_pos)` in the two nav dims; in
  Mosaic mode the target is the live input's full descriptor vector.
- **Descriptor weighting**: per-dimension weights on the distance. We already do this via
  per-corpus z-score normalisation; MFCCs carry the timbre-match weight in Mosaic mode.
- **Trigger / interaction modes** (their bow / beat / fence / chain) → our scheduling:
  - *bow* (continuous stream as the target moves) → **Map mode** grain cloud driven by `density`.
  - *beat* (tempo-synced) → **`follow = Sequence`** (MIDI-clock 24 PPQN) / tempo-synced `mosaic_rate`.
  - *fence* (fire on crossing a boundary) → **Mosaic mode** onset-triggered re-query.
  - *chain* (sequential walk) → future "corpus walk" follow mode.
  - *discrete* → **Notes mode** (note-on triggers one pitch-matched grain).
- **Repetition avoidance** (NEW — add to v1): CataRT forbids re-selecting the most recently played
  unit (optionally a *set* of recent units), or enforces a **minimum descriptor distance** from the
  last unit, to avoid the "stuck grain" artifact. **Plan:** keep a small ring buffer of the last
  `repeat_avoid` played slice indices; exclude them from the candidate set (fall back to next-nearest
  if all K are forbidden). New param `repeat_avoid` (int 0–16, default 4). Cheap, big musical payoff.
- **Concatenation cost** (discontinuity penalty between consecutive units) — CataRT sums it with the
  target-distance cost for smooth resynthesis. **v2** for Concat (matters most for clean audio
  mosaicking; v1 relies on grain-envelope crossfades to hide seams).
- **Grain playback**: 50–200 ms grains, **Hann/triangular** envelope, overlap for smooth texture →
  our Grain page (`grain_size` 10–500 ms def 120, `grain_env`, `density`, `pan_spread`). Grain-lock
  window-phase params at grain start (Schwung LESSON: avoid mid-grain clicks).

---

## Net new items for the spec
1. **`repeat_avoid`** param (int 0–16, def 4) — recently-played-slice ring buffer, exclude from kNN.
2. **FFT dependency** — kissfft (BSD-3) vs in-house radix-2; decide at scaffold.
3. Optional v2: concatenation-cost term; `chain`/corpus-walk follow mode; extra descriptors
   (rolloff, spread, crest) already free from `SpectralShape`.
