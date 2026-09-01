# Concat

Corpus concatenative synthesis for [Ableton Move](https://www.ableton.com/move/),
built on the [Schwung](https://github.com/charlesvestal/schwung) framework.

Concat analyses a **folder** of audio — not a single sample — slices every file into
short units, describes each by timbre (MFCC, brightness, noisiness, pitch, loudness),
and plays back the units nearest a moving point in that descriptor space. It's granular
synthesis navigated by *timbre* across a whole library, plus a **mosaic** mode that
re-assembles the corpus in real time to match live line/mic input.

> **Status:** scaffold / work in progress. The parameter surface, menus, and plumbing
> are in place; the analysis threads, corpus pool, and grain engine are being built.
> See `design-spec.md` and `references/dsp-notes.md`.

## What makes it different

The catalog's granular modules all read **one** buffer. Concat navigates a descriptor
space across a **sample folder** — CataRT/FluCoMa-style corpus synthesis on the Move.

## Play modes

- **Map** — drive an X/Y point through the descriptor map with the encoders; a radius/K
  sprays selection; pads jump to regions of the map.
- **Notes** — incoming MIDI notes pitch-match to the nearest slice.
- **Mosaic** — the target follows the live line/mic input; sing/beatbox/feed a source and
  the corpus is re-assembled to match it.

## Controls

| Control | Function |
|---------|----------|
| Jog wheel | Navigate pages: Corpus / Map / Grain / Play |
| Knobs 1–8 | Adjust the current page's parameters |

**Corpus:** pool cap, rescan, slice mode, min/max slice, analyze progress, silence gate.
**Map:** X/Y descriptor, X/Y position, radius, K nearest, follow, repeat-avoid.
**Grain:** density, grain size, envelope, pitch, pitch jitter, pan spread, reverse.
**Play / Out:** play mode, input source, mosaic rate, note match, dry mix, volume, width, saturation.

## Building

```
./scripts/build.sh
```

Requires Docker (cross-compiles for ARM64 aarch64). Produces `dist/concat-module.tar.gz`.

## Installation

```
./scripts/install.sh
```

Or install via the Schwung Manager (`http://move.local:7700`).

## Corpus material

Point Concat at any folder of audio on the device (`/data/UserData/...`) — your own
recordings, sample libraries, or the factory pack. You supply the audio; Concat does not
ship sample content. Supported: WAV / FLAC / MP3.

## Credits & acknowledgements

Concat is an independent clean-room implementation. Its algorithm choices are informed by:

- **FluCoMa** (Fluid Corpus Manipulation, University of Huddersfield, BSD-3-Clause) — the
  reference for the descriptor pipeline (MFCC, spectral shape, YINFFT, onset, stats,
  normalisation). Studied, not copied.
- **CataRT** (Diemo Schwarz, IRCAM) — the corpus-based concatenative synthesis model
  (kNN selection, trigger modes, repetition avoidance).
- Decoding via **dr_libs** (public domain); FFT via **kissfft** (BSD-3-Clause).

## License

MIT — see [LICENSE](LICENSE). (Framework: Schwung, CC BY-NC-SA 4.0, separate.)
