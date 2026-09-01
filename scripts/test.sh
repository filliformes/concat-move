#!/bin/bash
# Cross-compile the DSP unit tests for aarch64 and run them ON the Move
# (the only aarch64 runtime we have). Tests pure kernels — no Schwung host needed.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MOVE_HOST="${MOVE_HOST:-move.local}"
REMOTE_DIR="/data/UserData/concat_test"

docker build -t schwung-concat-builder "$SCRIPT_DIR" >/dev/null
mkdir -p "$ROOT/dist"

echo "Cross-compiling tests (aarch64)..."
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/build" schwung-concat-builder \
  aarch64-linux-gnu-gcc -O2 -Wall -Wextra -I/build/src/dsp \
    -o /build/dist/test_analysis \
    /build/tests/test_analysis.c /build/src/dsp/analysis.c -lm

echo "Running on $MOVE_HOST..."
ssh "ableton@$MOVE_HOST" "mkdir -p $REMOTE_DIR"
scp -q "$ROOT/dist/test_analysis" "ableton@$MOVE_HOST:$REMOTE_DIR/"
ssh "ableton@$MOVE_HOST" "chmod +x $REMOTE_DIR/test_analysis; $REMOTE_DIR/test_analysis; rc=\$?; rm -rf $REMOTE_DIR; exit \$rc"
