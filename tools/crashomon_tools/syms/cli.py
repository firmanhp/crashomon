"""CLI entry point for crashomon-syms."""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

from . import _run_dump_syms, _store_sym


def _default_tool(name: str, script_dir: Path | None) -> str:
    """Return co-located binary path if executable, else bare name for PATH lookup."""
    if script_dir is not None:
        candidate = script_dir / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return name


def _iter_elfs(search_dir: Path) -> list[Path]:
    """Return ELF files under search_dir (identified by magic bytes)."""
    results = []
    for path in search_dir.rglob("*"):
        if not path.is_file():
            continue
        try:
            with open(path, "rb") as f:
                magic = f.read(4)
            if magic == b"\x7fELF":
                results.append(path)
        except OSError:
            continue
    return sorted(results)


_DEFAULT_SYSROOT_SEARCH = ("usr/lib", "usr/lib64", "lib", "lib64")


def _iter_sysroot_elfs(sysroot: Path) -> list[Path]:
    """Return deduplicated .so files from well-known sysroot library paths.

    Scans recursively within each lib directory to pick up plugin and
    subsystem subdirs (e.g. usr/lib/systemd/, usr/lib/pulseaudio/).
    Resolves symlinks to handle merged-usr layouts (lib/ -> usr/lib)
    where multiple directory entries point to the same physical file.
    """
    seen: set[Path] = set()
    results: list[Path] = []

    for rel in _DEFAULT_SYSROOT_SEARCH:
        lib_dir = sysroot / rel
        if not lib_dir.is_dir():
            continue
        for path in lib_dir.rglob("*"):
            if not path.name.endswith(".so") and ".so." not in path.name:
                continue
            if not path.is_file():
                continue
            real = path.resolve()
            if real in seen:
                continue
            seen.add(real)
            try:
                with open(real, "rb") as f:
                    if f.read(4) == b"\x7fELF":
                        results.append(real)
            except OSError:
                continue

    return sorted(results)


def cmd_add(args: argparse.Namespace, script_dir: Path | None) -> int:
    store = Path(args.store)
    store.mkdir(parents=True, exist_ok=True)
    dump_syms: str = (
        args.dump_syms if args.dump_syms is not None else _default_tool("dump_syms", script_dir)
    )

    binaries: list[Path] = []
    if args.sysroot:
        binaries = _iter_sysroot_elfs(Path(args.sysroot))
        if not binaries:
            print(
                f"crashomon-syms: no shared libraries found in sysroot {args.sysroot}",
                file=sys.stderr,
            )
            return 1
    elif args.recursive:
        binaries = _iter_elfs(Path(args.recursive))
        if not binaries:
            print(
                f"crashomon-syms: no ELF files found under {args.recursive}",
                file=sys.stderr,
            )
            return 1
    else:
        if not args.binaries:
            print("crashomon-syms add: no binaries specified.", file=sys.stderr)
            return 1
        binaries = [Path(b) for b in args.binaries]

    errors = 0
    for binary in binaries:
        if not binary.exists():
            print(f"crashomon-syms: not found: {binary}", file=sys.stderr)
            errors += 1
            continue

        parsed = _run_dump_syms(dump_syms, binary)
        if parsed is None:
            errors += 1
            continue

        module_name, build_id, sym_text = parsed
        dest = _store_sym(store, module_name, build_id, sym_text)
        print(f"  stored: {dest.relative_to(store)}")

    return 0 if errors == 0 else 1


def cmd_list(args: argparse.Namespace) -> int:
    store = Path(args.store)
    if not store.is_dir():
        print(f"crashomon-syms: not a directory: {store}", file=sys.stderr)
        return 1

    found = False
    for module_dir in sorted(store.iterdir()):
        if not module_dir.is_dir():
            continue
        for build_dir in sorted(module_dir.iterdir()):
            if not build_dir.is_dir():
                continue
            sym_file = build_dir / f"{module_dir.name}.sym"
            if sym_file.exists():
                size_kb = sym_file.stat().st_size // 1024
                print(f"  {module_dir.name}/{build_dir.name}  ({size_kb} KB)")
                found = True

    if not found:
        print("  (no symbols stored)")
    return 0


def cmd_prune(args: argparse.Namespace) -> int:
    store = Path(args.store)
    if not store.is_dir():
        print(f"crashomon-syms: not a directory: {store}", file=sys.stderr)
        return 1

    keep: int = args.keep
    if keep < 1:
        print("crashomon-syms: --keep must be >= 1", file=sys.stderr)
        return 1

    for module_dir in sorted(store.iterdir()):
        if not module_dir.is_dir():
            continue

        versions: list[tuple[float, Path]] = []
        for build_dir in module_dir.iterdir():
            if not build_dir.is_dir():
                continue
            sym_file = build_dir / f"{module_dir.name}.sym"
            if sym_file.exists():
                versions.append((sym_file.stat().st_mtime, build_dir))

        versions.sort()  # oldest first

        to_delete = versions[: max(0, len(versions) - keep)]
        for _, build_dir in to_delete:
            print(f"  removing: {module_dir.name}/{build_dir.name}")
            shutil.rmtree(build_dir)

    return 0


def main(script_dir: Path | None = None) -> int:
    """Entry point for crashomon-syms.

    script_dir: directory containing the invoking script, used to locate
    a co-installed dump_syms binary.  Pass None when invoked as a
    pip-installed console script.
    """
    parser = argparse.ArgumentParser(
        description="Crashomon Breakpad symbol store manager"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_add = sub.add_parser("add", help="Ingest debug binary into symbol store")
    p_add.add_argument("--store", required=True, metavar="DIR", help="Symbol store root")
    p_add.add_argument(
        "--recursive",
        metavar="DIR",
        help="Recursively add all ELF binaries found under DIR",
    )
    p_add.add_argument(
        "--sysroot",
        metavar="DIR",
        help="Scan a cross-compilation sysroot for shared libraries. "
        "Resolves symlinks to avoid duplicates from merged-usr layouts.",
    )
    p_add.add_argument("binaries", nargs="*", metavar="BINARY")
    p_add.add_argument(
        "--dump-syms",
        default=None,
        metavar="PATH",
        help="Path to the dump_syms binary (default: co-located dump_syms, then PATH)",
    )

    p_list = sub.add_parser("list", help="List stored symbols")
    p_list.add_argument("--store", required=True, metavar="DIR")

    p_prune = sub.add_parser("prune", help="Remove old symbol versions, keeping latest N")
    p_prune.add_argument("--store", required=True, metavar="DIR")
    p_prune.add_argument(
        "--keep",
        type=int,
        default=3,
        metavar="N",
        help="Number of most-recent versions to keep per module (default: 3)",
    )

    args = parser.parse_args()
    dispatch = {
        "add": lambda a: cmd_add(a, script_dir),
        "list": cmd_list,
        "prune": cmd_prune,
    }
    return dispatch[args.command](args)
