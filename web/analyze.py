"""Core analysis mode functions for crashomon-analyze.

Each function corresponds to one CLI mode.  All return the output text
as a string or raise RuntimeError on failure.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

from web.log_parser import parse_tombstone
from web.symbol_store import _parse_module_line
from web.symbolizer import (
    format_raw_tombstone,
    format_symbolicated,
    parse_stackwalk_machine,
    symbolize_with_addr2line,
)


def _run_stackwalk(stackwalk: str, dmp: str, sym_paths: list[str]) -> str:
    """Run minidump_stackwalk and return stdout.

    Raises RuntimeError if the binary is not found, times out, or produces
    no output.
    """
    cmd = [stackwalk, dmp] + sym_paths
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
    return _run_stackwalk(stackwalk, dmp, [store])


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
        return _run_stackwalk(stackwalk, dmp, [tmp])


def mode_raw_tombstone(dmp: str, stackwalk: str) -> str:
    """Mode 4: format a raw (unsymbolicated) tombstone from a minidump.

    Runs minidump_stackwalk -m (machine-readable, no symbols) and reformats
    the output as an Android-style tombstone.  Register values are not shown
    (not present in -m output); the daemon still shows them via the C++ path.
    """
    cmd = [stackwalk, "-m", dmp]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except FileNotFoundError as exc:
        raise RuntimeError(f"minidump_stackwalk not found: {stackwalk!r}") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("minidump_stackwalk timed out") from exc
    tombstone = parse_stackwalk_machine(result.stdout)
    return format_raw_tombstone(tombstone)
