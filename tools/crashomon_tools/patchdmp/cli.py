"""crashomon-patchdmp — patch missing build IDs in Crashpad minidumps."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import (
    _BPEL_SIGNATURE,
    _iter_modules,
    _lookup_override,
    _resolve_elf,
    compute_elf_build_id,
    patch_minidump,
)


def _parse_build_id_overrides(specs: list[str]) -> dict[str, bytes] | None:
    """Parse --build-id NAME=HEX specs into a {name: bytes} dict.

    NAME may be a basename (libfoo.so) or a full path (/usr/lib/libfoo.so).
    HEX must be an even-length hex string (e.g. 32 chars for a 16-byte XOR
    fallback, 40 chars for a SHA-1 GNU build ID).
    """
    if not specs:
        return None
    overrides: dict[str, bytes] = {}
    for spec in specs:
        if "=" not in spec:
            raise ValueError(f"--build-id {spec!r}: expected NAME=HEXBYTES")
        name, _, hex_str = spec.partition("=")
        if not name:
            raise ValueError(f"--build-id {spec!r}: NAME must not be empty")
        if len(hex_str) % 2 != 0:
            raise ValueError(
                f"--build-id {spec!r}: hex string has odd length {len(hex_str)}"
            )
        try:
            overrides[name] = bytes.fromhex(hex_str)
        except ValueError:
            raise ValueError(f"--build-id {spec!r}: {hex_str!r} is not valid hex")
    return overrides


def main(script_dir: Path | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="crashomon-patchdmp",
        description=(
            "Patch Crashpad minidumps to supply Breakpad XOR-fallback build IDs "
            "for modules that lack a .note.gnu.build-id ELF note."
        ),
    )
    parser.add_argument("minidump", metavar="MINIDUMP", help="Path to .dmp file")
    parser.add_argument(
        "--sysroot",
        metavar="DIR",
        default="",
        help="Root directory for resolving ELF module paths (e.g. /path/to/sysroot)",
    )
    parser.add_argument(
        "--build-id",
        metavar="NAME=HEX",
        action="append",
        default=[],
        dest="build_ids",
        help=(
            "Override build ID for a module (repeatable). NAME is matched by "
            "basename or full path; HEX is the raw build ID bytes as a hex string "
            "(e.g. the INFO CODE_ID value from dump_syms). Takes priority over ELF "
            "disk lookup."
        ),
    )
    out_group = parser.add_mutually_exclusive_group()
    out_group.add_argument("--output", metavar="FILE", help="Write patched minidump to FILE")
    out_group.add_argument("--in-place", action="store_true", help="Patch MINIDUMP in place")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="List modules needing patches without writing output",
    )

    args = parser.parse_args()

    if not args.dry_run and not args.output and not args.in_place:
        parser.error("specify --output FILE, --in-place, or --dry-run")

    try:
        overrides = _parse_build_id_overrides(args.build_ids)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    dmp_path = Path(args.minidump)
    try:
        data = bytearray(dmp_path.read_bytes())
    except OSError as exc:
        print(f"error: cannot read {dmp_path}: {exc}", file=sys.stderr)
        return 1

    sysroot = Path(args.sysroot) if args.sysroot else None

    if args.dry_run:
        needs_patch = [
            m
            for m in _iter_modules(bytes(data))
            if m.cv_sig != _BPEL_SIGNATURE and m.cv_rva != 0
        ]
        if not needs_patch:
            print("No modules need patching.")
        for m in needs_patch:
            bid = _lookup_override(m.name, overrides or {})
            source = "override"
            if bid is None:
                elf = _resolve_elf(m.name, sysroot)
                bid = compute_elf_build_id(elf) if elf else None
                source = "ELF" if bid else "not found"
            status = f"{bid.hex()} ({source})" if bid else source
            print(f"  {m.name}  ->  {status}")
        return 0

    count = patch_minidump(data, sysroot=sysroot, build_id_overrides=overrides)
    print(f"Patched {count} module(s).")

    dest = dmp_path if args.in_place else Path(args.output)
    try:
        dest.write_bytes(data)
    except OSError as exc:
        print(f"error: cannot write {dest}: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
