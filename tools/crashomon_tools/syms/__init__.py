"""Shared symbol store primitives used by crashomon-syms and crashomon-analyze."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def _run_dump_syms(dump_syms: str, binary: Path) -> tuple[str, str, str] | None:
    """Run dump_syms on binary; return (module_name, build_id, sym_text) or None on failure."""
    try:
        result = subprocess.run(
            [dump_syms, str(binary)],
            capture_output=True,
            text=True,
            timeout=120,
        )
    except FileNotFoundError:
        print(f"dump_syms not found: {dump_syms!r}", file=sys.stderr)
        return None
    except subprocess.TimeoutExpired:
        print(f"dump_syms timed out for {binary}", file=sys.stderr)
        return None

    if result.returncode != 0 or not result.stdout:
        print(f"dump_syms failed for {binary} (exit {result.returncode})", file=sys.stderr)
        if result.stderr:
            print(result.stderr.rstrip(), file=sys.stderr)
        return None

    first_line = result.stdout.splitlines()[0]
    parts = first_line.split()
    if len(parts) < 5 or parts[0] != "MODULE":
        print(f"dump_syms: unexpected MODULE line:\n  {first_line}", file=sys.stderr)
        return None

    return parts[4], parts[3], result.stdout  # module_name, build_id, sym_text


def _store_sym(store: Path, module_name: str, build_id: str, sym_text: str) -> Path:
    """Write sym_text to <store>/<module_name>/<build_id>/<module_name>.sym."""
    dest_dir = store / module_name / build_id
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / f"{module_name}.sym"
    dest.write_text(sym_text, encoding="utf-8")
    return dest
