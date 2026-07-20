#!/usr/bin/env python3
"""
Convert a GNU ld .map file into a compact symbol file for Dreamcast on-screen profiler.

Output format (one per line):
  8c012340 FuncName

Notes:
- Output is sorted by address (ascending) to allow binary search on-target.
- This is intentionally simple and heuristic: it extracts (address, symbol) pairs from .map.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


ADDR_RE = re.compile(
    r"^\s*(?:0x)?(8c[0-9a-fA-F]{6,8})\s+([^\s]+)\s*$"
)


def is_bad_symbol(name: str) -> bool:
    # Filter out common non-function / noise labels.
    if not name:
        return True
    if name.startswith("."):
        return True
    if name.startswith("$"):
        return True
    if name.startswith("*"):
        return True
    if name.startswith("__gnu_local_gp"):
        return True
    # Linker internal-ish entries that don't help on-screen
    if name in {"__CTOR_LIST__", "__DTOR_LIST__", "__CTOR_END__", "__DTOR_END__"}:
        return True
    # Local assembler labels
    if name.startswith(".L"):
        return True
    return False


def parse_map_text(text: str, max_syms: int | None) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    seen: set[int] = set()

    for line in text.splitlines():
        m = ADDR_RE.match(line)
        if not m:
            continue
        addr_hex, name = m.group(1), m.group(2)
        if is_bad_symbol(name):
            continue
        addr = int(addr_hex, 16)
        # Keep only symbols in Dreamcast RAM text range (heuristic).
        if addr < 0x8C000000 or addr >= 0x8D000000:
            continue
        # Prefer first occurrence for an address.
        if addr in seen:
            continue
        seen.add(addr)
        out.append((addr, name))
        if max_syms is not None and len(out) >= max_syms:
            break

    out.sort(key=lambda x: x[0])
    return out


def parse_nm_symbols(elf_file: Path, max_syms: int | None) -> list[tuple[int, str]]:
    nm_bin = os.environ.get("KOS_NM", "sh-elf-nm")
    try:
        proc = subprocess.run(
            [nm_bin, "-n", str(elf_file)],
            check=True,
            capture_output=True,
            text=True,
        )
    except Exception as exc:
        raise RuntimeError(f"failed to run {nm_bin}: {exc}") from exc

    out: list[tuple[int, str]] = []
    seen: set[int] = set()

    for line in proc.stdout.splitlines():
        # Typical: 8c1a9330 t _SampleVertexLight
        parts = line.strip().split(None, 2)
        if len(parts) != 3:
            continue
        addr_s, typ, name = parts
        if not re.fullmatch(r"[0-9a-fA-F]+", addr_s):
            continue
        # Keep function-like text symbols.
        if typ not in {"t", "T", "w", "W"}:
            continue
        if is_bad_symbol(name):
            continue

        addr = int(addr_s, 16)
        if addr < 0x8C000000 or addr >= 0x8D000000:
            continue
        if addr in seen:
            continue
        seen.add(addr)
        out.append((addr, name))
        if max_syms is not None and len(out) >= max_syms:
            break

    out.sort(key=lambda x: x[0])
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Convert .map to prof.syms")
    ap.add_argument("map_file", type=Path, help="Input GNU ld .map file")
    ap.add_argument("--elf", type=Path, default=None, help="Optional ELF file to augment symbols using sh-elf-nm")
    ap.add_argument("-o", "--out", type=Path, default=Path("prof.syms"), help="Output file (default: prof.syms)")
    ap.add_argument("--max", type=int, default=0, help="Max symbols (0 = no limit)")
    args = ap.parse_args(argv)

    text = args.map_file.read_text(errors="replace")
    max_syms = None if args.max == 0 else args.max
    syms = parse_map_text(text, max_syms=max_syms)

    if args.elf is not None:
        nm_syms = parse_nm_symbols(args.elf, max_syms=max_syms)
        # Merge map + nm, preferring nm name when same address appears.
        merged: dict[int, str] = {addr: name for addr, name in syms}
        for addr, name in nm_syms:
            merged[addr] = name
        syms = sorted(merged.items(), key=lambda x: x[0])
        if max_syms is not None:
            syms = syms[:max_syms]

    with args.out.open("w", newline="\n") as f:
        for addr, name in syms:
            f.write(f"{addr:08x} {name}\n")

    print(f"Wrote {len(syms)} symbols -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

