"""Symbolization and tombstone formatting utilities.

Provides:
  - symbolize_with_store: resolve frame addresses via a Breakpad symbol store
  - format_symbolicated: format a ParsedTombstone with symbol information
  - parse_stackwalk_machine: parse minidump_stackwalk -m output
  - format_raw_tombstone: format a ParsedTombstone without symbols (Mode 4)
"""

from __future__ import annotations

import bisect
import re as _re
import struct
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


def parse_stackwalk_machine(
    text: str,
) -> tuple[ParsedTombstone, SymbolTable, dict[str, int]]:
    """Parse minidump_stackwalk -m pipe-delimited output into a ParsedTombstone.

    minidump_stackwalk -m produces lines like:
      OS|Linux|...
      CPU|amd64|...|4
      Crash|SIGSEGV|0xdeadbeef|0          (crash_reason|addr|crashing_thread_idx)
      Module|name|ver|debug_file|debug_id|base|max|main
      thread_num|frame_num|module|func|file|line|offset

    Returns (tombstone, symbols, module_bases) where module_bases maps
    module name to its load address.  Raises ValueError if no frame lines found.
    """
    result = ParsedTombstone()
    threads: dict[int, ParsedThread] = {}
    table: SymbolTable = {}
    module_bases: dict[str, int] = {}
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

        if parts[0] == "OS":
            continue

        # Module|name|ver|debug_file|debug_id|base|max|main
        if parts[0] == "Module" and len(parts) >= 7:
            mod_name = parts[1]
            try:
                module_bases[mod_name] = int(parts[5], 16)
            except ValueError:
                pass
            if len(parts) >= 8 and parts[7] == "1":
                result.process_name = mod_name
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
    return result, table, module_bases


def format_raw_tombstone(tombstone: ParsedTombstone) -> str:
    """Format a ParsedTombstone without symbol information."""
    return format_symbolicated(tombstone, {})


# ---------------------------------------------------------------------------
# Correct module offsets using human-readable minidump_stackwalk output
# ---------------------------------------------------------------------------

_HUMAN_THREAD_RE = _re.compile(r"^Thread (\d+)")
_HUMAN_FRAME_RE = _re.compile(r"^ {0,3}(\d+)\s{2,}")
_HUMAN_PC_RE = _re.compile(r"\bpc\s*=\s*(0x[0-9a-fA-F]+)")


def parse_human_frame_pcs(text: str) -> dict[tuple[int, int], int]:
    """Parse ``pc = 0xXXXX`` lines from minidump_stackwalk human output.

    Returns {(thread_idx, frame_idx): absolute_pc}.
    """
    result: dict[tuple[int, int], int] = {}
    thread_idx = -1
    frame_idx = -1

    for line in text.splitlines():
        m = _HUMAN_THREAD_RE.match(line)
        if m:
            thread_idx = int(m.group(1))
            frame_idx = -1
            continue

        if thread_idx < 0:
            continue

        m = _HUMAN_FRAME_RE.match(line)
        if m:
            frame_idx = int(m.group(1))
            continue

        if frame_idx < 0:
            continue

        # Register dump lines — extract pc value (only the first occurrence per frame).
        m = _HUMAN_PC_RE.search(line)
        if m and (thread_idx, frame_idx) not in result:
            result[(thread_idx, frame_idx)] = int(m.group(1), 16)

    return result


def fix_frame_module_offsets(
    tombstone: ParsedTombstone,
    module_bases: dict[str, int],
    frame_pcs: dict[tuple[int, int], int],
) -> None:
    """Replace function-relative offsets with module-relative offsets.

    parse_stackwalk_machine() stores the -m ``function_offset`` field in
    frame.module_offset.  This function replaces that with the true module
    offset: absolute_pc - module_load_base, using PC values extracted from
    the human-readable minidump_stackwalk output.
    """
    for thread in tombstone.threads:
        for frame in thread.frames:
            pc = frame_pcs.get((thread.tid, frame.index))
            if pc is None:
                continue
            base = module_bases.get(frame.module_path)
            if base is None:
                continue
            frame.module_offset = pc - base


# ---------------------------------------------------------------------------
# Process info (pid, crashing tid) from minidump binary
# ---------------------------------------------------------------------------

_MINIDUMP_MAGIC = b"MDMP"
_STREAM_MISC_INFO = 15  # MINIDUMP_MISC_INFO
_STREAM_EXCEPTION = 6  # MINIDUMP_EXCEPTION_STREAM
_STREAM_THREAD_LIST = 3  # MINIDUMP_THREAD_LIST
_STREAM_THREAD_NAMES = 24  # MINIDUMP_THREAD_NAMES_LIST (Crashpad extension)
_MINIDUMP_MISC1_PROCESS_ID = 0x1
_THREAD_STRUCT_SIZE = 48  # sizeof(MINIDUMP_THREAD)
_THREAD_NAME_ENTRY_SIZE = 12  # sizeof(MINIDUMP_THREAD_NAME): tid(4) + name_rva(8)


def read_minidump_process_info(dmp_path: str) -> tuple[int, int]:
    """Return (pid, crashing_tid) by parsing the minidump binary.

    crashing_tid is the OS-level thread ID of the faulting thread, taken
    from the ExceptionStream.  pid comes from MiscInfoStream when the
    MINIDUMP_MISC1_PROCESS_ID flag is set.  Returns (0, 0) on any error.
    """
    try:
        with open(dmp_path, "rb") as fobj:
            data = fobj.read()
    except OSError:
        return (0, 0)

    if len(data) < 32 or data[:4] != _MINIDUMP_MAGIC:
        return (0, 0)

    try:
        _, _, stream_count, dir_rva = struct.unpack_from("<IIII", data, 0)

        misc_rva: int | None = None
        exc_rva: int | None = None

        for i in range(stream_count):
            off = dir_rva + i * 12
            stype, _size, srva = struct.unpack_from("<III", data, off)
            if stype == _STREAM_MISC_INFO:
                misc_rva = srva
            elif stype == _STREAM_EXCEPTION:
                exc_rva = srva

        pid = 0
        if misc_rva is not None:
            _sz, flags1 = struct.unpack_from("<II", data, misc_rva)
            if flags1 & _MINIDUMP_MISC1_PROCESS_ID:
                pid = struct.unpack_from("<I", data, misc_rva + 8)[0]

        crashing_tid = 0
        if exc_rva is not None:
            # MINIDUMP_EXCEPTION_STREAM: ThreadId is the first ULONG32.
            crashing_tid = struct.unpack_from("<I", data, exc_rva)[0]

        return (pid, crashing_tid)
    except struct.error:
        return (0, 0)


def read_minidump_thread_names(dmp_path: str) -> dict[int, str]:
    """Return {thread_index: thread_name} by parsing the minidump binary.

    Combines ThreadListStream (index→OS tid) with ThreadNamesStream
    (OS tid→name).  Returns an empty dict on any parse error or if the
    streams are absent.
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

        thread_list_rva: int | None = None
        thread_names_rva: int | None = None

        for i in range(stream_count):
            off = dir_rva + i * 12
            stype, _size, srva = struct.unpack_from("<III", data, off)
            if stype == _STREAM_THREAD_LIST:
                thread_list_rva = srva
            elif stype == _STREAM_THREAD_NAMES:
                thread_names_rva = srva

        if thread_list_rva is None or thread_names_rva is None:
            return {}

        # ThreadListStream: count(4) + MINIDUMP_THREAD[count]
        # MINIDUMP_THREAD first field is ThreadId(4); stride is 48 bytes.
        tl_count = struct.unpack_from("<I", data, thread_list_rva)[0]
        index_to_tid: dict[int, int] = {}
        for i in range(tl_count):
            tid = struct.unpack_from(
                "<I", data, thread_list_rva + 4 + i * _THREAD_STRUCT_SIZE
            )[0]
            index_to_tid[i] = tid

        # ThreadNamesStream: count(4) + MINIDUMP_THREAD_NAME[count]
        # MINIDUMP_THREAD_NAME: tid(4) + name_rva(8, 64-bit RVA into file)
        # Name is MINIDUMP_STRING: byte_length(4) + UTF-16LE chars
        tn_count = struct.unpack_from("<I", data, thread_names_rva)[0]
        tid_to_name: dict[int, str] = {}
        for i in range(tn_count):
            off = thread_names_rva + 4 + i * _THREAD_NAME_ENTRY_SIZE
            tid, name_rva = struct.unpack_from("<IQ", data, off)
            byte_len = struct.unpack_from("<I", data, name_rva)[0]
            name = data[name_rva + 4 : name_rva + 4 + byte_len].decode(
                "utf-16-le", errors="replace"
            )
            if name:
                tid_to_name[tid] = name

        return {
            idx: tid_to_name[tid]
            for idx, tid in index_to_tid.items()
            if tid in tid_to_name
        }
    except struct.error:
        return {}
