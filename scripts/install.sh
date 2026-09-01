#!/bin/bash
# Deploy Concat to a Move over SSH. Requires a prior ./scripts/build.sh.
set -e
MODULE_ID="concat"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/sound_generators"

echo "Installing $MODULE_ID to $MOVE_HOST..."
scp -r "dist/$MODULE_ID" "root@$MOVE_HOST:$DEST/"
# chmod +x is required or dlopen() fails silently; files must be owned by ableton:users.
ssh "root@$MOVE_HOST" "chmod +x $DEST/$MODULE_ID/dsp.so; chown -R ableton:users $DEST/$MODULE_ID"
echo "Done. Verify: ssh root@$MOVE_HOST 'ls -la $DEST/$MODULE_ID/'"
echo "Power-cycle the Move if module.json changed (manifest is cached at startup)."
