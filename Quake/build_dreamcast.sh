#!/bin/bash
#
# build_dreamcast.sh -- build QuakeSpasm for the Sega Dreamcast and package a
# bootable .cdi image.
#
# Steps:
#   1. compile + link quakespasm.elf   (Makefile.dreamcast, kos-cc)
#   2. bundle the cd/ folder + elf into quakespasm.cdi via mkdcdisc
#
# Game data lives under ./cd (mounts at /cd on the Dreamcast, the engine's
# basedir). Put pak files in cd/id1/ before running -- see cd/id1/*.txt.
#
# Usage:
#   ./build_dreamcast.sh              # build elf + cdi
#   ./build_dreamcast.sh elf          # build elf only
#   ./build_dreamcast.sh cdi          # package cdi only (elf must exist)
#   ./build_dreamcast.sh clean        # remove objects, elf and cdi
#   JOBS=8 ./build_dreamcast.sh       # override parallel jobs

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TARGET_ELF="quakespasm.elf"
TARGET_CDI="quakespasm.cdi"
DATA_DIR="cd"
GAME_NAME="quakespasm"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# --- Locate and source the KOS environment -------------------------------
if [ -z "$KOS_BASE" ]; then
    for envsh in \
        /opt/toolchains/dc/kos/environ.sh \
        "$HOME/toolchains/dc/kos/environ.sh"; do
        if [ -f "$envsh" ]; then
            . "$envsh"
            break
        fi
    done
fi
if [ -z "$KOS_BASE" ]; then
    echo "error: KOS environment not found. Source your kos environ.sh first." >&2
    exit 1
fi

build_elf() {
    echo ">> Building $TARGET_ELF (jobs=$JOBS)"
    make -f Makefile.dreamcast -j"$JOBS"
}

build_cdi() {
    if [ ! -f "$TARGET_ELF" ]; then
        echo "error: $TARGET_ELF not found; build it first." >&2
        exit 1
    fi
    if ! command -v mkdcdisc >/dev/null 2>&1; then
        echo "error: mkdcdisc not found in PATH." >&2
        exit 1
    fi
    if [ ! -d "$DATA_DIR/id1" ] || [ -z "$(find "$DATA_DIR/id1" -iname '*.pak' 2>/dev/null)" ]; then
        echo ">> WARNING: no .pak files under $DATA_DIR/id1 -- the disc will boot"
        echo "   but the engine won't find game data. See $DATA_DIR/id1/*.txt."
    fi
    echo ">> Packaging $TARGET_CDI (data track = $DATA_DIR/)"
    # -N: no padding -- keeps the image small (default pads to a full ~700MB
    # disc). Works fine on emulators and GDEMU/ODEs; drop -N if a specific
    # setup needs a fully padded track.
    mkdcdisc -N \
             -e "$TARGET_ELF" \
             -o "$TARGET_CDI" \
             -n "$GAME_NAME" \
             -D "$DATA_DIR"
    echo ">> Done: $TARGET_CDI ($(du -h "$TARGET_CDI" | cut -f1))"
}

case "${1:-all}" in
    elf)    build_elf ;;
    cdi)    build_cdi ;;
    clean)  make -f Makefile.dreamcast clean; rm -f "$TARGET_CDI" ;;
    all|"") build_elf; build_cdi ;;
    *)      echo "usage: $0 [all|elf|cdi|clean]" >&2; exit 1 ;;
esac
