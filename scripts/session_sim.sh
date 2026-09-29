#!/bin/bash
# Run the Concat session simulator ON the Move, in its own process.
# It loads a COPY of the installed dsp.so (the live module is never touched) and drives
# a full user session: first launch + auto-pick, knob sweeps on every param, pads +
# pressure, recipes, stress, Mosaic, state round-trip, and module-swap teardown timing.
#
#   scripts/session_sim.sh                  # full session (teardown test uses POSSIBLES)
#   scripts/session_sim.sh perf  <folder>   # big-corpus render cost (CPU time per block)
#   scripts/session_sim.sh restore <folder> # host-order state restore race test
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MOVE_HOST="${MOVE_HOST:-move.local}"
REMOTE=/data/UserData/concat_sim
MODE="${1:-session}"
FOLDER="${2:-/data/UserData/UserLibrary/Samples/POSSIBLES}"

docker build -t schwung-concat-builder "$SCRIPT_DIR" >/dev/null
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/build" schwung-concat-builder \
  aarch64-linux-gnu-gcc -O2 -o /build/dist/session_sim /build/tests/session_sim.c -ldl -lm -lpthread
ssh "root@$MOVE_HOST" "mkdir -p $REMOTE && cp /data/UserData/schwung/modules/sound_generators/concat/dsp.so $REMOTE/dsp.so"
scp -q "$ROOT/dist/session_sim" "root@$MOVE_HOST:$REMOTE/"
ARG4=""; [ "$MODE" != "session" ] && ARG4="$MODE"
ssh "root@$MOVE_HOST" "cd $REMOTE && chmod +x session_sim && ./session_sim $REMOTE/dsp.so $REMOTE '$FOLDER' $ARG4"
