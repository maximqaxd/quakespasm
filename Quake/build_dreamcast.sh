#!/bin/bash
#
# build_dreamcast.sh -- build QuakeSpasm for the Sega Dreamcast and package a
# bootable .cdi image.
#
# Steps:
#   1. compile + link quakespasm.elf   (Makefile.dreamcast, kos-cc)
#   2. bundle the cd/ folder + elf into quakespasm.cdi via mkdcdisc
#
# Game data lives under ../Dreamcast/cd (mounts at /cd on the Dreamcast, the
# engine's basedir). Put pak files in Dreamcast/cd/id1/ before running.
#
# Usage:
#   ./build_dreamcast.sh              # build elf + cdi
#   ./build_dreamcast.sh elf          # build elf only
#   ./build_dreamcast.sh cdi          # package cdi only (elf must exist)
#   ./build_dreamcast.sh clean        # remove objects, elf and cdi
#   JOBS=8 ./build_dreamcast.sh       # override parallel jobs
#   USE_DC_PROFILER=1 ./build_dreamcast.sh   # on-screen function profiler:
#                                     #   builds with -finstrument-functions and
#                                     #   drops prof.syms into the cd/ folder so
#                                     #   the "prof" cvar shows named functions.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TARGET_ELF="quakespasm.elf"
# The bootable disc image is written next to the game data, at Dreamcast/.
TARGET_CDI="../Dreamcast/quakespasm.cdi"
# Disc data (id1/ with pak files + music) lives at <repo root>/Dreamcast/cd.
# The script runs from Quake/, so reach it one level up.
DATA_DIR="../Dreamcast/cd"
GAME_NAME="quakespasm"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
# Native PVR renderer on by default. Override with USE_PVR_RENDER=0 for the GLdc path.
USE_PVR_RENDER="${USE_PVR_RENDER:-1}"
# QuakeWorld protocol (client) built in; runtime opt-in via "connect <addr> qw".
USE_QW_PROTOCOL="${USE_QW_PROTOCOL:-1}"
# On-screen function profiler (off by default). When 1, the engine is built with
# -finstrument-functions and prof.syms is generated into the cd/ folder.
USE_DC_PROFILER="${USE_DC_PROFILER:-0}"

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
    echo ">> Building $TARGET_ELF (jobs=$JOBS, USE_PVR_RENDER=$USE_PVR_RENDER, USE_QW_PROTOCOL=$USE_QW_PROTOCOL, USE_DC_PROFILER=$USE_DC_PROFILER)"
    # make keys rebuilds off the .c timestamps, so flipping a -D flag (e.g. the
    # profiler) would NOT rebuild unchanged files -- the new define would silently
    # not take effect. Track the flag set in a stamp and force a clean when it
    # changes, in either direction.
    local stamp=".build_flags"
    # include a checksum of the Makefile so any flag/CFLAGS edit (not just the
    # command-line toggles) also forces the clean -- make itself doesn't rebuild
    # .o's when only the Makefile changed.
    local mkhash
    mkhash=$( (md5sum Makefile.dreamcast 2>/dev/null || cksum Makefile.dreamcast) | cut -d' ' -f1)
    local cur="PVR=$USE_PVR_RENDER QW=$USE_QW_PROTOCOL PROF=$USE_DC_PROFILER MK=$mkhash"
    if [ ! -f "$stamp" ] || [ "$(cat "$stamp" 2>/dev/null)" != "$cur" ]; then
        echo ">> build flags changed ($cur) -> make clean"
        make -f Makefile.dreamcast clean \
            USE_PVR_RENDER="$USE_PVR_RENDER" \
            USE_QW_PROTOCOL="$USE_QW_PROTOCOL" \
            USE_DC_PROFILER="$USE_DC_PROFILER" >/dev/null 2>&1 || true
    fi
    echo "$cur" > "$stamp"
    make -f Makefile.dreamcast -j"$JOBS" \
        USE_PVR_RENDER="$USE_PVR_RENDER" \
        USE_QW_PROTOCOL="$USE_QW_PROTOCOL" \
        USE_DC_PROFILER="$USE_DC_PROFILER"
    if [ "$USE_DC_PROFILER" = "1" ]; then
        build_syms
    fi
}

# Dump function symbols from the (unstripped, profiler) elf to cd/prof.syms so the
# on-target profiler can show names instead of raw addresses. Prefer the compact
# sorted output from map2profsyms.py; fall back to raw sh-elf-nm.
build_syms() {
    local out="$DATA_DIR/prof.syms"
    mkdir -p "$DATA_DIR"
    if command -v python3 >/dev/null 2>&1 && [ -f "quakespasm.map" ]; then
        echo ">> Generating $out (map2profsyms.py)"
        python3 tools/map2profsyms.py quakespasm.map --elf "$TARGET_ELF" -o "$out"
    elif command -v sh-elf-nm >/dev/null 2>&1; then
        echo ">> Generating $out (sh-elf-nm)"
        sh-elf-nm -n "$TARGET_ELF" | awk '$2 ~ /^[tTwW]$/ {print $1, $3}' > "$out"
        echo "   wrote $(wc -l < "$out") symbols"
    else
        echo ">> WARNING: neither python3 nor sh-elf-nm found; prof.syms not built"
        echo "   the profiler will show raw 0x8c... addresses."
    fi
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
    # Ship the engine's own pak (menu/progs assets) alongside the game data.
    if [ -f "quakespasm.pak" ]; then
        echo ">> Copying quakespasm.pak -> $DATA_DIR/id1/"
        mkdir -p "$DATA_DIR/id1"
        cp -f "quakespasm.pak" "$DATA_DIR/id1/quakespasm.pak"
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
    syms)   build_syms ;;
    clean)  make -f Makefile.dreamcast clean USE_PVR_RENDER="$USE_PVR_RENDER" USE_QW_PROTOCOL="$USE_QW_PROTOCOL" USE_DC_PROFILER="$USE_DC_PROFILER"; rm -f "$TARGET_CDI" "$DATA_DIR/prof.syms" quakespasm.map ;;
    all|"") build_elf; build_cdi ;;
    *)      echo "usage: $0 [all|elf|cdi|syms|clean]" >&2; exit 1 ;;
esac
