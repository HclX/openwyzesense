#!/usr/bin/env bash
# Re-creates third_party/ti_coresdk/: the subset of TI's SimpleLink core SDK the firmware builds from, taken from
# the Contiki-NG mirror of it (BSD-3-Clause, TI), pinned by commit and archive SHA-256. Run it only to change the
# pinned version or the file list; the result is committed.
set -euo pipefail

REPO=contiki-ng/coresdk_cc13xx_cc26xx
COMMIT=60e7325e122e95660fa9a4019e7868aedb4c317d # 2023-09-08, "Merge pull request #6"
SHA256=37148952eab63ee2791da879f9a87fd2791b3c1fb9c1ed315b9243ff99f78968

FILES=(
    LICENSE
    source/ti/devices/DeviceFamily.h
    'source/ti/devices/cc13x0/driverlib/*.[ch]'
    'source/ti/devices/cc13x0/inc/*.h'
    'source/ti/devices/cc13x0/rf_patches/rf_patch_*_sl_longrange.h'
    source/ti/devices/cc13x0/startup_files/ccfg.c
    source/ti/drivers/Power.h
    source/ti/drivers/PIN.h
    'source/ti/drivers/dpl/*.h'
    source/ti/drivers/rf/RF.h
    source/ti/drivers/rf/RFCC26XX_singleMode.c
    source/ti/drivers/power/PowerCC26XX.c
    source/ti/drivers/power/PowerCC26XX.h
    source/ti/drivers/power/PowerCC26XX_calibrateRCOSC.c
    source/ti/drivers/pin/PINCC26XX.c
    source/ti/drivers/pin/PINCC26XX.h
    source/ti/drivers/utils/List.c
    source/ti/drivers/utils/List.h
    kernel/nortos/NoRTOS.c
    kernel/nortos/NoRTOS.h
    kernel/nortos/dpl/ClockPTimer_nortos.c
    kernel/nortos/dpl/DebugP_nortos.c
    kernel/nortos/dpl/HwiPCC26XX_nortos.c
    kernel/nortos/dpl/MutexP_nortos.c
    kernel/nortos/dpl/PowerCC26XX_nortos.c
    kernel/nortos/dpl/QueueP.h
    kernel/nortos/dpl/QueueP_nortos.c
    kernel/nortos/dpl/SemaphoreP_nortos.c
    kernel/nortos/dpl/SwiP_nortos.c
    kernel/nortos/dpl/SystemP_nortos.c
    kernel/nortos/dpl/TimerP.h
    kernel/nortos/dpl/TimerPCC26XX_nortos.c
    kernel/nortos/startup/startup_cc13xx_cc26xx_gcc.c
)

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=$ROOT/third_party/ti_coresdk
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

curl -fsSL -o "$TMP/sdk.tar.gz" "https://codeload.github.com/$REPO/tar.gz/$COMMIT"
echo "$SHA256  $TMP/sdk.tar.gz" | sha256sum -c --quiet -
tar -xzf "$TMP/sdk.tar.gz" -C "$TMP"
SRC=$TMP/${REPO#*/}-$COMMIT

rm -rf "$DEST"
cd "$SRC"
for pattern in "${FILES[@]}"; do
    # shellcheck disable=SC2086  # the patterns are globs
    for f in $pattern; do
        [[ -f $f ]] || {
            echo "missing: $f" >&2
            exit 1
        }
        install -D -m 644 "$f" "$DEST/$f"
    done
done
cat > "$DEST/VERSION" << EOF
Subset of https://github.com/$REPO (TI SimpleLink core SDK, BSD-3-Clause; see LICENSE)
commit $COMMIT
archive sha256 $SHA256
created by tools/vendor_coresdk.sh; do not edit
EOF
echo "vendored $(find "$DEST" -type f | wc -l) files into $DEST"
