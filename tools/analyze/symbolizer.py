"""Symbolization and tombstone formatting utilities.

Provides:
  - parse_stackwalk_json: convert minidump-stackwalk --json output to ParsedTombstone
  - format_symbolicated: format a ParsedTombstone with symbol information
  - format_raw_tombstone: format a ParsedTombstone without symbols
  - read_minidump_annotations: read Crashpad SimpleStringDictionary from a minidump binary
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from .log_parser import ParsedFrame, ParsedThread, ParsedTombstone

_SEP = "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***"

# Signal name → signal number (POSIX, x86-64 Linux).
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


# ---------------------------------------------------------------------------
# rust-minidump JSON → ParsedTombstone
# ---------------------------------------------------------------------------


def parse_stackwalk_json(data: dict) -> tuple[ParsedTombstone, SymbolTable]:
    """Convert minidump-stackwalk --json output to (ParsedTombstone, SymbolTable).

    Raises ValueError if the JSON does not contain a threads array.
    """
    if "threads" not in data:
        raise ValueError("No threads array in minidump-stackwalk JSON output")

    result = ParsedTombstone()
    table: SymbolTable = {}

    result.pid = data.get("pid") or 0

    crash_info = data.get("crash_info", {})
    sig_name = crash_info.get("type") or ""
    result.signal_info = sig_name
    result.signal_number = _SIGNAL_NUMBERS.get(sig_name.split()[0], 0) if sig_name else 0
    addr_str = crash_info.get("address") or "0x0"
    try:
        result.fault_addr = int(addr_str, 16)
    except ValueError:
        result.fault_addr = 0
    crashing_thread_idx = crash_info.get("crashing_thread") or 0

    main_module_idx = data.get("main_module") or 0
    modules = data.get("modules") or []
    if main_module_idx < len(modules):
        result.process_name = Path(modules[main_module_idx].get("filename") or "").name

    for thread_idx, thread_data in enumerate(data["threads"]):
        tid = thread_data.get("thread_id") or thread_idx
        name = thread_data.get("thread_name") or ""
        thread = ParsedThread(
            tid=tid,
            name=name,
            is_crashing=(thread_idx == crashing_thread_idx),
        )
        for frame_data in thread_data.get("frames") or []:
            frame_idx = frame_data.get("frame", len(thread.frames))
            offset_str = frame_data.get("module_offset") or "0x0"
            try:
                offset = int(offset_str, 16)
            except ValueError:
                offset = 0
            frame = ParsedFrame(
                index=frame_idx,
                module_offset=offset,
                module_path=frame_data.get("module") or "",
                trust=frame_data.get("trust") or "",
            )
            thread.frames.append(frame)
            func = frame_data.get("function")
            if func:
                sym = SymbolInfo(function=func)
                src_file = frame_data.get("file")
                src_line = frame_data.get("line")
                if src_file:
                    sym.source_file = src_file
                if src_line is not None:
                    sym.source_line = int(src_line)
                table[(tid, frame_idx)] = sym
        result.threads.append(thread)

    if not result.threads:
        raise ValueError("No thread frames found in minidump-stackwalk JSON output")

    # Move crashing thread to front.
    if crashing_thread_idx < len(result.threads):
        crashing = result.threads.pop(crashing_thread_idx)
        result.threads.insert(0, crashing)
        result.crashing_tid = crashing.tid

    return result, table


# ---------------------------------------------------------------------------
# Tombstone formatter
# ---------------------------------------------------------------------------


def _emit_stack_trace_section(
    thread: ParsedThread,
    symbols: SymbolTable,
    out: list[str],
    addr_w: int,
) -> None:
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

        heuristic = " [HEURISTIC]" if frame.trust == "scan" else ""
        if trailing:
            out.append(f"    #{frame.index:02d} pc {addr}  {module}  {trailing}{heuristic}\n")
        else:
            out.append(f"    #{frame.index:02d} pc {addr}  {module}{heuristic}\n")


def _tombstone_addr_width(tombstone: ParsedTombstone) -> int:
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

    if tombstone.abort_message:
        if tombstone.terminate_type:
            out.append(f"Abort message: '{tombstone.terminate_type}: {tombstone.abort_message}'\n")
        else:
            out.append(f"Abort message: '{tombstone.abort_message}'\n")

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


def format_raw_tombstone(tombstone: ParsedTombstone) -> str:
    """Format a ParsedTombstone without symbol information."""
    return format_symbolicated(tombstone, {})


# ---------------------------------------------------------------------------
# Crashpad simple-annotations reader
# ---------------------------------------------------------------------------

_MINIDUMP_MAGIC = b"MDMP"
_CRASHPAD_INFO_STREAM_TYPE = 0x43500007


def _read_utf8_string(data: bytes, rva: int) -> str:
    """Read a MINIDUMP_UTF8_STRING at *rva*: length(u32le) + UTF-8 bytes."""
    if rva == 0 or rva + 4 > len(data):
        return ""
    length = struct.unpack_from("<I", data, rva)[0]
    if length == 0 or rva + 4 + length > len(data):
        return ""
    return data[rva + 4 : rva + 4 + length].decode("utf-8", errors="replace")


def read_minidump_annotations(dmp_path: str) -> dict[str, str]:
    """Return the Crashpad SimpleStringDictionary from a minidump as a plain dict.

    Parses the CrashpadInfo stream (type 0x43500007) directly from the file
    binary. Returns an empty dict on any parse error or when the stream is absent.
    """
    try:
        with open(dmp_path, "rb") as fobj:
            data = fobj.read()
    except OSError:
        return {}

    if len(data) < 32 or data[:4] != _MINIDUMP_MAGIC:
        return {}

    try:
        _, _, stream_count, dir_rva = struct.unpack_from("<IIII", data, 0)

        stream_rva = 0
        for i in range(stream_count):
            off = dir_rva + i * 12
            stype, _size, srva = struct.unpack_from("<III", data, off)
            if stype == _CRASHPAD_INFO_STREAM_TYPE:
                stream_rva = srva
                break

        if stream_rva == 0:
            return {}

        # MinidumpCrashpadInfo:
        #   version(4) | report_id(16) | client_id(16) |
        #   annotations_DataSize(4) | annotations_RVA(4) | ...
        if stream_rva + 44 > len(data):
            return {}
        version = struct.unpack_from("<I", data, stream_rva)[0]
        if version != 1:
            return {}
        dict_size, dict_rva = struct.unpack_from("<II", data, stream_rva + 36)
        if dict_rva == 0 or dict_size < 4:
            return {}

        count = struct.unpack_from("<I", data, dict_rva)[0]
        result: dict[str, str] = {}
        for i in range(count):
            entry_off = dict_rva + 4 + i * 8
            if entry_off + 8 > len(data):
                break
            key_rva, val_rva = struct.unpack_from("<II", data, entry_off)
            result[_read_utf8_string(data, key_rva)] = _read_utf8_string(data, val_rva)

        return result
    except struct.error:
        return {}
