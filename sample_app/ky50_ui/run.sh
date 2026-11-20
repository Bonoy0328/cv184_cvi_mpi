#!/bin/sh
# Stage all runtime files locally: pulling Ethernet must not stall an NFS ELF.
set -eu
# Dropbear non-interactive commands may omit /sbin from PATH.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
src=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
dst=/tmp/ky50-ui
if [ -r /run/ky50-ui.pid ]; then
    pid=$(cat /run/ky50-ui.pid)
    case "$pid" in ''|*[!0-9]*) pid=0;; esac
    if [ "$(readlink "/proc/$pid/exe" 2>/dev/null || true)" = "$dst/ky50_ui" ]; then
        echo "ky50_ui already running: $pid" >&2; exit 1
    fi
fi
for proc in /proc/[0-9]*; do
    exe=$(readlink "$proc/exe" 2>/dev/null || true)
    case "$exe" in */ky50_sensor_capture|*/sample_ky50_lcd|*/ky50_ui)
        echo "Stop the current media application first: ${proc##*/} $exe" >&2;exit 1;;
    esac
done
mkdir -p "$dst"
cp "$src/stop.sh" "$dst/stop.sh"
cp "$src/ky50_ui" "$dst/ky50_ui.new"
chmod 755 "$dst/ky50_ui.new"
mv "$dst/ky50_ui.new" "$dst/ky50_ui"
# The package contains exactly the models selected at build time. Use a fresh
# directory so switching to a smaller release cannot reuse an old model.
rm -rf "$dst/pva"
if [ -d "$src/pva" ]; then
    cp -R "$src/pva" "$dst/pva"
fi
# BMRuntime stays on local /system; neither executable nor model depends on NFS.
export LD_LIBRARY_PATH=/system/lib:/usr/lib:/lib
mkdir -p "$dst/assets"
cp "$src/assets/recognition_success.s16le" "$dst/assets/recognition_success.s16le"
mkdir -p "$dst/assets/ime"
cp "$src/assets/ime/pinyin.tsv" "$dst/assets/ime/pinyin.tsv"
# A factory-validated profile ships with the application and is reused by the
# same camera assembly. A deliberate local recalibration overrides this file.
if [ -r "$src/assets/face_ir_calibration.conf" ]; then
    cp "$src/assets/face_ir_calibration.conf" "$dst/assets/face_ir_calibration.conf.new"
    mv "$dst/assets/face_ir_calibration.conf.new" "$dst/assets/face_ir_calibration.conf"
else
    rm -f "$dst/assets/face_ir_calibration.conf"
fi
# /mnt/cfg and /mnt/data are each about 1 MiB on this board. Business data
# belongs on the larger persistent system UBIFS, outside the installed app.
umask 077
mkdir -p /mnt/system/ky50-data/avatars
if [ -d "$dst/pva" ]; then mkdir -p /mnt/system/ky50-data/pva; fi
# Do not import legacy template files. On a v1/v2 users database the
# application clears old profiles and both SDK stores once, then uses schema 4. Existing v3 users migrate without deletion.
for name in ky50_i2c1 ky50_touch ky50_relay ky50_lights ky50_amp; do
    cp "$src/$name.ko" "$dst/$name.ko"
    # Upgrade only the relay protocol after the old app has closed its fd.
    # Never unload media/VO here. An in-use relay fails safely via rmmod.
    if [ "$name" = ky50_relay ] && [ -d /sys/module/ky50_relay ] &&
       [ "$(cat /sys/module/ky50_relay/version 2>/dev/null || true)" != "2.0" ]; then
        rmmod ky50_relay
    fi
    if [ ! -d "/sys/module/$name" ]; then
        # Touch must probe after the application's LCD/TP reset sequence.
        if [ "$name" = ky50_i2c1 ]; then insmod "$dst/$name.ko"; fi
        if [ "$name" = ky50_relay ]; then
            insmod "$dst/$name.ko" || echo "Relay unavailable; other tests remain usable" >&2
        fi
        if [ "$name" = ky50_lights ]; then
            insmod "$dst/$name.ko" || echo "Factory lights unavailable; other tests remain usable" >&2
        fi
        if [ "$name" = ky50_amp ]; then
            insmod "$dst/$name.ko" || echo "Speaker amplifier unavailable" >&2
        fi
    fi
done
printf '%s\n' "$$" > /run/ky50-ui.pid
cd "$dst"
# Set only AFTER the two clock nets have physically been exchanged. The
# shipped/schematic wiring must never enable CAM_MCLK0 on a sensor output.
if [ -r /mnt/cfg/ky50-ui/dvp-clock-wiring ]; then
    wiring=$(cat /mnt/cfg/ky50-ui/dvp-clock-wiring)
    case "$wiring" in
        schematic|swapped) export KY50_DVP_CLOCK_WIRING="$wiring";;
        *) echo "Invalid DVP wiring setting; refusing to drive an unverified clock pin" >&2;exit 1;;
    esac
fi
exec ./ky50_ui "$@"
