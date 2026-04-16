"""Tests for tools.analyze mode functions and symbolizer.parse_stackwalk_machine."""

from __future__ import annotations

import os
import shutil
import textwrap
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from tools.analyze.analyze import (
    _extract_sysroot_symbols,
    mode_minidump_store,
    mode_raw_tombstone,
    mode_stdin_store,
    mode_sym_file_minidump,
)
from tools.analyze.symbolizer import format_raw_tombstone, parse_stackwalk_machine

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_MACHINE_OUTPUT = textwrap.dedent("""\
    OS|Linux|0.0.0 Linux #1 SMP
    CPU|amd64|family 6|4
    GPU|UNKNOWN|0
    Crash|SIGSEGV|0x0000000000000004|0
    Module|my_service||my_service|DEADBEEF0|0x400000|0x410000|1
    Module|libc.so||libc.so|AABBCCDD0|0x7f000000|0x7f100000|0
    0|0|my_service||||0x1000
    0|1|libc.so||||0x2000
    1|0|libc.so||||0x3000
""")


# ---------------------------------------------------------------------------
# parse_stackwalk_machine
# ---------------------------------------------------------------------------


def test_parse_machine_crash_signal():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    assert t.signal_info == "SIGSEGV"


def test_parse_machine_signal_number_derived_from_name():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    assert t.signal_number == 11  # SIGSEGV = 11


def test_parse_machine_signal_number_compound_crash_reason():
    # Breakpad formats crash_reason as "SIGSEGV /SEGV_MAPERR" (space-slash, no trailing space).
    output = _MACHINE_OUTPUT.replace("Crash|SIGSEGV|", "Crash|SIGSEGV /SEGV_MAPERR|")
    t, _, _ = parse_stackwalk_machine(output)
    assert t.signal_number == 11
    assert t.signal_info == "SIGSEGV /SEGV_MAPERR"


def test_parse_machine_fault_addr():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    assert t.fault_addr == 4


def test_parse_machine_crashing_thread_first():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    assert t.threads[0].is_crashing


def test_parse_machine_thread_count():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    assert len(t.threads) == 2


def test_parse_machine_frame_count():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    assert len(t.threads[0].frames) == 2
    assert len(t.threads[1].frames) == 1


def test_parse_machine_frame_module():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    assert t.threads[0].frames[0].module_path == "my_service"
    assert t.threads[0].frames[1].module_path == "libc.so"


def test_parse_machine_frame_offset():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    assert t.threads[0].frames[0].module_offset == 0x1000


def test_parse_machine_empty_raises():
    with pytest.raises(ValueError):
        parse_stackwalk_machine("")


def test_parse_machine_no_frames_raises():
    with pytest.raises(ValueError):
        parse_stackwalk_machine("OS|Linux|0.0.0\nCrash|SIGSEGV|0x0|0\n")


# ---------------------------------------------------------------------------
# format_raw_tombstone (smoke test — exercises format_symbolicated with no syms)
# ---------------------------------------------------------------------------


def test_format_raw_tombstone_contains_sep():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    out = format_raw_tombstone(t)
    assert "*** *** ***" in out


def test_format_raw_tombstone_contains_signal():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    out = format_raw_tombstone(t)
    assert "SIGSEGV" in out


def test_format_raw_tombstone_contains_frame():
    t, _, _ = parse_stackwalk_machine(_MACHINE_OUTPUT)
    out = format_raw_tombstone(t)
    assert "my_service" in out


# ---------------------------------------------------------------------------
# mode_minidump_store
# ---------------------------------------------------------------------------


def test_mode_minidump_store_passes_args():
    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(stdout=_MACHINE_OUTPUT, returncode=0)
        result = mode_minidump_store("/sym/store", "crash.dmp", "minidump_stackwalk")
    # First call is machine mode (-m), second is human mode (no -m).
    cmd = mock_run.call_args_list[0][0][0]
    assert cmd == ["minidump_stackwalk", "-m", "crash.dmp", "/sym/store"]
    assert "*** *** ***" in result


def test_mode_minidump_store_missing_binary_raises():
    with (
        patch("tools.analyze.analyze.subprocess.run", side_effect=FileNotFoundError),
        pytest.raises(RuntimeError, match="not found"),
    ):
        mode_minidump_store("/store", "crash.dmp", "no_such_binary")


def test_mode_minidump_store_empty_output_raises():
    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(stdout="", returncode=1)
        with pytest.raises(RuntimeError, match="no output"):
            mode_minidump_store("/store", "crash.dmp", "minidump_stackwalk")


# ---------------------------------------------------------------------------
# mode_stdin_store
# ---------------------------------------------------------------------------


_SIMPLE_TOMBSTONE = (
    "pid: 1, tid: 1, name: app  >>> app <<<\n"
    "signal 11 (SIGSEGV), fault addr 0x0\n"
    "\n"
    "backtrace:\n"
    "    #00 pc 0x0000000000001000  /nonexistent/app\n"
)


def test_mode_stdin_store_returns_formatted():
    # Module path /nonexistent/app won't exist so symbolization is skipped.
    result = mode_stdin_store(_SIMPLE_TOMBSTONE, "/store")
    assert "*** *** ***" in result
    assert "SIGSEGV" in result


def test_mode_stdin_store_invalid_input_raises():
    with pytest.raises(ValueError):
        mode_stdin_store("", "/store")


# ---------------------------------------------------------------------------
# mode_sym_file_minidump
# ---------------------------------------------------------------------------


_SYM_CONTENT = (
    "MODULE Linux x86_64 DEADBEEF0 my_service\n"
    "FILE 0 main.cpp\n"
    "FUNC 0 10 0 main\n"
)


def test_mode_sym_file_minidump_creates_temp_store(tmp_path):
    sym_file = tmp_path / "my_service.sym"
    sym_file.write_text(_SYM_CONTENT)

    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(stdout=_MACHINE_OUTPUT, returncode=0)
        result = mode_sym_file_minidump(str(sym_file), "crash.dmp", "minidump_stackwalk")

    assert "*** *** ***" in result
    # First call is machine mode (-m), second is human mode (no -m).
    cmd = mock_run.call_args_list[0][0][0]
    assert cmd[0] == "minidump_stackwalk"
    assert cmd[1] == "-m"
    assert cmd[2] == "crash.dmp"
    # store path is a temp dir that no longer exists (cleaned up)
    # Temp dir is cleaned up after the call — just verify the command structure.
    assert len(cmd) == 4


def test_mode_sym_file_minidump_missing_sym_raises(tmp_path):
    with pytest.raises(OSError):
        mode_sym_file_minidump(str(tmp_path / "missing.sym"), "crash.dmp", "sw")


# ---------------------------------------------------------------------------
# mode_raw_tombstone
# ---------------------------------------------------------------------------


def test_mode_raw_tombstone_uses_machine_flag():
    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(stdout=_MACHINE_OUTPUT, returncode=0)
        result = mode_raw_tombstone("crash.dmp", "minidump_stackwalk")

    # First call is machine mode (-m), second is human mode (no -m).
    cmd = mock_run.call_args_list[0][0][0]
    assert "-m" in cmd
    assert "crash.dmp" in cmd
    assert "*** *** ***" in result


def test_mode_raw_tombstone_missing_binary_raises():
    with (
        patch("tools.analyze.analyze.subprocess.run", side_effect=FileNotFoundError),
        pytest.raises(RuntimeError, match="not found"),
    ):
        mode_raw_tombstone("crash.dmp", "no_such_binary")


# ---------------------------------------------------------------------------
# _extract_sysroot_symbols
# ---------------------------------------------------------------------------


def test_extract_sysroot_symbols_populates_store(tmp_path: Path) -> None:
    """Verify _extract_sysroot_symbols finds a library and stores its .sym file."""
    # This test requires dump_syms and a real ELF — skip if not available.
    dump_syms = shutil.which("breakpad_dump_syms") or shutil.which("dump_syms")
    if dump_syms is None:
        pytest.skip("dump_syms not found")

    # Use the test binary itself as a stand-in for a sysroot library.
    # Create a mock sysroot layout: sysroot/usr/lib/<binary>
    sysroot = tmp_path / "sysroot"
    lib_dir = sysroot / "usr" / "lib"
    lib_dir.mkdir(parents=True)

    # Find any ELF with debug info in the build tree.
    build_dir = Path(os.environ.get("BUILD_DIR", "build"))
    candidates = list(build_dir.glob("examples/crashomon-example-segfault"))
    if not candidates:
        pytest.skip("No test ELF available")

    shutil.copy2(candidates[0], lib_dir / "libfake.so.1")
    store = tmp_path / "store"

    _extract_sysroot_symbols(
        {"/usr/lib/libfake.so.1"},
        sysroot,
        store,
        dump_syms,
    )

    sym_files = list(store.rglob("*.sym"))
    assert len(sym_files) == 1
    assert sym_files[0].name == "libfake.so.1.sym"


def test_extract_sysroot_symbols_missing_module_is_silent(tmp_path: Path) -> None:
    """Modules not found in the sysroot print a message but do not raise."""
    sysroot = tmp_path / "sysroot"
    (sysroot / "usr" / "lib").mkdir(parents=True)
    store = tmp_path / "store"

    # Should complete without raising even though the module is absent.
    _extract_sysroot_symbols(
        {"/usr/lib/libnonexistent.so.1"},
        sysroot,
        store,
        "dump_syms",
    )

    assert not list(store.rglob("*.sym")) if store.exists() else True


def test_extract_sysroot_symbols_dump_syms_failure_is_silent(tmp_path: Path) -> None:
    """When dump_syms fails for a library, extraction continues silently."""
    sysroot = tmp_path / "sysroot"
    lib_dir = sysroot / "usr" / "lib"
    lib_dir.mkdir(parents=True)
    (lib_dir / "libfake.so.1").write_bytes(b"\x7fELF" + b"\x00" * 60)
    store = tmp_path / "store"

    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(returncode=1, stdout="", stderr="")
        _extract_sysroot_symbols(
            {"/usr/lib/libfake.so.1"},
            sysroot,
            store,
            "dump_syms",
        )

    assert not list(store.rglob("*.sym")) if store.exists() else True
