#!/usr/bin/env bash
# Offline tests in the gcc:12 Docker image (needs network: they read the live catalog and download one zip).
#   tests/run.sh            the wrapper host test, each fake device in tests/devices/, then the addin flow against
#                           an offline fixture (tests/fixture.py: three addin packages and a file:// catalog)
# A fake device is a shell script that builds the device's files inside the container: MPC.settings (the real format,
# trimmed), plugin folders, a stub systemctl. FS_ANY=1 lets the container's overlay filesystem count as internal storage.
# Add one for your model (copy its MPC.settings layout from the device.txt the plugin writes) and send it with your PR.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
MV="${MPC_VST:-$here/../mpc-vst-plugins}"
python3 "$MV/tools/gen_vst.py" "$here/vst/vst.json" --params-h >/dev/null
MPC_VST="$MV" python3 "$here/tests/fixture.py"
docker run --rm -v "$here":/p -v "$MV":/mv:ro -w /p gcc:12 bash -c '
set -e
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
gcc $SAN -std=gnu11 -w -Ivst/build src/manager.c /mv/tools/host_test.c /mv/wrapper/vst2_wrap.c -lpthread -ldl -lm -o /tmp/host_test
(cd /tmp && ./host_test | tail -n 1)
gcc $SAN -DFS_ANY=1 -std=gnu11 -w src/manager.c tests/drive.c -lpthread -ldl -o /tmp/drive
for dev in tests/devices/*.sh; do
  echo "== $dev"
  # each device starts from a clean filesystem
  rm -rf /media /sdcard /tmp/pluginmgr; mkdir -p /media
  sh "$dev" && /tmp/drive && grep -E "location|target|problem" /tmp/pluginmgr/device.txt
done
echo "== addins (offline fixture)"
gcc $SAN -DFS_ANY=1 -DCATALOG_URL=\"file:///p/vst/build/tests/catalog.json\" -std=gnu11 -w src/manager.c tests/drive.c -lpthread -ldl -o /tmp/drive_addins
rm -rf /media /sdcard /data /tmp/pluginmgr; mkdir -p /media
sh tests/devices/mpc-one.sh && sh tests/addins.sh && /tmp/drive_addins addins
grep -E "^addin" /tmp/pluginmgr/device.txt
echo "-- apply.sh:"; grep -E "^(sh|echo)" /tmp/pluginmgr/apply.sh
grep -q "/install.sh'"'"' -y -n -t '"'"'/data/mpc-addins/beta-addin'"'"'" /tmp/pluginmgr/apply.sh
grep -q "/data/mpc-addins/alpha-addin/uninstall.sh'"'"' -y -n -t '"'"'/data/mpc-addins/alpha-addin'"'"'" /tmp/pluginmgr/apply.sh
! grep -q gamma /tmp/pluginmgr/apply.sh
echo "addins OK"'
