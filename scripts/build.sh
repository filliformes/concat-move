#!/bin/bash
# Concat — Docker ARM64 cross-compile (canonical build; never build ad-hoc).
set -e
MODULE_ID="concat"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

docker build -t schwung-concat-builder "$SCRIPT_DIR"
mkdir -p "$ROOT/dist/$MODULE_ID"

# Compile every .c under src/dsp/ (concat.c + future analysis.c/corpus.c/kissfft.c/dr_libs).
# Flags per /move-synth: -ffast-math (no FTZ on ARM), NO -lrt (shm_open@2.34), NO -Ofast (libmvec).
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/build" schwung-concat-builder \
  bash -c 'aarch64-linux-gnu-gcc -O2 -shared -fPIC -ffast-math -Wall -Wextra \
    -I/build/src/dsp \
    -o /build/dist/concat/dsp.so \
    /build/src/dsp/*.c \
    -lm -lpthread'

cp "$ROOT/src/module.json" "$ROOT/dist/$MODULE_ID/"
[ -f "$ROOT/src/help.json" ] && cp "$ROOT/src/help.json" "$ROOT/dist/$MODULE_ID/"
tar -czf "$ROOT/dist/$MODULE_ID-module.tar.gz" -C "$ROOT/dist" "$MODULE_ID"
echo "Built: dist/$MODULE_ID-module.tar.gz"

# GLIBC sanity: max symbol must be <= 2.29 (Move ships 2.29).
echo "GLIBC symbols used:"
docker run --rm -v "$ROOT:/build" schwung-concat-builder \
  bash -c 'aarch64-linux-gnu-strings /build/dist/concat/dsp.so | grep GLIBC_ | sort -uV || true'
