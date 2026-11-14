#!/bin/sh
# Started by /mnt/data/auto.sh (S99user). All resources are local UBIFS.
set -eu
base=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
# S99user sources auto.sh in the background. Wait for rcS to finish so the
# older flashed S99z_ky50_lcd cannot start a second VO owner after us.
attempt=0
while ps | grep -q '[r]cS'; do
    attempt=$((attempt+1))
    [ "$attempt" -le 60 ] || { echo "rcS still active; refusing a media-owner race"; exit 1; }
    sleep 1
done
if [ -x /etc/init.d/S99z_ky50_lcd ]; then /etc/init.d/S99z_ky50_lcd stop; fi
exec "$base/run.sh"
