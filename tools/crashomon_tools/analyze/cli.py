"""CLI entry point for crashomon-analyze."""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

from .analyze import (
    mode_minidump_store,
    mode_minidump_sysroot,
    mode_raw_tombstone,
    mode_sym_file_minidump,
)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="crashomon-analyze",
        description="Symbolicate Crashpad minidumps and tombstone text.",
        add_help=True,
    )
    parser.add_argument("--store", action="append", default=[], metavar="DIR",
        help="Symbol store directory (repeatable: --store dir1 --store dir2)")
    parser.add_argument("--minidump", metavar="FILE", default="")
    parser.add_argument("--symbols", metavar="FILE", default="")
    parser.add_argument("--sysroot", metavar="DIR", default="")
    parser.add_argument(
        "--dump-syms-binary", metavar="PATH", default="dump_syms",
        help="Path to dump_syms (needed for --sysroot lazy extraction; default: co-located, then PATH)",
    )
    parser.add_argument(
        "--stackwalk-binary", metavar="PATH", default="minidump-stackwalk",
        help="Path to minidump-stackwalk (default: co-located, then PATH)",
    )
    parser.add_argument(
        "--show-trust",
        action="store_true",
        default=False,
        help="Annotate every frame with its recovery method (context/cfi/frame_pointer/scan)",
    )
    return parser


def _resolve_binary(name: str, script_dir: Path | None) -> str:
    if script_dir is not None and Path(name).parent == Path("."):
        candidate = script_dir / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            print(f"crashomon-analyze: using {name}: {candidate}", file=sys.stderr)
            return str(candidate)
    resolved = shutil.which(name)
    if resolved is None:
        print(f"crashomon-analyze: {name!r} not found on PATH", file=sys.stderr)
        if "stackwalk" in name:
            print(
                "  Build it with:  cmake -B build -DCRASHOMON_BUILD_ANALYZE=ON"
                " && cmake --build build",
                file=sys.stderr,
            )
            print(
                "  Then pass:      --stackwalk-binary=build/rust_minidump_target"
                "/release/minidump-stackwalk",
                file=sys.stderr,
            )
            print("  See INSTALL.md §'Building minidump-stackwalk'", file=sys.stderr)
        sys.exit(1)
    print(f"crashomon-analyze: using {name}: {resolved}", file=sys.stderr)
    return resolved


def main(script_dir: Path | None = None) -> int:
    """Entry point for crashomon-analyze.

    script_dir: directory containing the invoking script, used to locate
    co-installed binaries (dump_syms, minidump-stackwalk).  Pass None when
    invoked as a pip-installed console script.
    """
    parser = _build_parser()
    args = parser.parse_args()

    def resolve(name: str) -> str:
        return _resolve_binary(name, script_dir)

    try:
        if args.store and args.sysroot and args.minidump:
            output = mode_minidump_sysroot(
                args.store,
                args.sysroot,
                args.minidump,
                resolve(args.stackwalk_binary),
                resolve(args.dump_syms_binary),
                args.show_trust,
            )
        elif args.store and args.minidump:
            output = mode_minidump_store(
                args.store,
                args.minidump,
                resolve(args.stackwalk_binary),
                args.show_trust,
            )
        elif args.symbols and args.minidump:
            output = mode_sym_file_minidump(
                args.symbols, args.minidump, resolve(args.stackwalk_binary), args.show_trust
            )
        elif args.minidump:
            output = mode_raw_tombstone(
                args.minidump, resolve(args.stackwalk_binary), args.show_trust
            )
        else:
            print("crashomon-analyze: no valid mode specified.", file=sys.stderr)
            parser.print_usage(sys.stderr)
            return 1
    except (RuntimeError, ValueError, OSError) as exc:
        print(f"crashomon-analyze: {exc}", file=sys.stderr)
        return 1

    sys.stdout.write(output)
    return 0
