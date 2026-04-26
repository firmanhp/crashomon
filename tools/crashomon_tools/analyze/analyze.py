"""Core analysis mode functions for crashomon-analyze.

Each function corresponds to one CLI mode.  All return the output text
as a string or raise RuntimeError on failure.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from crashomon_tools.syms import _run_dump_syms, _store_sym

from .log_parser import ParsedTombstone
from .symbolizer import (
    format_raw_tombstone,
    format_symbolicated,
    parse_stackwalk_json,
    read_minidump_annotations,
)


def _parse_module_line(sym_text: str) -> tuple[str, str]:
    """Parse first line of a .sym file; return (module_name, build_id).

    Raises ValueError on invalid input.
    """
    first_line = sym_text.splitlines()[0] if sym_text else ""
    parts = first_line.split()
    if len(parts) < 5 or parts[0] != "MODULE":
        raise ValueError(f"Invalid MODULE line: {first_line!r}")
    return parts[4], parts[3]  # module_name, build_id


def _run_stackwalk_json(stackwalk: str, dmp: str, sym_paths: list[str]) -> dict:
    """Run minidump-stackwalk --json and return the parsed JSON dict.

    Raises RuntimeError if the binary is not found, times out, or produces
    no output. Raises ValueError if stdout is not valid JSON.
    """
    cmd = [stackwalk, "--json", dmp]
    for p in sym_paths:
        cmd += ["--symbols-path", p]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except FileNotFoundError as exc:
        raise RuntimeError(f"minidump-stackwalk not found: {stackwalk!r}") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("minidump-stackwalk timed out") from exc
    if not result.stdout:
        raise RuntimeError(f"minidump-stackwalk produced no output (exit {result.returncode})")
    return json.loads(result.stdout)


def _apply_annotations(tombstone: ParsedTombstone, dmp: str) -> None:
    annotations = read_minidump_annotations(dmp)
    tombstone.abort_message = annotations.get("abort_message", "")
    tombstone.terminate_type = annotations.get("terminate_type", "")


def mode_minidump_store(stores: list[str], dmp: str, stackwalk: str, show_trust: bool = False) -> str:
    """Mode 1: symbolicate a minidump against one or more symbol stores."""
    data = _run_stackwalk_json(stackwalk, dmp, stores)
    tombstone, symbols = parse_stackwalk_json(data)
    _apply_annotations(tombstone, dmp)
    return format_symbolicated(tombstone, symbols, show_trust)


def mode_sym_file_minidump(sym_file: str, dmp: str, stackwalk: str, show_trust: bool = False) -> str:
    """Mode 3: symbolicate a minidump using a single explicit .sym file.

    Installs the .sym into a temporary Breakpad store layout and invokes
    minidump-stackwalk against it.
    """
    sym_text = Path(sym_file).read_text(encoding="utf-8")
    module_name, build_id = _parse_module_line(sym_text)

    with tempfile.TemporaryDirectory(prefix="crashomon_analyze_") as tmp:
        sym_dir = Path(tmp) / module_name / build_id
        sym_dir.mkdir(parents=True)
        shutil.copy2(sym_file, sym_dir / f"{module_name}.sym")
        data = _run_stackwalk_json(stackwalk, dmp, [tmp])
    tombstone, symbols = parse_stackwalk_json(data)
    _apply_annotations(tombstone, dmp)
    return format_symbolicated(tombstone, symbols, show_trust)


def mode_raw_tombstone(dmp: str, stackwalk: str, show_trust: bool = False) -> str:
    """Mode 4: format a raw (unsymbolicated) tombstone from a minidump."""
    data = _run_stackwalk_json(stackwalk, dmp, [])
    tombstone, _symbols = parse_stackwalk_json(data)
    _apply_annotations(tombstone, dmp)
    return format_raw_tombstone(tombstone, show_trust)


def _extract_sysroot_symbols(
    modules: set[str],
    sysroot: Path,
    store: Path,
    dump_syms: str,
) -> None:
    """Run dump_syms on sysroot libraries matching unresolved module names."""
    for module_name in sorted(modules):
        basename = Path(module_name).name
        candidate = sysroot / "usr" / "lib" / basename
        if not candidate.exists():
            print(f"  sysroot: {module_name} not found in sysroot", file=sys.stderr)
            continue
        parsed = _run_dump_syms(dump_syms, candidate.resolve())
        if parsed is None:
            continue
        mod_name, build_id, sym_text = parsed
        _store_sym(store, mod_name, build_id, sym_text)
        print(f"  sysroot: extracted {mod_name}/{build_id}/{mod_name}.sym", file=sys.stderr)


def mode_minidump_sysroot(
    stores: list[str], sysroot: str, dmp: str, stackwalk: str, dump_syms: str, show_trust: bool = False
) -> str:
    """Mode 5: symbolicate a minidump using symbol stores and a sysroot.

    Runs dump_syms lazily on sysroot libraries that are referenced by the
    minidump but missing from any store. Generated .sym files are written
    to the first store so subsequent runs skip the conversion.
    """
    primary_store = Path(stores[0])
    sysroot_path = Path(sysroot)
    primary_store.mkdir(parents=True, exist_ok=True)

    data = _run_stackwalk_json(stackwalk, dmp, stores)
    tombstone, symbols = parse_stackwalk_json(data)

    resolved_modules = {
        frame.module_path
        for thread in tombstone.threads
        for frame in thread.frames
        if (thread.tid, frame.index) in symbols
    }
    unresolved_modules = {
        frame.module_path
        for thread in tombstone.threads
        for frame in thread.frames
        if frame.module_path and frame.module_path not in resolved_modules
    }

    if unresolved_modules:
        _extract_sysroot_symbols(unresolved_modules, sysroot_path, primary_store, dump_syms)

    data = _run_stackwalk_json(stackwalk, dmp, stores)
    tombstone, symbols = parse_stackwalk_json(data)
    _apply_annotations(tombstone, dmp)
    return format_symbolicated(tombstone, symbols, show_trust)
