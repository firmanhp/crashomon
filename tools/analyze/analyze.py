"""Core analysis mode functions for crashomon-analyze.

Each function corresponds to one CLI mode.  All return the output text
as a string or raise RuntimeError on failure.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

from .log_parser import parse_tombstone
from .symbolizer import (
    build_build_id_index,
    format_raw_tombstone,
    format_symbolicated,
    parse_stackwalk_machine,
    symbolize_with_addr2line,
    symbolize_with_build_id_index,
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


def mode_minidump_store(store: str, dmp: str, stackwalk: str) -> str:
    """Mode 1: symbolicate a minidump against a symbol store."""
    raw = _run_stackwalk(stackwalk, dmp, [store], machine=True)
    tombstone, symbols = parse_stackwalk_machine(raw)
    return format_symbolicated(tombstone, symbols)


def mode_stdin_store(text: str, store: str, addr2line: str) -> str:
    """Mode 2: parse tombstone text from stdin and symbolicate with eu-addr2line."""
    tombstone = parse_tombstone(text)
    symbols = symbolize_with_addr2line(tombstone, addr2line)
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
        raw = _run_stackwalk(stackwalk, dmp, [tmp], machine=True)
    tombstone, symbols = parse_stackwalk_machine(raw)
    return format_symbolicated(tombstone, symbols)


def mode_stdin_debug_dir(text: str, debug_dir: str, addr2line: str) -> str:
    """Mode 5: parse tombstone from stdin, symbolicate using host-side debug ELFs.

    Builds a build-ID index by walking *debug_dir* for ELF files, then
    resolves each frame whose build ID matches a found ELF via eu-addr2line.
    Works even when target paths are inaccessible on the host.
    """
    tombstone = parse_tombstone(text)
    index = build_build_id_index(Path(debug_dir))
    symbols = symbolize_with_build_id_index(tombstone, index, addr2line)
    return format_symbolicated(tombstone, symbols)


def mode_raw_tombstone(dmp: str, stackwalk: str) -> str:
    """Mode 4: format a raw (unsymbolicated) tombstone from a minidump.

    Runs minidump_stackwalk -m (machine-readable, no symbols) and reformats
    the output as an Android-style tombstone.  Register values are not shown
    (not present in -m output); the daemon still shows them via the C++ path.
    """
    raw = _run_stackwalk(stackwalk, dmp, [], machine=True)
    tombstone, _symbols = parse_stackwalk_machine(raw)
    return format_raw_tombstone(tombstone)
