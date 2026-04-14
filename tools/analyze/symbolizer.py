"""Symbolization and tombstone formatting utilities.

Provides:
  - symbolize_with_addr2line: resolve frame addresses via eu-addr2line
  - build_build_id_index: index ELF debug binaries by GNU build ID (pure Python)
  - symbolize_with_build_id_index: resolve frames via build-ID-indexed ELFs
  - format_symbolicated: format a ParsedTombstone with symbol information
  - parse_stackwalk_machine: parse minidump_stackwalk -m output
  - format_raw_tombstone: format a ParsedTombstone without symbols (Mode 4)
"""

from __future__ import annotations

import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from .log_parser import ParsedFrame, ParsedThread, ParsedTombstone

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


def _frame_func(frame: ParsedFrame, sym: SymbolInfo | None) -> str:
    """Return the best available function string for *frame*."""
    if sym and sym.function not in ("??", ""):
        return sym.function
    if frame.trailing:
        return frame.trailing
    if frame.module_path:
        return frame.module_path
    return "??"


def _emit_stack_trace_section(
    thread: ParsedThread,
    symbols: SymbolTable,
    out: list[str],
    addr_w: int,
) -> None:
    """Emit one Android-style 'Stack Trace:' section for *thread*."""
    rows: list[tuple[str, str, str]] = []
    for frame in thread.frames:
        reladdr = f"{frame.module_offset:0{addr_w}x}"
        func_str = _frame_func(frame, symbols.get((thread.tid, frame.index)))
        sym = symbols.get((thread.tid, frame.index))
        file_line = ""
        if sym and sym.source_file and sym.source_line > 0:
            file_line = f"{sym.source_file}:{sym.source_line}"
        rows.append((reladdr, func_str, file_line))

    if not rows:
        return

    func_w = max(len(r[1]) for r in rows)
    func_w = max(func_w, len("FUNCTION"))

    out.append("Stack Trace:\n")
    out.append(f"  {'RELADDR':<{addr_w}}  {'FUNCTION':<{func_w}}  FILE:LINE\n")
    for reladdr, func_str, file_line in rows:
        if file_line:
            out.append(f"  {reladdr}  {func_str:<{func_w}}  {file_line}\n")
        else:
            out.append(f"  {reladdr}  {func_str}\n")


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
        out.append("\n")
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


# ---------------------------------------------------------------------------
# Build-ID–based symbolization (Mode 5 — --debug-dir)
# ---------------------------------------------------------------------------

_PT_NOTE = 4
_NT_GNU_BUILD_ID = 3
_GNU_NOTE_NAME = b"GNU\x00"


def _extract_build_id(data: bytes) -> str:
    """Extract the GNU build ID from raw ELF bytes.

    Parses PT_NOTE program headers (or falls back to nothing) looking for a
    note with name "GNU\\0" and type NT_GNU_BUILD_ID (3).  Returns the
    descriptor bytes as a lowercase hex string, or "" if not found.
    """
    if len(data) < 16 or data[:4] != b"\x7fELF":
        return ""

    ei_class = data[4]  # 1 = 32-bit, 2 = 64-bit
    ei_data = data[5]   # 1 = LE, 2 = BE
    endian = "<" if ei_data == 1 else ">"

    try:
        if ei_class == 2:  # 64-bit ELF
            if len(data) < 64:
                return ""
            (ph_off,) = struct.unpack_from(f"{endian}Q", data, 32)
            ph_ent_size, ph_num = struct.unpack_from(f"{endian}HH", data, 54)
        elif ei_class == 1:  # 32-bit ELF
            if len(data) < 52:
                return ""
            (ph_off,) = struct.unpack_from(f"{endian}I", data, 28)
            ph_ent_size, ph_num = struct.unpack_from(f"{endian}HH", data, 42)
        else:
            return ""

        for i in range(ph_num):
            ph_start = ph_off + i * ph_ent_size
            if ph_start + ph_ent_size > len(data):
                break

            if ei_class == 2:
                (p_type,) = struct.unpack_from(f"{endian}I", data, ph_start)
                if p_type != _PT_NOTE:
                    continue
                p_offset, p_filesz = struct.unpack_from(f"{endian}QQ", data, ph_start + 8)
            else:
                p_type, p_offset, _vaddr, _paddr, p_filesz = struct.unpack_from(
                    f"{endian}IIIII", data, ph_start
                )
                if p_type != _PT_NOTE:
                    continue

            # Walk notes in this PT_NOTE segment.
            note_pos = p_offset
            note_end = p_offset + p_filesz
            while note_pos + 12 <= note_end and note_pos + 12 <= len(data):
                namesz, descsz, n_type = struct.unpack_from(f"{endian}III", data, note_pos)
                note_pos += 12
                name_padded = (namesz + 3) & ~3
                desc_padded = (descsz + 3) & ~3
                if note_pos + name_padded + desc_padded > len(data):
                    break
                name = data[note_pos : note_pos + namesz]
                note_pos += name_padded
                desc = data[note_pos : note_pos + descsz]
                note_pos += desc_padded
                if n_type == _NT_GNU_BUILD_ID and name == _GNU_NOTE_NAME:
                    return desc.hex()
    except struct.error:
        return ""

    return ""


def build_build_id_index(debug_dir: Path) -> dict[str, Path]:
    """Walk *debug_dir* recursively and index ELF files by GNU build ID.

    Returns ``{build_id_hex_lower: elf_path}``.  On duplicate build IDs the
    first path wins and a warning is printed to stderr.  Non-ELF files and
    files that cannot be read are silently skipped.
    """
    index: dict[str, Path] = {}
    for path in debug_dir.rglob("*"):
        if not path.is_file():
            continue
        try:
            with path.open("rb") as fobj:
                magic = fobj.read(4)
                if magic != b"\x7fELF":
                    continue
                fobj.seek(0)
                data = fobj.read()
        except OSError:
            continue
        build_id = _extract_build_id(data)
        if not build_id:
            continue
        if build_id in index:
            print(
                f"warning: duplicate build ID {build_id}: {index[build_id]} and {path}",
                file=sys.stderr,
            )
            continue
        index[build_id] = path
    return index


def symbolize_with_build_id_index(
    tombstone: ParsedTombstone,
    index: dict[str, Path],
    addr2line: str = "eu-addr2line",
) -> SymbolTable:
    """Resolve frame addresses using a build-ID index of host-side debug ELFs.

    Frames whose ``build_id`` is empty or not found in *index* map to
    ``SymbolInfo(function="??")`` — the formatter renders these as unsymbolicated.
    Never raises; missing binaries or addr2line errors result in unknown symbols.
    """
    # Group frames by the resolved local ELF path.
    by_elf: dict[Path, list[tuple[int, int, int]]] = {}
    for thread in tombstone.threads:
        for frame in thread.frames:
            if not frame.build_id:
                continue
            elf_path = index.get(frame.build_id)
            if elf_path is None:
                continue
            by_elf.setdefault(elf_path, []).append(
                (thread.tid, frame.index, frame.module_offset)
            )

    table: SymbolTable = {}
    for elf_path, refs in by_elf.items():
        lines = _query_addr2line(str(elf_path), addr2line, refs)
        for i, (tid, idx, _) in enumerate(refs):
            base = i * 2
            func = lines[base] if base < len(lines) else "??"
            loc = lines[base + 1] if base + 1 < len(lines) else ""
            table[(tid, idx)] = _parse_addr2line_output(func, loc)

    return table
