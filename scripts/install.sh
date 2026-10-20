#!/bin/bash
# Deploy Concat to a Move over SSH. Requires a prior ./scripts/build.sh.
#
# Files are uploaded to a staging folder, then each one is moved into place with
# `mv -f` (an atomic rename on the same filesystem). The running MoveOriginal keeps
# its mapping of the OLD dsp.so inode, so a deploy can no longer corrupt the code a
# live instance is executing (copying over the mapped file in place crashed the
# Move: SIGSEGV pc=0x0 on 2026-09-20). The new code loads on the next restart.
set -e
MODULE_ID="concat"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/sound_generators"
STAGE="$DEST/.$MODULE_ID.staging"

echo "Installing $MODULE_ID to $MOVE_HOST..."
ssh "root@$MOVE_HOST" "rm -rf '$STAGE' && mkdir -p '$STAGE' '$DEST/$MODULE_ID'"
scp -q -r "dist/$MODULE_ID/." "root@$MOVE_HOST:$STAGE/"
# chmod +x is required or dlopen() fails silently; files must be owned by ableton:users.
ssh "root@$MOVE_HOST" "set -e; cd '$STAGE'; chmod +x dsp.so; chown -R ableton:users .;
  for f in *; do
    if [ -d \"\$f\" ]; then rm -rf '$DEST/$MODULE_ID/'\"\$f\"; fi;
    mv -f \"\$f\" '$DEST/$MODULE_ID/';
  done;
  cd /; rmdir '$STAGE'; chown -R ableton:users '$DEST/$MODULE_ID'"
echo "Done. Verify: ssh root@$MOVE_HOST 'ls -la $DEST/$MODULE_ID/'"
echo "Restart Move to load the new dsp.so; power-cycle if module.json changed (manifest is cached at startup)."
