"""Symbol store management — Breakpad store layout.

Breakpad store layout:
  <store>/<module_name>/<build_id>/<module_name>.sym
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path
from typing import NamedTuple


class SymbolEntry(NamedTuple):
    module: str
    build_id: str
    size_bytes: int
    mtime: float  # Unix timestamp


def _parse_module_line(sym_text: str) -> tuple[str, str]:
    """Parse first line of a .sym file; return (module_name, build_id).

    Raises ValueError on invalid input.
    """
    first_line = sym_text.splitlines()[0] if sym_text else ""
    parts = first_line.split()
    if len(parts) < 5 or parts[0] != "MODULE":
        raise ValueError(f"Invalid MODULE line: {first_line!r}")
    return parts[4], parts[3]  # module_name, build_id


def add_binary(
    store_path: str | Path,
    binary_path: str | Path,
    *,
    dump_syms: str = "dump_syms",
) -> Path:
    """Run dump_syms on binary_path and store the resulting .sym file.

    Returns the path to the stored .sym file.
    Raises FileNotFoundError if binary_path does not exist.
    Raises RuntimeError if dump_syms fails.
    """
    binary = Path(binary_path)
    if not binary.exists():
        raise FileNotFoundError(f"binary not found: {binary}")

    try:
        result = subprocess.run(
            [dump_syms, str(binary)],
            capture_output=True,
            text=True,
            timeout=120,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(f"dump_syms not found: {dump_syms!r}") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"dump_syms timed out for {binary}") from exc

    if result.returncode != 0 or not result.stdout:
        raise RuntimeError(f"dump_syms failed for {binary} (exit {result.returncode})")

    return add_sym_text(store_path, result.stdout)


def add_sym_text(store_path: str | Path, sym_text: str) -> Path:
    """Store a .sym file given its text content.

    Parses the MODULE line to determine the store path.
    Raises ValueError if sym_text does not start with a valid MODULE line.
    Returns the path to the stored .sym file.
    """
    module_name, build_id = _parse_module_line(sym_text)
    store = Path(store_path)
    dest_dir = store / module_name / build_id
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / f"{module_name}.sym"
    dest.write_text(sym_text, encoding="utf-8")
    return dest


def list_symbols(store_path: str | Path) -> list[SymbolEntry]:
    """Return all stored symbol entries, sorted by (module, build_id)."""
    store = Path(store_path)
    results: list[SymbolEntry] = []
    if not store.is_dir():
        return results
    for module_dir in sorted(store.iterdir()):
        if not module_dir.is_dir():
            continue
        for build_dir in sorted(module_dir.iterdir()):
            if not build_dir.is_dir():
                continue
            sym_file = build_dir / f"{module_dir.name}.sym"
            if sym_file.exists():
                st = sym_file.stat()
                results.append(
                    SymbolEntry(
                        module=module_dir.name,
                        build_id=build_dir.name,
                        size_bytes=st.st_size,
                        mtime=st.st_mtime,
                    )
                )
    return results


def prune(store_path: str | Path, keep: int = 3) -> list[str]:
    """Delete oldest symbol versions beyond keep per module.

    Returns list of removed paths relative to the store.
    Raises ValueError if keep < 1.
    """
    if keep < 1:
        raise ValueError("keep must be >= 1")
    store = Path(store_path)
    removed: list[str] = []
    if not store.is_dir():
        return removed
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
        for _, build_dir in versions[: max(0, len(versions) - keep)]:
            removed.append(f"{module_dir.name}/{build_dir.name}")
            shutil.rmtree(build_dir)
    return removed


def find_sym(store_path: str | Path, module: str, build_id: str) -> Path | None:
    """Return path to .sym file or None if not present."""
    sym = Path(store_path) / module / build_id / f"{module}.sym"
    return sym if sym.exists() else None
