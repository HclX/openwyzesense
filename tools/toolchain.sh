#!/usr/bin/env bash
# Fetches the pinned build tools (tools/toolchain.lock) into .toolchain/, checking each archive's SHA-256.
# Idempotent: a tool already unpacked is left alone. Linux x86-64 only.
#   tools/toolchain.sh [gcc|openocd|uniflash]...   (default: gcc and openocd; uniflash for the v1 contact
#   bench: only when asked; the ESP32 relay's ESP-IDF: HclX/wyzesense_relay)
# TOOLCHAIN_DEST overrides .toolchain (to try an install somewhere else).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=${TOOLCHAIN_DEST:-$ROOT/.toolchain}
# shellcheck source-path=SCRIPTDIR source=toolchain.lock
source "$ROOT/tools/toolchain.lock"

fetch() { # name dir url sha256
    local name=$1 dir=$2 url=$3 sha=$4
    if [[ -d $DEST/$dir ]]; then
        echo "$name: $DEST/$dir"
        return
    fi
    mkdir -p "$DEST/.dl"
    local archive=$DEST/.dl/${url##*/}
    if [[ ! -f $archive ]] || ! echo "$sha  $archive" | sha256sum -c --quiet - 2> /dev/null; then
        echo "$name: downloading ${url##*/}"
        curl -fsSL --retry 3 -o "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    echo "$sha  $archive" | sha256sum -c --quiet -
    rm -rf "$DEST.tmp" && mkdir -p "$DEST.tmp"
    tar -xf "$archive" -C "$DEST.tmp"
    mv "$DEST.tmp/$dir" "$DEST/$dir"
    rm -rf "$DEST.tmp" "$archive"
    echo "$name: $DEST/$dir"
}

uniflash() { # TI's installer, unattended, into $DEST/uniflash; marked with its version
    local dir=$DEST/uniflash stamp=$DEST/uniflash/.ows-version
    if [[ -f $stamp && $(cat "$stamp") == "$UNIFLASH_VERSION" ]]; then
        echo "uniflash: $dir"
        return
    fi
    # an install made by hand (docs/design/v1-contact.md) of the same version: keep it, mark it
    if [[ -f $dir/uniflash_install.log ]] && grep -q "BuildID: ${UNIFLASH_VERSION%.*} ${UNIFLASH_VERSION##*.}" "$dir/uniflash_install.log"; then
        echo "$UNIFLASH_VERSION" > "$stamp"
    else
        mkdir -p "$DEST/.dl"
        local inst=$DEST/.dl/${UNIFLASH_URL##*/}
        if [[ ! -f $inst ]] || ! echo "$UNIFLASH_SHA256  $inst" | sha256sum -c --quiet - 2> /dev/null; then
            echo "uniflash: downloading ${UNIFLASH_URL##*/} (~380 MB)"
            curl -fsSL --retry 3 -o "$inst.part" "$UNIFLASH_URL"
            mv "$inst.part" "$inst"
        fi
        echo "$UNIFLASH_SHA256  $inst" | sha256sum -c --quiet -
        chmod +x "$inst"
        rm -rf "$dir"
        # it warns about libgconf-2.so.4: its GUI only; the command line (DSLite) does not need it
        "$inst" --mode unattended --prefix "$dir" > /dev/null || true
        [[ -x $dir/deskdb/content/TICloudAgent/linux/ccs_base/DebugServer/bin/DSLite ]] || {
            echo "uniflash: install failed" >&2
            exit 1
        }
        echo "$UNIFLASH_VERSION" > "$stamp"
        rm -f "$inst"
    fi
    # its scripting (run.sh) looks for the cloud agent where a CCS install has it
    ln -sfn ../../../../../TICloudAgentHostApp "$dir/deskdb/content/TICloudAgent/linux/ccs_base/cloudagent"
    echo "uniflash: $dir"
}

tools=("$@")
[[ ${#tools[@]} -gt 0 ]] || tools=(gcc openocd)
for t in "${tools[@]}"; do
    case $t in
        gcc) fetch gcc "$ARM_GCC_DIR" "$ARM_GCC_URL" "$ARM_GCC_SHA256" ;;
        openocd) fetch openocd "$OPENOCD_DIR" "$OPENOCD_URL" "$OPENOCD_SHA256" ;;
        uniflash) uniflash ;;
        *)
            echo "unknown tool: $t" >&2
            exit 2
            ;;
    esac
done
