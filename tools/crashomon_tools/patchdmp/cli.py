"""crashomon-patchdmp — patch missing build IDs in Crashpad minidumps."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import _BPEL_SIGNATURE, _iter_modules, _resolve_elf, compute_elf_build_id, patch_minidump


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
            elf = _resolve_elf(m.name, sysroot)
            bid = compute_elf_build_id(elf) if elf else None
            status = bid.hex() if bid else "ELF not found"
            print(f"  {m.name}  ->  {status}")
        return 0

    count = patch_minidump(data, sysroot=sysroot)
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
