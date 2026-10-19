#!/bin/bash
# Build the Corpus Builder's analysis core (src/wasm/cb_wasm.c + Concat's decode,
# slicing and descriptor code) to WebAssembly, as one self-contained JS file
# (wasm embedded, so the manager's content types do not matter).
# Output: src/builder/cb.js (committed; rebuild when analysis/corpus/ingest change).
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
mkdir -p "$ROOT/src/builder"
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -w /src emscripten/emsdk:3.1.74 \
  emcc -O2 -ffast-math -I src/dsp \
    src/wasm/cb_wasm.c src/dsp/analysis.c src/dsp/corpus.c src/dsp/ingest.c \
    -s MODULARIZE=1 -s EXPORT_NAME=ConcatCB -s SINGLE_FILE=1 -s ALLOW_MEMORY_GROWTH=1 \
    -s MAXIMUM_MEMORY=2147483648 -s ENVIRONMENT=web,worker,node \
    -s EXPORTED_FUNCTIONS=_cb_alloc,_cb_free,_cb_ndesc,_cb_win_seconds,_cb_load,_cb_unload,_cb_decode,_cb_audio,_cb_analyse,_cb_slices,_cb_desc \
    -s EXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32,HEAPF32,stringToNewUTF8 \
    -o src/builder/cb.js
echo "Built: src/builder/cb.js ($(wc -c < "$ROOT/src/builder/cb.js") bytes)"
