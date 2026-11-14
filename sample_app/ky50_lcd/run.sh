#!/bin/sh
# KY50 / M050-0287-1B0: supplier ST7102 table and timing, NFS debug only.
# Optional duration in seconds; 0 keeps the image until Ctrl-C/SIGTERM.
set -eu
cd "$(dirname "$0")"
# Older flashed images may still contain the former firmware service.
# Stop it for this boot before claiming the same GPIO/VO/MIPI resources.
if [ -x /etc/init.d/S99z_ky50_lcd ]; then
    /etc/init.d/S99z_ky50_lcd stop
fi
# The flashed VO driver still includes the header in long-packet CRCs.
# Use the NFS userspace workaround without unloading that kernel module.
export KY50_DIAG_PAYLOAD_CRC=1
exec ./sample_ky50_lcd --vendor-test 2,40,40,2,9,193,60 "${1:-0}"
