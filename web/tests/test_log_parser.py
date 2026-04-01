"""Unit tests for web.log_parser — port of test/test_log_parser.cpp."""

import pytest

from web.log_parser import parse_tombstone

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

MINIMAL_TOMBSTONE = (
    "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
    "pid: 1234, tid: 1234, name: my_service  >>> my_service <<<\n"
    "signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0000000000000000\n"
    "\n"
    "backtrace:\n"
    "    #00 pc 0x0000000000001abc  /usr/bin/my_service\n"
    "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
)

FULL_TOMBSTONE = (
    "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
    "pid: 42, tid: 100, name: worker  >>> worker <<<\n"
    "signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x00000000deadbeef\n"
    "timestamp: 2026-03-27T10:15:30Z\n"
    "\n"
    "backtrace:\n"
    "    #00 pc 0x0000000000004000  /usr/lib/libfoo.so\n"
    "    #01 pc 0x0000000000008000  /usr/bin/worker\n"
    "\n"
    "--- --- --- thread 101 --- --- ---\n"
    "    #00 pc 0x0000000000001000  /usr/lib/libpthread.so\n"
    "\n"
    "--- --- --- thread 102 (io_thread) --- --- ---\n"
    "    #00 pc 0x0000000000002000  /usr/lib/libc.so\n"
    "    #01 pc 0x0000000000003000  ???\n"
    "\n"
    "minidump saved to: /var/crashomon/abc123.dmp\n"
    "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
)

# ---------------------------------------------------------------------------
# Basic parsing
# ---------------------------------------------------------------------------


def test_empty_input_raises():
    with pytest.raises(ValueError):
        parse_tombstone("")


def test_no_frames_raises():
    text = (
        "pid: 1, tid: 1, name: foo  >>> foo <<<\n"
        "signal 11 (SIGSEGV), fault addr 0x0\n"
    )
    with pytest.raises(ValueError):
        parse_tombstone(text)


def test_minimal_succeeds():
    result = parse_tombstone(MINIMAL_TOMBSTONE)
    assert result is not None


# ---------------------------------------------------------------------------
# Header fields
# ---------------------------------------------------------------------------


def test_parses_pid():
    result = parse_tombstone(MINIMAL_TOMBSTONE)
    assert result.pid == 1234


def test_parses_crashing_tid():
    result = parse_tombstone(MINIMAL_TOMBSTONE)
    assert result.crashing_tid == 1234


def test_parses_process_name():
    result = parse_tombstone(MINIMAL_TOMBSTONE)
    assert result.process_name == "my_service"


# ---------------------------------------------------------------------------
# Signal line
# ---------------------------------------------------------------------------


def test_parses_signal_number():
    result = parse_tombstone(MINIMAL_TOMBSTONE)
    assert result.signal_number == 11


def test_parses_signal_with_code():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert result.signal_info == "SIGSEGV / SEGV_MAPERR"
    assert result.signal_code == 1


def test_parses_fault_addr():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert result.fault_addr == 0xDEADBEEF


def test_fault_addr_zero_is_valid():
    result = parse_tombstone(MINIMAL_TOMBSTONE)
    assert result.fault_addr == 0


# ---------------------------------------------------------------------------
# Timestamp
# ---------------------------------------------------------------------------


def test_timestamp_absent_is_empty():
    result = parse_tombstone(MINIMAL_TOMBSTONE)
    assert result.timestamp == ""


def test_timestamp_parsed():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert result.timestamp == "2026-03-27T10:15:30Z"


# ---------------------------------------------------------------------------
# Threads and frames
# ---------------------------------------------------------------------------


def test_crashing_thread_is_first():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert result.threads
    assert result.threads[0].is_crashing


def test_crashing_thread_frame_count():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert len(result.threads[0].frames) == 2


def test_other_thread_count():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert len(result.threads) == 3
    assert not result.threads[1].is_crashing
    assert not result.threads[2].is_crashing


def test_thread_tids_parsed():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert result.threads[0].tid == 100
    assert result.threads[1].tid == 101
    assert result.threads[2].tid == 102


def test_thread_name_parsed():
    result = parse_tombstone(FULL_TOMBSTONE)
    # thread 101 has no name; thread 102 is "io_thread"
    assert result.threads[1].name == ""
    assert result.threads[2].name == "io_thread"


# ---------------------------------------------------------------------------
# Frame fields
# ---------------------------------------------------------------------------


def test_frame_index():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert result.threads[0].frames[0].index == 0
    assert result.threads[0].frames[1].index == 1


def test_frame_module_offset():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert result.threads[0].frames[0].module_offset == 0x4000
    assert result.threads[0].frames[1].module_offset == 0x8000


def test_frame_module_path():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert result.threads[0].frames[0].module_path == "/usr/lib/libfoo.so"


def test_unmapped_frame_has_empty_path():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert len(result.threads[2].frames) == 2
    assert result.threads[2].frames[1].module_path == ""


# ---------------------------------------------------------------------------
# Trailing / already-symbolicated text
# ---------------------------------------------------------------------------


def test_trailing_text_preserved():
    text = (
        "pid: 1, tid: 1, name: foo  >>> foo <<<\n"
        "signal 6 (SIGABRT), fault addr 0x0\n"
        "\n"
        "backtrace:\n"
        "    #00 pc 0x0000000000001abc  /usr/bin/foo (bar_func) [bar.cpp:42]\n"
        "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
    )
    result = parse_tombstone(text)
    assert len(result.threads[0].frames) == 1
    assert result.threads[0].frames[0].trailing == "(bar_func) [bar.cpp:42]"


# ---------------------------------------------------------------------------
# Minidump path
# ---------------------------------------------------------------------------


def test_minidump_path_parsed():
    result = parse_tombstone(FULL_TOMBSTONE)
    assert result.minidump_path == "/var/crashomon/abc123.dmp"


def test_minidump_path_absent_is_empty():
    result = parse_tombstone(MINIMAL_TOMBSTONE)
    assert result.minidump_path == ""


# ---------------------------------------------------------------------------
# Edge cases
# ---------------------------------------------------------------------------


def test_signal_without_code():
    text = (
        "pid: 10, tid: 10, name: app  >>> app <<<\n"
        "signal 6 (SIGABRT), fault addr 0x0000000000000000\n"
        "\n"
        "backtrace:\n"
        "    #00 pc 0x0000000000000100  /usr/bin/app\n"
    )
    result = parse_tombstone(text)
    assert result.signal_number == 6
    assert result.signal_info == "SIGABRT"
    assert result.signal_code == 0


def test_thread_with_no_frames_not_saved():
    text = (
        "pid: 5, tid: 5, name: prog  >>> prog <<<\n"
        "signal 11 (SIGSEGV), fault addr 0x0\n"
        "\n"
        "backtrace:\n"
        "    #00 pc 0x0000000000001000  /usr/bin/prog\n"
        "\n"
        "--- --- --- thread 6 --- --- ---\n"
        "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
    )
    result = parse_tombstone(text)
    # Thread 6 has no frames — at minimum the crashing thread must be present.
    assert len(result.threads) >= 1
    assert result.threads[0].is_crashing
