# Corpus files and the Corpus Builder (design draft, 2026-09-28)

> **Status (2026-10-20):** built and released in v0.2.0. The user-facing description is in [MANUAL.md](MANUAL.md#7-the-corpus-builder); this file keeps the design decisions and the verified schwung-manager facts. Two changes from the draft: the page is `builder/app.html` (the manager redirects `index.html` to a folder URL that returns 404), and the browser decodes audio with Concat's own dr_libs code compiled to WebAssembly instead of `decodeAudioData`, which gives the same samples as the Move and reads AIFF in every browser.

Decisions (from Vincent): JSON `.concat` files, stored in `UserLibrary/Corpora/`,
per-file descriptor cache with audio re-read on load, and the web tool first.

## 1. The `.concat` file

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
  "slicing": { "mode": "onset", "min_ms": 40, "max_ms": 600 }
}
```

- Paths are relative to `/data/UserData/UserLibrary` (absolute allowed).
- A file entry may carry `start`/`end` in seconds: only that range enters the corpus.
- A folder entry may carry include/exclude globs and `recurse`.
- Missing entries are skipped and counted in the status line, never fatal.
- `descriptors.use` selects which descriptor groups the query and map use.

## 2. Engine (Concat DSP)

1. `corpus_resolve()` replaces `ingest_list_audio(folder)` inside `corpus_index_build`:
   folder, single file, or `.concat` (parsed on the analysis thread).
2. Source picker filter gains `.concat`.
3. Per-file descriptor cache keyed by path + mtime + size + range + slicing params.
   Audio is re-read from the original file on load. Only new or changed files are
   analysed; robust fit, k-means and PCA are recomputed over the whole set.
4. Hot reload: the analysis thread stats the `.concat` file and referenced folders
   every ~2 s; changes rebuild in the background with the old corpus playing.
5. Per-slice source/file index, stored in the cache (bundle with cache version 5).

Prerequisite: Phase 0 (cancellable build, bounded teardown, status line). Hot reload
without it can freeze the Move exactly like the module-switch bug.

**Status (2026-10-19): items 1-4 implemented** (`json.c`, `corpus_file.c`, per-file cache
in `corpus_engine.c`, watch loop in `concat.c`). Item 5 (per-slice source index) waits for
the builder, which computes its own. Notes from implementation and device tests:

- `descriptors.use` zeroes the unused columns after RobustScaling, so search, k-means and
  PCA ignore them with no other change. A raw X/Y axis set to an unused descriptor goes flat.
- Per-file cache: `<cache_dir>/files/<hash>.cfd`, keyed by path + range + slicing params,
  validated by mtime + size. It records slice boundaries per 60 s window and descriptors as
  they are computed, so a memory-capped or cancelled build still saves its progress.
- Hot reload polls every 2 s and rebuilds once a change holds still for one poll. The
  baseline stamp is taken when the sources are read (ns mtime), so a save during a build
  is not missed. Device: editing a 9170-slice `.concat` goes live in 8.6 s with the
  per-file cache (75.8 s cold); a broken save keeps the old corpus with "bad JSON: ...,
  kept old" on the status line.
- No cache eviction yet: on the test Move the cache dir is 352 MB.

## 3. Corpus Builder web tool

### Transport: no new server needed

schwung-manager (port 7700) already exposes what the tool needs:

| Need | Existing route |
|---|---|
| Browse UserLibrary | `GET /files` |
| Stream a sample for audition and waveform | `GET /files/download` |
| Save or update a `.concat` file | `POST /files/upload`, `/files/rename`, `/files/delete` |
| Serve the tool's own page and scripts | `GET /api/remote-ui/module-assets/{id}/{filepath...}` |

### What the `/files` routes actually do (schwung main, `schwung-manager/main.go:2507-2685`)

Checked in source on 2026-09-28. Not yet probed live: the Move was offline.

| Route | Parameters | Returns | Notes for the builder |
|---|---|---|---|
| `GET /files` | `path` (absolute dir) | **HTML page only** | No JSON mode. Rows carry `data-path`, `data-name`, `data-isdir`; parse with `DOMParser`. No sizes or dates as attributes. |
| `GET /files/download` | `path` (absolute file) | `http.ServeFile` | **Range requests work** (206, `Accept-Ranges`), plus `Last-Modified`. `Content-Disposition: attachment` does not affect `fetch`. |
| `POST /files/upload` | multipart: `path` (dest dir), `file` (name taken from the part's filename) | 200 with `X-CSRF-Token` header, else 303 | **Overwrites in place, not atomically** (`os.Create` then copy). |
| `POST /files/rename` | `path` (old), `name` (new name, same dir) | always 303 to the HTML page | `os.Rename`: atomic, replaces an existing target. |
| `POST /files/mkdir` | `path` (parent), `name` | 200 with header | `MkdirAll`. |
| `POST /files/delete` | `path` | 200 with header | **`os.RemoveAll`: recursive.** |

**Roots.** `-roots` defaults to `/data/UserData/`, so the whole UserData tree is
reachable, including `schwung/` and installed modules. Paths must be absolute;
`..` is rejected (`middleware/security.go:ValidatePath`). Symlinks are not resolved,
so a link inside UserData pointing elsewhere is followed.

**Authentication.** None. The LAN is the trust boundary. State-changing requests
use a double-submit CSRF check: cookie `csrf_token` (`SameSite=Strict`, readable by
JS) must equal the `X-CSRF-Token` header. There are no CORS headers.

**Timeouts.** `ReadTimeout` 30 s, `WriteTimeout` 60 s. A big WAV over WiFi can take
longer than 60 s to download, and an upload longer than 30 s fails.

**Module pages.** `GET /api/remote-ui/module-assets/{id}/{filepath...}` serves any
file under the module's folder, with `X-Frame-Options: SAMEORIGIN`,
`Cache-Control: no-store` and **no CSP**, so WASM and inline scripts run.

### Design consequences

1. **Serve the builder from the manager, same origin.** Ship it in the module as
   `builder/index.html` (+ JS, WASM, three.js) and open
   `http://move.local:7700/api/remote-ui/module-assets/concat/builder/index.html`.
   A page on any other origin cannot send the CSRF cookie or read downloads. Bundle
   every library locally: the Move's browser user may be offline.
2. **List by parsing HTML.** Folder drag-and-drop recurses with one `/files` fetch
   per directory. Propose an upstream `?format=json` (with size and mtime) to Schwung
   later; keep the HTML parser as fallback.
3. **Stay inside `/data/UserData/UserLibrary`.** The builder enforces this itself,
   since the server allows all of UserData.
4. **Download audio in Range chunks** (for example 4 MB) and reassemble, so no single
   response hits the 60 s write timeout; decode after. Chunks also give a progress bar.
5. **Save atomically:** upload `Name.concat.tmp`, then rename it to `Name.concat`.
   Concat's hot reload must ignore `*.tmp` and debounce, or it could read a
   half-written file.
6. **Never call `/files/delete` on library content.** Removing a sample from a corpus
   is only a JSON edit. The builder only deletes its own `.tmp` files and whole
   `.concat` files, after an in-page confirmation.
7. **Uploaded descriptor caches must upload in under 30 s**, so write them per file,
   not as one big blob.

### Verified live on the Move (2026-09-28, Schwung 1.5.0, over USB)

| Check | Result |
|---|---|
| Process | `schwung-manager -port 7700 -roots /data/UserData/`, uid 1000 (`ableton`) |
| `GET /files` | HTML only, `Accept: application/json` ignored. Rows parse cleanly. MELLOTRON (387 files) lists in 80 ms, 352 KB. |
| Range | `206`, `Accept-Ranges: bytes`, correct `Content-Range`, `Last-Modified` |
| Content types | `.wav` → `audio/wav`, `.aif` → `audio/x-aiff` |
| Throughput (USB) | 26.6 MB/s full download; 4 MB range chunk in 0.16 s. WiFi not measured. |
| Outside root / relative path | `403` |
| POST without token | `403 Forbidden: invalid CSRF token` |
| Upload, overwrite, mkdir with token | `200` |
| Rename | works, always `303`. **A `/` or `../` in the new name moves the file anywhere under UserData.** |
| Delete of a missing path | `200` (no error) |
| Writes into a root-owned folder | `500 permission denied`: the manager can only write where `ableton` can |

Consequences added to the design:

- **The builder must validate the rename `name` itself** (plain file name, no `/`), because the server accepts moves across folders.
- **Everything Concat and the builder share must be `ableton:users`.** `MoveOriginal` also runs as uid 1000.
- **AIFF.** The library holds `.aif` samples that Concat ignores: ingest and the Source filter only take WAV, FLAC and MP3. dr_libs has no AIFF decoder, so this needs a small AIFF reader (PCM, big-endian). The browser decodes AIFF natively.

### Side finding: the analysis cache was not writable

`/data/UserData/schwung/concat-cache` was owned by `root` (mode 755) while Concat runs as
`ableton`, so Concat could not write new cache entries and re-analysed on every load.
Ownership was reset to `ableton:users` on 2026-09-28. Root-run test tools (`test.sh`
analysis tests, the session simulator run as root) must not create files there again:
run them as `ableton` or chown afterwards.

### Analysis runs in the browser, with Concat's own C code

Compile `analysis.c`, `corpus.c` and the descriptor code to WebAssembly (emscripten).
The page downloads each sample, decodes it with the browser's `decodeAudioData`,
resamples to 44.1 kHz mono and runs the identical slicing and descriptor code.
Result: the statistics and the 3D map show exactly what Concat will compute, and the
CM4 does no work while you curate. Optionally the page uploads the per-file
descriptor cache so the Move skips analysis entirely.

### Features

- Library tree on the left; drag files or folders onto the corpus list.
- Per item: play, waveform with draggable start and end markers, remove.
- Descriptor panel: every available descriptor with per-corpus statistics
  (min, median, IQR, histogram, variance share); toggle which ones the corpus uses.
- 3D map: PCA (later UMAP) of the chosen descriptors, colour by source, click a
  point to audition that slice. three.js from cdnjs.
- Save writes the `.concat` file; Concat hot-reloads it if it is loaded.

## 4. Order

1. Phase 0 of the audit roadmap.
2. `.concat` + resolver + hot reload in the DSP.
3. Verify the manager `/files` API; build the WASM analysis module and check it
   matches the device descriptors bit for bit on a few files.
4. Builder page: library, corpus list, audition and trim, save.
5. Descriptor statistics and selection, then the 3D map.
