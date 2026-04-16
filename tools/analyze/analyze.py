"""Core analysis mode functions for crashomon-analyze.

Each function corresponds to one CLI mode.  All return the output text
as a string or raise RuntimeError on failure.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

from .log_parser import ParsedTombstone, parse_tombstone
from .symbolizer import (
    fix_frame_module_offsets,
    format_raw_tombstone,
    format_symbolicated,
    parse_human_frame_pcs,
    parse_stackwalk_machine,
    read_minidump_process_info,
    read_minidump_thread_names,
    symbolize_with_store,
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


def _run_stackwalk(
    stackwalk: str, dmp: str, sym_paths: list[str], *, machine: bool = False
) -> str:
    """Run minidump_stackwalk and return stdout.

    Pass ``machine=True`` to add the ``-m`` flag for pipe-delimited output.
    Raises RuntimeError if the binary is not found, times out, or produces
    no output.
    """
    cmd = [stackwalk]
    if machine:
        cmd.append("-m")
    cmd.append(dmp)
    cmd += sym_paths
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except FileNotFoundError as exc:
        raise RuntimeError(f"minidump_stackwalk not found: {stackwalk!r}") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("minidump_stackwalk timed out") from exc
    if not result.stdout:
        raise RuntimeError(
            f"minidump_stackwalk produced no output (exit {result.returncode})"
        )
    return result.stdout


def _apply_minidump_metadata(
    tombstone: ParsedTombstone,
    dmp: str,
    module_bases: dict[str, int],
    raw_human: str,
) -> None:
    """Populate pid, crashing_tid, thread names, and correct module offsets."""
    pid, crashing_tid = read_minidump_process_info(dmp)
    tombstone.pid = pid
    tombstone.crashing_tid = crashing_tid

    frame_pcs = parse_human_frame_pcs(raw_human)
    fix_frame_module_offsets(tombstone, module_bases, frame_pcs)

    thread_names = read_minidump_thread_names(dmp)
    for thread in tombstone.threads:
        thread.name = thread_names.get(thread.tid, "")


def mode_minidump_store(store: str, dmp: str, stackwalk: str) -> str:
    """Mode 1: symbolicate a minidump against a symbol store."""
    raw_m = _run_stackwalk(stackwalk, dmp, [store], machine=True)
    raw_human = _run_stackwalk(stackwalk, dmp, [store])
    tombstone, symbols, module_bases = parse_stackwalk_machine(raw_m)
    _apply_minidump_metadata(tombstone, dmp, module_bases, raw_human)
    return format_symbolicated(tombstone, symbols)


def mode_stdin_store(text: str, store: str) -> str:
    """Mode 2: parse tombstone text from stdin and symbolicate via the symbol store.

    Frames are matched to .sym files by GNU build ID, which the daemon writes
    into every tombstone frame as ``(BuildId: <hex>)``.  No external tools are
    required.
    """
    tombstone = parse_tombstone(text)
    symbols = symbolize_with_store(tombstone, Path(store))
    return format_symbolicated(tombstone, symbols)


def mode_sym_file_minidump(sym_file: str, dmp: str, stackwalk: str) -> str:
    """Mode 3: symbolicate a minidump using a single explicit .sym file.

    Installs the .sym into a temporary Breakpad store layout and invokes
    minidump_stackwalk against it.
    """
    sym_text = Path(sym_file).read_text(encoding="utf-8")
    module_name, build_id = _parse_module_line(sym_text)

    with tempfile.TemporaryDirectory(prefix="crashomon_analyze_") as tmp:
        sym_dir = Path(tmp) / module_name / build_id
        sym_dir.mkdir(parents=True)
        shutil.copy2(sym_file, sym_dir / f"{module_name}.sym")
        raw_m = _run_stackwalk(stackwalk, dmp, [tmp], machine=True)
        raw_human = _run_stackwalk(stackwalk, dmp, [tmp])
    tombstone, symbols, module_bases = parse_stackwalk_machine(raw_m)
    _apply_minidump_metadata(tombstone, dmp, module_bases, raw_human)
    return format_symbolicated(tombstone, symbols)


def mode_raw_tombstone(dmp: str, stackwalk: str) -> str:
    """Mode 4: format a raw (unsymbolicated) tombstone from a minidump.

    Runs minidump_stackwalk -m (machine-readable, no symbols) and reformats
    the output as an Android-style tombstone.  Register values are not shown
    (not present in -m output); the daemon still shows them via the C++ path.
    """
    raw_m = _run_stackwalk(stackwalk, dmp, [], machine=True)
    raw_human = _run_stackwalk(stackwalk, dmp, [])
    tombstone, _symbols, module_bases = parse_stackwalk_machine(raw_m)
    _apply_minidump_metadata(tombstone, dmp, module_bases, raw_human)
    return format_raw_tombstone(tombstone)
