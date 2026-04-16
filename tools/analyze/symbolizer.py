"""Symbolization and tombstone formatting utilities.

Provides:
  - symbolize_with_store: resolve frame addresses via a Breakpad symbol store
  - format_symbolicated: format a ParsedTombstone with symbol information
  - parse_stackwalk_machine: parse minidump_stackwalk -m output
  - format_raw_tombstone: format a ParsedTombstone without symbols (Mode 4)
"""

from __future__ import annotations

import bisect
import subprocess
from dataclasses import dataclass, field as dc_field
from pathlib import Path

from .log_parser import ParsedFrame, ParsedThread, ParsedTombstone

_SEP = "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***"

# Signal name → signal number (POSIX, x86-64 Linux).
# minidump_stackwalk -m only provides the signal name; we derive the number for display.
_SIGNAL_NUMBERS: dict[str, int] = {
    "SIGABRT": 6,
    "SIGBUS": 7,
    "SIGFPE": 8,
    "SIGILL": 4,
    "SIGKILL": 9,
    "SIGSEGV": 11,
    "SIGSYS": 31,
    "SIGTRAP": 5,
}


# ---------------------------------------------------------------------------
# Symbol store symbolization
# ---------------------------------------------------------------------------


@dataclass
class SymbolInfo:
    function: str = "??"
    source_file: str = ""
    source_line: int = 0


# Maps (tid, frame_index) -> SymbolInfo
SymbolTable = dict[tuple[int, int], SymbolInfo]


@dataclass
class _FuncEntry:
    """One FUNC record from a .sym file, with its associated LINE records."""

    start: int
    end: int  # exclusive (start + size)
    name: str
    # Parallel lists sorted by address, one entry per LINE record.
    line_addrs: list[int] = dc_field(default_factory=list)
    line_nums: list[int] = dc_field(default_factory=list)
    line_files: list[int] = dc_field(default_factory=list)


def _parse_sym_file(sym_path: Path) -> tuple[dict[int, str], list[_FuncEntry]]:
    """Parse a Breakpad .sym file into (file_map, funcs sorted by start address).

    Returns empty structures if the file cannot be read.
    """
    file_map: dict[int, str] = {}
    funcs: list[_FuncEntry] = []
    current: _FuncEntry | None = None

    try:
        text = sym_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}, []

    for line in text.splitlines():
        if line.startswith("FILE "):
            parts = line.split(" ", 2)
            if len(parts) >= 3:
                try:
                    file_map[int(parts[1])] = parts[2]
                except ValueError:
                    pass
        elif line.startswith("FUNC "):
            # FUNC <addr> <size> <param_size> <name>
            parts = line.split(" ", 4)
            if len(parts) >= 5:
                try:
                    start = int(parts[1], 16)
                    size = int(parts[2], 16)
                    current = _FuncEntry(start=start, end=start + size, name=parts[4])
                    funcs.append(current)
                except ValueError:
                    current = None
        elif current is not None and line and line[0].isdigit():
            # LINE record: <addr> <size> <line> <file_num>  (no keyword prefix)
            parts = line.split()
            if len(parts) >= 4:
                try:
                    current.line_addrs.append(int(parts[0], 16))
                    current.line_nums.append(int(parts[2]))
                    current.line_files.append(int(parts[3]))
                except ValueError:
                    pass

    funcs.sort(key=lambda f: f.start)
    return file_map, funcs


def _lookup_offset(
    file_map: dict[int, str],
    funcs: list[_FuncEntry],
    offset: int,
) -> SymbolInfo:
    """Binary-search parsed sym data for the FUNC+LINE containing offset."""
    if not funcs:
        return SymbolInfo()

    idx = bisect.bisect_right([f.start for f in funcs], offset) - 1
    if idx < 0:
        return SymbolInfo()

    func = funcs[idx]
    if offset >= func.end:
        return SymbolInfo()

    sym = SymbolInfo(function=func.name)
    if func.line_addrs:
        li = bisect.bisect_right(func.line_addrs, offset) - 1
        if li >= 0:
            sym.source_file = file_map.get(func.line_files[li], "")
            sym.source_line = func.line_nums[li]

    return sym


def _build_store_index(store: Path) -> dict[str, Path]:
    """Walk a Breakpad symbol store and return {raw_build_id: sym_file_path}.

    The store directory name is the Breakpad debug ID (uppercase hex + trailing
    "0" age suffix).  Stripping the trailing "0" and lowercasing recovers the
    raw GNU build ID that appears in tombstone (BuildId:) annotations.
    """
    index: dict[str, Path] = {}
    for sym_file in store.rglob("*.sym"):
        store_id = sym_file.parent.name
        raw_id = (store_id[:-1] if store_id.endswith("0") else store_id).lower()
        index[raw_id] = sym_file
    return index


def symbolize_with_store(
    tombstone: ParsedTombstone,
    store: Path,
) -> SymbolTable:
    """Resolve frame addresses using build IDs matched against a Breakpad symbol store.

    Frames whose build_id is absent or not found in the store map to
    SymbolInfo(function="??").  Never raises — missing or unparseable .sym
    files result in unknown symbols.
    """
    store_index = _build_store_index(store)
    parsed_cache: dict[Path, tuple[dict[int, str], list[_FuncEntry]]] = {}

    table: SymbolTable = {}
    for thread in tombstone.threads:
        for frame in thread.frames:
            if not frame.build_id:
                continue
            sym_path = store_index.get(frame.build_id)
            if sym_path is None:
                continue
            if sym_path not in parsed_cache:
                parsed_cache[sym_path] = _parse_sym_file(sym_path)
            file_map, funcs = parsed_cache[sym_path]
            sym = _lookup_offset(file_map, funcs, frame.module_offset)
            if sym.function not in ("??", ""):
                table[(thread.tid, frame.index)] = sym

    return table


# ---------------------------------------------------------------------------
# Tombstone formatter (symbolicated)
# ---------------------------------------------------------------------------


def _emit_stack_trace_section(
    thread: ParsedThread,
    symbols: SymbolTable,
    out: list[str],
    addr_w: int,
) -> None:
    """Emit Android-style backtrace frames for *thread*.

    Outputs one ``    #N pc 0xADDR  MODULE  (func) [file:line]`` line per
    frame, matching the format produced by the C++ daemon so that output is
    re-parseable by ``parse_tombstone``.  The ``backtrace:`` section header
    is emitted by the caller only for the crashing thread.
    """
    for frame in thread.frames:
        module = frame.module_path or "???"
        addr = f"0x{frame.module_offset:0{addr_w}x}"
        sym = symbols.get((thread.tid, frame.index))

        trailing = ""
        if sym and sym.function not in ("??", ""):
            if sym.source_file and sym.source_line > 0:
                trailing = f"({sym.function}) [{sym.source_file}:{sym.source_line}]"
            else:
                trailing = f"({sym.function})"
        elif frame.trailing:
            trailing = frame.trailing

        if trailing:
            out.append(f"    #{frame.index:02d} pc {addr}  {module}  {trailing}\n")
        else:
            out.append(f"    #{frame.index:02d} pc {addr}  {module}\n")


def _tombstone_addr_width(tombstone: ParsedTombstone) -> int:
    """Return 16 if any frame offset exceeds 32 bits, else 8."""
    for thread in tombstone.threads:
        for frame in thread.frames:
            if frame.module_offset > 0xFFFFFFFF:
                return 16
    return 8


def format_symbolicated(tombstone: ParsedTombstone, symbols: SymbolTable) -> str:
    """Format a ParsedTombstone with resolved symbols into tombstone text."""
    addr_w = _tombstone_addr_width(tombstone)
    out: list[str] = []
    out.append(_SEP + "\n")
    out.append(
        f"pid: {tombstone.pid}, tid: {tombstone.crashing_tid},"
        f" name: {tombstone.process_name}  >>> {tombstone.process_name} <<<\n"
    )

    slash = tombstone.signal_info.find(" / ")
    if slash != -1:
        sig_name = tombstone.signal_info[:slash]
        code_name = tombstone.signal_info[slash + 3 :]
        out.append(
            f"signal {tombstone.signal_number} ({sig_name}),"
            f" code {tombstone.signal_code} ({code_name}),"
            f" fault addr 0x{tombstone.fault_addr:x}\n"
        )
    else:
        out.append(
            f"signal {tombstone.signal_number} ({tombstone.signal_info}),"
            f" fault addr 0x{tombstone.fault_addr:x}\n"
        )

    if tombstone.timestamp:
        out.append(f"timestamp: {tombstone.timestamp}\n")

    if tombstone.threads and tombstone.threads[0].is_crashing:
        out.append("\nbacktrace:\n")
        _emit_stack_trace_section(tombstone.threads[0], symbols, out, addr_w)

    for thread in tombstone.threads[1:]:
        if thread.name:
            out.append(f"\n--- --- --- thread {thread.tid} ({thread.name}) --- --- ---\n")
        else:
            out.append(f"\n--- --- --- thread {thread.tid} --- --- ---\n")
        _emit_stack_trace_section(thread, symbols, out, addr_w)

    if tombstone.minidump_path:
        out.append(f"\nminidump saved to: {tombstone.minidump_path}\n")

    out.append(_SEP + "\n")
    return "".join(out)


# ---------------------------------------------------------------------------
# minidump_stackwalk -m parser + raw tombstone formatter (Mode 4)
# ---------------------------------------------------------------------------


def parse_stackwalk_machine(text: str) -> tuple[ParsedTombstone, SymbolTable]:
    """Parse minidump_stackwalk -m pipe-delimited output into a ParsedTombstone.

    minidump_stackwalk -m produces lines like:
      OS|Linux|...
      CPU|amd64|...|4
      Crash|SIGSEGV|0xdeadbeef|0          (crash_reason|addr|crashing_thread_idx)
      Module|name|ver|debug_file|debug_id|base|max|main
      thread_num|frame_num|module|func|file|line|offset

    Returns (tombstone, symbols).  Raises ValueError if no frame lines are found.
    """
    result = ParsedTombstone()
    threads: dict[int, ParsedThread] = {}
    table: SymbolTable = {}
    crashing_thread_idx = 0

    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split("|")

        if parts[0] == "Crash" and len(parts) >= 3:
            # Crash|signal_name|address|crashing_thread_index
            result.signal_info = parts[1]
            # Breakpad formats crash_reason as "SIGSEGV /SEGV_MAPERR" (space-slash,
            # no trailing space), so split on whitespace and take the first token.
            result.signal_number = _SIGNAL_NUMBERS.get(parts[1].split()[0], 0)
            try:
                result.fault_addr = int(parts[2], 16)
            except ValueError:
                result.fault_addr = 0
            try:
                crashing_thread_idx = int(parts[3]) if len(parts) > 3 else 0
            except ValueError:
                crashing_thread_idx = 0
            continue

        if parts[0] == "OS" and len(parts) >= 2:
            # No process name in -m output; leave blank.
            continue

        # Frame line: thread_num|frame_num|module|func|file|line|offset
        if parts[0].isdigit() and len(parts) >= 7:
            thread_num = int(parts[0])
            frame_num = int(parts[1])
            module = parts[2]
            func = parts[3]
            src_file = parts[4]
            src_line_str = parts[5]
            try:
                offset = int(parts[6], 16)
            except ValueError:
                offset = 0

            if thread_num not in threads:
                threads[thread_num] = ParsedThread(
                    tid=thread_num,
                    is_crashing=(thread_num == crashing_thread_idx),
                )
            threads[thread_num].frames.append(
                ParsedFrame(
                    index=frame_num,
                    module_offset=offset,
                    module_path=module,
                )
            )

            if func:
                sym = SymbolInfo(function=func)
                if src_file and src_file != "??":
                    sym.source_file = src_file
                    try:
                        sym.source_line = int(src_line_str)
                    except ValueError:
                        pass
                table[(thread_num, frame_num)] = sym

    if not threads:
        raise ValueError("No thread frames found in minidump_stackwalk -m output")

    # Build ordered thread list: crashing thread first.
    ordered = sorted(threads.keys())
    if crashing_thread_idx in ordered:
        ordered.remove(crashing_thread_idx)
        ordered.insert(0, crashing_thread_idx)

    result.threads = [threads[n] for n in ordered]
    return result, table


def format_raw_tombstone(tombstone: ParsedTombstone) -> str:
    """Format a ParsedTombstone without symbol information."""
    return format_symbolicated(tombstone, {})
