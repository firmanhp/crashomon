"""Tombstone data model — structured types produced by parsing minidump_stackwalk output."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class ParsedFrame:
    index: int = 0
    module_offset: int = 0
    module_path: str = ""
    trailing: str = ""  # already-symbolicated text preserved verbatim
    build_id: str = ""  # GNU build ID hex (lowercase), from (BuildId: ...) suffix
    trust: str = ""  # frame recovery method: "context", "cfi", "frame_pointer", "scan", ""


@dataclass
class ParsedThread:
    tid: int = 0
    name: str = ""
    is_crashing: bool = False
    frames: list[ParsedFrame] = field(default_factory=list)


@dataclass
class ParsedTombstone:
    pid: int = 0
    crashing_tid: int = 0
    process_name: str = ""
    signal_info: str = ""  # e.g. "SIGSEGV / SEGV_MAPERR"
    signal_number: int = 0
    signal_code: int = 0
    fault_addr: int = 0
    timestamp: str = ""
    threads: list[ParsedThread] = field(default_factory=list)
    minidump_path: str = ""
    abort_message: str = ""
    terminate_type: str = ""
