"""Symbolization and tombstone formatting utilities.

Provides:
  - symbolize_with_addr2line: resolve frame addresses via eu-addr2line
  - format_symbolicated: format a ParsedTombstone with symbol information
  - parse_stackwalk_machine: parse minidump_stackwalk -m output
  - format_raw_tombstone: format a ParsedTombstone without symbols (Mode 4)
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path

from web.log_parser import ParsedFrame, ParsedThread, ParsedTombstone

_SEP = "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***"


# ---------------------------------------------------------------------------
# eu-addr2line symbolization
# ---------------------------------------------------------------------------


@dataclass
class SymbolInfo:
    function: str = "??"
    source_file: str = ""
    source_line: int = 0


# Maps (tid, frame_index) -> SymbolInfo
SymbolTable = dict[tuple[int, int], SymbolInfo]


def _query_addr2line(
    module: str, addr2line: str, refs: list[tuple[int, int, int]]
) -> list[str]:
    """Call addr2line once for all (tid, index, offset) refs in one module.

    Returns raw output lines (2 lines per address: function, then file:line).
    """
    cmd = [addr2line, "-e", module, "-f", "-s"]
    cmd += [f"0x{offset:016x}" for _, _, offset in refs]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []
    if result.returncode != 0:
        return []
    return result.stdout.splitlines()


def _parse_addr2line_output(func_line: str, loc_line: str) -> SymbolInfo:
    """Parse one (function, file:line) pair from addr2line output."""
    sym = SymbolInfo(function=func_line or "??")
    # loc_line is "file.c:142" or "??:0"
    colon = loc_line.rfind(":")
    if colon != -1:
        src = loc_line[:colon]
        num = loc_line[colon + 1 :]
        sym.source_file = "" if src == "??" else src
        try:
            sym.source_line = int(num)
        except ValueError:
            sym.source_line = 0
    return sym


def symbolize_with_addr2line(
    tombstone: ParsedTombstone, addr2line_binary: str = "eu-addr2line"
) -> SymbolTable:
    """Resolve frame addresses via eu-addr2line; return a SymbolTable.

    Frames whose module binary is not present on this machine are mapped
    to SymbolInfo with function="??".  Never raises — missing binaries
    or addr2line errors result in unknown symbols.
    """
    # Group frames by module path.
    by_module: dict[str, list[tuple[int, int, int]]] = {}
    for thread in tombstone.threads:
        for frame in thread.frames:
            if frame.module_path:
                by_module.setdefault(frame.module_path, []).append(
                    (thread.tid, frame.index, frame.module_offset)
                )

    table: SymbolTable = {}
    for module, refs in by_module.items():
        if not Path(module).is_file():
            for tid, idx, _ in refs:
                table[(tid, idx)] = SymbolInfo()
            continue

        lines = _query_addr2line(module, addr2line_binary, refs)
        for i, (tid, idx, _) in enumerate(refs):
            base = i * 2
            func = lines[base] if base < len(lines) else "??"
            loc = lines[base + 1] if base + 1 < len(lines) else ""
            table[(tid, idx)] = _parse_addr2line_output(func, loc)

    return table


# ---------------------------------------------------------------------------
# Tombstone formatter (symbolicated)
# ---------------------------------------------------------------------------


def _emit_frames(
    thread: ParsedThread, symbols: SymbolTable, out: list[str]
) -> None:
    for frame in thread.frames:
        mod = frame.module_path or "???"
        sym = symbols.get((thread.tid, frame.index))
        if sym and sym.function != "??":
            if sym.source_file and sym.source_line > 0:
                out.append(
                    f"    #{frame.index:02d} pc 0x{frame.module_offset:016x}"
                    f"  {mod} ({sym.function}) [{sym.source_file}:{sym.source_line}]\n"
                )
            else:
                out.append(
                    f"    #{frame.index:02d} pc 0x{frame.module_offset:016x}"
                    f"  {mod} ({sym.function})\n"
                )
        elif not frame.module_path:
            out.append(
                f"    #{frame.index:02d} pc 0x{frame.module_offset:016x}  ???\n"
            )
        elif frame.trailing:
            out.append(
                f"    #{frame.index:02d} pc 0x{frame.module_offset:016x}"
                f"  {frame.module_path} {frame.trailing}\n"
            )
        else:
            out.append(
                f"    #{frame.index:02d} pc 0x{frame.module_offset:016x}"
                f"  {frame.module_path}\n"
            )


def format_symbolicated(tombstone: ParsedTombstone, symbols: SymbolTable) -> str:
    """Format a ParsedTombstone with resolved symbols into tombstone text."""
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
            f" fault addr 0x{tombstone.fault_addr:016x}\n"
        )
    else:
        out.append(
            f"signal {tombstone.signal_number} ({tombstone.signal_info}),"
            f" fault addr 0x{tombstone.fault_addr:016x}\n"
        )

    if tombstone.timestamp:
        out.append(f"timestamp: {tombstone.timestamp}\n")

    if tombstone.threads and tombstone.threads[0].is_crashing:
        out.append("\nbacktrace:\n")
        _emit_frames(tombstone.threads[0], symbols, out)

    for thread in tombstone.threads[1:]:
        if thread.name:
            out.append(f"\n--- --- --- thread {thread.tid} ({thread.name}) --- --- ---\n")
        else:
            out.append(f"\n--- --- --- thread {thread.tid} --- --- ---\n")
        _emit_frames(thread, symbols, out)

    if tombstone.minidump_path:
        out.append(f"\nminidump saved to: {tombstone.minidump_path}\n")

    out.append(_SEP + "\n")
    return "".join(out)


# ---------------------------------------------------------------------------
# minidump_stackwalk -m parser + raw tombstone formatter (Mode 4)
# ---------------------------------------------------------------------------


def parse_stackwalk_machine(text: str) -> ParsedTombstone:
    """Parse minidump_stackwalk -m pipe-delimited output into a ParsedTombstone.

    minidump_stackwalk -m produces lines like:
      OS|Linux|...
      CPU|amd64|...|4
      Crash|SIGSEGV|0xdeadbeef|0          (crash_reason|addr|crashing_thread_idx)
      Module|name|ver|debug_file|debug_id|base|max|main
      thread_num|frame_num|module|func|file|line|offset

    Raises ValueError if no crash line is found.
    """
    result = ParsedTombstone()
    threads: dict[int, ParsedThread] = {}
    crashing_thread_idx = 0

    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split("|")

        if parts[0] == "Crash" and len(parts) >= 3:
            # Crash|signal_name|address|crashing_thread_index
            result.signal_info = parts[1]
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

    if not threads:
        raise ValueError("No thread frames found in minidump_stackwalk -m output")

    # Build ordered thread list: crashing thread first.
    ordered = sorted(threads.keys())
    if crashing_thread_idx in ordered:
        ordered.remove(crashing_thread_idx)
        ordered.insert(0, crashing_thread_idx)

    result.threads = [threads[n] for n in ordered]
    return result


def format_raw_tombstone(tombstone: ParsedTombstone) -> str:
    """Format a ParsedTombstone without symbol information."""
    return format_symbolicated(tombstone, {})
