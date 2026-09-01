#!/bin/bash
# Cross-compile the DSP unit tests for aarch64 and run them ON the Move
# (the only aarch64 runtime we have). Tests pure kernels — no Schwung host needed.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MOVE_HOST="${MOVE_HOST:-move.local}"
REMOTE_DIR="/data/UserData/concat_test"

# Kernel sources every test links against (exclude concat.c — it's the plugin entry).
SRCS="/build/src/dsp/analysis.c /build/src/dsp/corpus.c /build/src/dsp/query.c /build/src/dsp/ingest.c"

docker build -t schwung-concat-builder "$SCRIPT_DIR" >/dev/null
mkdir -p "$ROOT/dist"
ssh "ableton@$MOVE_HOST" "mkdir -p $REMOTE_DIR"

FAIL=0
for t in "$ROOT"/tests/test_*.c; do
  name=$(basename "$t" .c)
  echo "=== $name ==="
  MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/build" schwung-concat-builder \
    aarch64-linux-gnu-gcc -O2 -Wall -Wextra -I/build/src/dsp \
      -o "/build/dist/$name" "/build/tests/$name.c" $SRCS -lm
  scp -q "$ROOT/dist/$name" "ableton@$MOVE_HOST:$REMOTE_DIR/"
  ssh "ableton@$MOVE_HOST" "chmod +x $REMOTE_DIR/$name; $REMOTE_DIR/$name" || FAIL=1
done

ssh "ableton@$MOVE_HOST" "rm -rf $REMOTE_DIR"
exit $FAIL
