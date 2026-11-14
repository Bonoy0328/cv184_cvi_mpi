#!/bin/sh
# Install only on KY50. Does not flash kernel/rootfs or modify network settings.
set -eu
src=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
dst=/mnt/system/ky50-ui
[ -d /mnt/system ] && [ -d /mnt/cfg ] && [ -d /mnt/data ]
grep -q ' /mnt/system ubifs rw' /proc/mounts
grep -q ' /mnt/data ubifs rw' /proc/mounts
(cd "$src" && sha256sum -c SHA256SUMS)
mkdir -p /mnt/system/ky50-ui-backups /mnt/cfg/ky50-ui
stage=$(mktemp -d /mnt/system/ky50-ui.stage.XXXXXX)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
cp -a "$src/." "$stage/"
if [ -d "$dst" ]; then
    mv "$dst" "/mnt/system/ky50-ui-backups/app-$(date +%s)"
fi
mv "$stage" "$dst"
auto=/mnt/data/auto.sh
if [ -e "$auto" ]; then
    cp -p "$auto" "/mnt/system/ky50-ui-backups/auto-$(date +%s).sh"
    sed '/^# BEGIN KY50_UI_BOOT$/,/^# END KY50_UI_BOOT$/d' "$auto" > "$auto.ky50-new"
else
    printf '#!/bin/sh\n' > "$auto.ky50-new"
fi
cat >> "$auto.ky50-new" <<'BOOT'

# BEGIN KY50_UI_BOOT
/mnt/system/ky50-ui/boot.sh </dev/null >/tmp/ky50-ui-boot.log 2>&1 &
# END KY50_UI_BOOT
BOOT
chmod 755 "$auto.ky50-new"
mv "$auto.ky50-new" "$auto"
sync
echo "Installed $dst; saved display source is in /mnt/cfg/ky50-ui/display-source."
echo "Auto-start enabled. Existing app was not interrupted; restart using stop.sh then boot.sh."
