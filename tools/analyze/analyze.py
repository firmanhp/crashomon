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


def mode_minidump_store(store: str, dmp: str, stackwalk: str) -> str:
    """Mode 1: symbolicate a minidump against a symbol store."""
    data = _run_stackwalk_json(stackwalk, dmp, [store])
    tombstone, symbols = parse_stackwalk_json(data)
    _apply_annotations(tombstone, dmp)
    return format_symbolicated(tombstone, symbols)


def mode_sym_file_minidump(sym_file: str, dmp: str, stackwalk: str) -> str:
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
    return format_symbolicated(tombstone, symbols)


def mode_raw_tombstone(dmp: str, stackwalk: str) -> str:
    """Mode 4: format a raw (unsymbolicated) tombstone from a minidump."""
    data = _run_stackwalk_json(stackwalk, dmp, [])
    tombstone, _symbols = parse_stackwalk_json(data)
    _apply_annotations(tombstone, dmp)
    return format_raw_tombstone(tombstone)


def _extract_sysroot_symbols(
    modules: set[str],
    sysroot: Path,
    store: Path,
    dump_syms: str,
) -> None:
    """Run dump_syms on sysroot libraries matching unresolved module names."""
    search_dirs = [sysroot / "usr" / "lib"]

    for module_name in sorted(modules):
        basename = Path(module_name).name
        in_sysroot = False
        for search_dir in search_dirs:
            candidate = search_dir / basename
            if not candidate.exists():
                continue
            in_sysroot = True

            try:
                result = subprocess.run(
                    [dump_syms, str(candidate.resolve())],
                    capture_output=True,
                    text=True,
                    timeout=120,
                )
            except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
                print(f"  sysroot: dump_syms failed for {basename}: {exc}", file=sys.stderr)
                break

            if result.returncode != 0 or not result.stdout:
                print(
                    f"  sysroot: dump_syms failed for {basename} (exit {result.returncode})",
                    file=sys.stderr,
                )
                break

            first_line = result.stdout.splitlines()[0]
            parts = first_line.split()
            if len(parts) < 5 or parts[0] != "MODULE":
                print(f"  sysroot: unexpected MODULE line for {basename}", file=sys.stderr)
                break

            build_id = parts[3]
            mod_name = parts[4]
            dest_dir = store / mod_name / build_id
            dest_dir.mkdir(parents=True, exist_ok=True)
            sym_file = dest_dir / f"{mod_name}.sym"
            sym_file.write_text(result.stdout, encoding="utf-8")
            print(
                f"  sysroot: extracted {mod_name}/{build_id}/{mod_name}.sym",
                file=sys.stderr,
            )
            break

        if not in_sysroot:
            print(
                f"  sysroot: {module_name} not found in sysroot",
                file=sys.stderr,
            )


def mode_minidump_sysroot(
    store: str, sysroot: str, dmp: str, stackwalk: str, dump_syms: str
) -> str:
    """Mode 5: symbolicate a minidump using both a symbol store and a sysroot.

    Runs dump_syms lazily on sysroot libraries that are referenced by the
    minidump but missing from the store.  Generated .sym files are written
    to the store so subsequent runs skip the conversion.
    """
    store_path = Path(store)
    sysroot_path = Path(sysroot)
    store_path.mkdir(parents=True, exist_ok=True)

    # First pass: discover which modules are unresolved.
    data = _run_stackwalk_json(stackwalk, dmp, [store])
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
        _extract_sysroot_symbols(unresolved_modules, sysroot_path, store_path, dump_syms)

    # Second pass with enriched store.
    data = _run_stackwalk_json(stackwalk, dmp, [store])
    tombstone, symbols = parse_stackwalk_json(data)
    _apply_annotations(tombstone, dmp)
    return format_symbolicated(tombstone, symbols)
