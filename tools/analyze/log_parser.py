"""Tombstone log parser — parse Android-style crash tombstone text.

Converts the text output produced by crashomon-watcherd (and by
crashomon-analyze --store ... --stdin) back into structured data.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field


@dataclass
class ParsedFrame:
    index: int = 0
    module_offset: int = 0
    module_path: str = ""
    trailing: str = ""  # already-symbolicated text preserved verbatim
    build_id: str = ""  # GNU build ID hex (lowercase), from (BuildId: ...) suffix


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
    signal_info: str = ""   # e.g. "SIGSEGV / SEGV_MAPERR"
    signal_number: int = 0
    signal_code: int = 0
    fault_addr: int = 0
    timestamp: str = ""
    threads: list[ParsedThread] = field(default_factory=list)
    minidump_path: str = ""


# ---------------------------------------------------------------------------
# Compiled patterns — same semantics as the C++ regexes in log_parser.cpp
# ---------------------------------------------------------------------------

# "pid: 1234, tid: 1234, name: my_service  >>> my_service <<<"
_HEADER_RE = re.compile(r"pid:\s*(\d+),\s*tid:\s*(\d+),\s*name:\s*(\S+)")

# "signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xdeadbeef"
# code group is optional (SIGABRT has no code in our format)
_SIGNAL_RE = re.compile(
    r"signal\s+(\d+)\s+\(([^)]+)\)"
    r"(?:,\s*code\s+(\d+)\s+\(([^)]+)\))?"
    r"[^0-9a-fx]*fault addr\s+(0x[0-9a-fA-F]+)"
)

# "timestamp: 2026-03-27T10:15:30Z"
_TIMESTAMP_RE = re.compile(r"timestamp:\s*(\S+)")

# "--- --- --- thread 1235 --- --- ---"
# or "--- --- --- thread 1235 (worker) --- --- ---"
_THREAD_RE = re.compile(
    r"---\s+---\s+---\s+thread\s+(\d+)(?:\s+\(([^)]+)\))?\s+---\s+---\s+---"
)

# "    #00 pc 0x00001a0  /usr/bin/my_service (symbol) [src:line]"
# groups: 1=index  2=hex_offset  3=module_path_or_???  4=trailing(optional)
_FRAME_RE = re.compile(
    r"^\s{2,}#(\d+)\s+pc\s+(0x[0-9a-fA-F]+)\s+(\S+)(?:\s+(.+))?$"
)

# "(BuildId: <hex>)" — detected in the trailing group to extract build IDs.
_BUILD_ID_TRAILING_RE = re.compile(r"^\(BuildId:\s*([0-9a-fA-F]+)\)$")

# "minidump saved to: /var/crashomon/xxx.dmp"
_MINIDUMP_PATH_RE = re.compile(r"minidump saved to:\s*(\S+)")

# Tombstone-specific token anchors used by _strip_log_prefix.  The first match
# position in a line determines where the tombstone content starts, allowing
# arbitrary log prefixes (journalctl, syslog, ISO-8601) to be stripped.
_LOG_PREFIX_TOKENS = re.compile(
    r"\*\*\* \*\*\* \*\*\*"
    r"|pid:"
    r"|signal "
    r"|timestamp:"
    r"|backtrace:"
    r"|--- --- ---"
    r"|\s*#\d+\s+pc "
    r"|minidump saved to:"
)


def _strip_log_prefix(line: str) -> str:
    """Strip any leading log prefix (journalctl, syslog, ISO-8601) from *line*.

    Scans for the first occurrence of a known tombstone token and returns the
    line starting at that position.  If no token is found, returns *line*
    unchanged so that blank lines and register-state noise pass through as-is.
    """
    m = _LOG_PREFIX_TOKENS.search(line)
    if m is None:
        return line
    return line[m.start():]


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def parse_tombstone(text: str) -> ParsedTombstone:
    """Parse Android-style tombstone text into a ParsedTombstone.

    Raises ValueError on empty input or if no thread frames are found.
    """
    if not text:
        raise ValueError("Empty input")

    result = ParsedTombstone()
    working = ParsedThread(is_crashing=True)
    in_backtrace = False
    in_other_thread = False

    for raw_line in text.splitlines():
        line = _strip_log_prefix(raw_line)

        # Header
        m = _HEADER_RE.search(line)
        if m:
            result.pid = int(m.group(1))
            result.crashing_tid = int(m.group(2))
            result.process_name = m.group(3)
            working.tid = result.crashing_tid
            continue

        # Signal
        m = _SIGNAL_RE.search(line)
        if m:
            result.signal_number = int(m.group(1))
            result.signal_info = m.group(2)
            if m.group(3) is not None:
                result.signal_code = int(m.group(3))
                result.signal_info += " / " + m.group(4)
            result.fault_addr = int(m.group(5), 16)
            continue

        # Timestamp
        m = _TIMESTAMP_RE.search(line)
        if m:
            result.timestamp = m.group(1)
            continue

        # "backtrace:" section start
        if "backtrace:" in line:
            in_backtrace = True
            in_other_thread = False
            continue

        # Thread separator
        m = _THREAD_RE.search(line)
        if m:
            if in_backtrace or in_other_thread:
                result.threads.append(working)
            working = ParsedThread(
                tid=int(m.group(1)),
                name=m.group(2) or "",
                is_crashing=False,
            )
            in_other_thread = True
            in_backtrace = False
            continue

        # Minidump path
        m = _MINIDUMP_PATH_RE.search(line)
        if m:
            result.minidump_path = m.group(1)
            if in_backtrace or in_other_thread:
                result.threads.append(working)
                working = ParsedThread()
                in_backtrace = False
                in_other_thread = False
            continue

        # Stack frame (only inside a backtrace or other-thread section)
        if not (in_backtrace or in_other_thread):
            continue
        m = _FRAME_RE.search(line)
        if m:
            mod = m.group(3)
            trailing = m.group(4) or ""
            build_id = ""
            if trailing:
                bm = _BUILD_ID_TRAILING_RE.match(trailing)
                if bm:
                    build_id = bm.group(1).lower()
                    trailing = ""
            working.frames.append(
                ParsedFrame(
                    index=int(m.group(1)),
                    module_offset=int(m.group(2), 16),
                    module_path="" if mod == "???" else mod,
                    trailing=trailing,
                    build_id=build_id,
                )
            )

    # Flush the last thread if it has frames (e.g. no minidump path line).
    if working.frames or (working.tid != 0 and in_other_thread):
        result.threads.append(working)

    if not result.threads:
        raise ValueError("No thread frames found — not a valid tombstone")

    return result
