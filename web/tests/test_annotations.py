"""Tests for Crashpad annotation extraction and tombstone formatting."""

from __future__ import annotations

import struct
from pathlib import Path

import pytest
from crashomon_tools.analyze.log_parser import ParsedFrame, ParsedThread, ParsedTombstone
from crashomon_tools.analyze.symbolizer import format_symbolicated, read_minidump_annotations

# ---------------------------------------------------------------------------
# read_minidump_annotations — binary reader for CrashpadInfo stream
# ---------------------------------------------------------------------------


def _utf8_str(s: str) -> bytes:
    """Serialize a MINIDUMP_UTF8_STRING: length(u32le) + UTF-8 bytes + null."""
    encoded = s.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded + b"\x00"


def _build_annotated_minidump(abort_msg: str, term_type: str = "") -> bytes:
    """Build a minimal valid MDMP binary with a CrashpadInfo stream."""
    mdmp_signature = 0x504D444D
    mdmp_version = 0xA793
    crashpad_stream_type = 0x43500007

    header_size = 32
    dir_entry_size = 12
    crashpad_header_size = 52

    num_entries = 1 if not term_type else 2
    dict_rva = header_size + dir_entry_size + crashpad_header_size
    dict_bytes = 4 + num_entries * 8
    strings_rva = dict_rva + dict_bytes

    def str_total(s: str) -> int:
        return 4 + len(s.encode("utf-8")) + 1

    key0 = "abort_message"
    val0 = abort_msg
    key0_rva = strings_rva
    val0_rva = key0_rva + str_total(key0)

    key1 = "terminate_type"
    key1_rva = val0_rva + str_total(val0)
    val1_rva = key1_rva + str_total(key1)

    total_strings = str_total(key0) + str_total(val0)
    if term_type:
        total_strings += str_total(key1) + str_total(term_type)
    stream_size = crashpad_header_size + dict_bytes + total_strings

    stream_rva = header_size + dir_entry_size

    buf = bytearray()
    buf += struct.pack("<I", mdmp_signature)
    buf += struct.pack("<I", mdmp_version)
    buf += struct.pack("<I", 1)
    buf += struct.pack("<I", header_size)
    buf += struct.pack("<I", 0)
    buf += struct.pack("<I", 0)
    buf += b"\x00" * 8

    buf += struct.pack("<I", crashpad_stream_type)
    buf += struct.pack("<I", stream_size)
    buf += struct.pack("<I", stream_rva)

    buf += struct.pack("<I", 1)
    buf += b"\x00" * 16
    buf += b"\x00" * 16
    buf += struct.pack("<I", dict_bytes)
    buf += struct.pack("<I", dict_rva)
    buf += struct.pack("<I", 0)
    buf += struct.pack("<I", 0)

    buf += struct.pack("<I", num_entries)
    buf += struct.pack("<II", key0_rva, val0_rva)
    if term_type:
        buf += struct.pack("<II", key1_rva, val1_rva)

    buf += _utf8_str(key0)
    buf += _utf8_str(val0)
    if term_type:
        buf += _utf8_str(key1)
        buf += _utf8_str(term_type)

    return bytes(buf)


@pytest.fixture
def abort_only_dmp(tmp_path: Path) -> Path:
    p = tmp_path / "abort_only.dmp"
    p.write_bytes(_build_annotated_minidump("assertion failed: 'x > 0' (main.cpp:7, run())"))
    return p


@pytest.fixture
def abort_and_type_dmp(tmp_path: Path) -> Path:
    p = tmp_path / "abort_and_type.dmp"
    p.write_bytes(_build_annotated_minidump("unhandled C++ exception", "std::logic_error"))
    return p


def test_read_annotations_abort_message_only(abort_only_dmp: Path) -> None:
    result = read_minidump_annotations(str(abort_only_dmp))
    assert result.get("abort_message") == "assertion failed: 'x > 0' (main.cpp:7, run())"
    assert "terminate_type" not in result


def test_read_annotations_both_keys(abort_and_type_dmp: Path) -> None:
    result = read_minidump_annotations(str(abort_and_type_dmp))
    assert result.get("abort_message") == "unhandled C++ exception"
    assert result.get("terminate_type") == "std::logic_error"


def test_read_annotations_nonexistent_file_returns_empty() -> None:
    result = read_minidump_annotations("/nonexistent/path/no.dmp")
    assert result == {}


def test_read_annotations_garbage_file_returns_empty(tmp_path: Path) -> None:
    p = tmp_path / "garbage.dmp"
    p.write_bytes(b"not a minidump at all")
    result = read_minidump_annotations(str(p))
    assert result == {}


# ---------------------------------------------------------------------------
# format_symbolicated with abort_message
# ---------------------------------------------------------------------------


def _make_tombstone(abort_msg: str = "", term_type: str = "") -> ParsedTombstone:
    t = ParsedTombstone(
        pid=1,
        crashing_tid=1,
        process_name="test_proc",
        signal_info="SIGABRT",
        signal_number=6,
        fault_addr=0,
        timestamp="2026-04-20T00:00:00Z",
        abort_message=abort_msg,
        terminate_type=term_type,
    )
    thread = ParsedThread(tid=1, is_crashing=True)
    thread.frames = [ParsedFrame(index=0, module_offset=0x1000, module_path="test_proc")]
    t.threads = [thread]
    return t


def test_format_symbolicated_emits_abort_message_line() -> None:
    out = format_symbolicated(
        _make_tombstone(abort_msg="assertion failed: 'x > 0' (f.cpp:1, g())"), {}
    )
    assert "Abort message: 'assertion failed: 'x > 0' (f.cpp:1, g())'" in out


def test_format_symbolicated_combines_terminate_type_and_message() -> None:
    out = format_symbolicated(
        _make_tombstone(abort_msg="unhandled C++ exception", term_type="std::runtime_error"),
        {},
    )
    assert "Abort message: 'std::runtime_error: unhandled C++ exception'" in out


def test_format_symbolicated_no_abort_message_no_line() -> None:
    out = format_symbolicated(_make_tombstone(), {})
    assert "Abort message:" not in out


def test_format_symbolicated_abort_message_after_signal_before_timestamp() -> None:
    out = format_symbolicated(_make_tombstone(abort_msg="test msg"), {})
    sig_pos = out.find("signal 6")
    abort_pos = out.find("Abort message:")
    ts_pos = out.find("timestamp:")
    assert sig_pos != -1
    assert abort_pos != -1
    assert ts_pos != -1
    assert abort_pos > sig_pos
    assert abort_pos < ts_pos
