"""Tests for tools.analyze mode functions and symbolizer.parse_stackwalk_json."""

from __future__ import annotations

import json
import os
import shutil
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from tools.analyze.analyze import (
    _extract_sysroot_symbols,
    mode_minidump_store,
    mode_raw_tombstone,
    mode_sym_file_minidump,
)
from tools.analyze.log_parser import ParsedFrame, ParsedThread, ParsedTombstone
from tools.analyze.symbolizer import format_raw_tombstone, parse_stackwalk_json

# ---------------------------------------------------------------------------
# JSON fixture — matches real minidump-stackwalk --json output structure
# ---------------------------------------------------------------------------

_JSON_OUTPUT: dict = {
    "status": "OK",
    "pid": 1234,
    "main_module": 0,
    "crash_info": {
        "type": "SIGSEGV",
        "address": "0x0000000000000004",
        "crashing_thread": 0,
        "assertion": None,
    },
    "modules": [
        {
            "filename": "my_service",
            "base_addr": "0x0000000000400000",
            "end_addr": "0x0000000000410000",
            "debug_id": "DEADBEEF0",
            "loaded_symbols": False,
            "missing_symbols": False,
        },
        {
            "filename": "libc.so",
            "base_addr": "0x00007f0000000000",
            "end_addr": "0x00007f0000100000",
            "debug_id": "AABBCCDD0",
            "loaded_symbols": False,
            "missing_symbols": False,
        },
    ],
    "thread_count": 2,
    "threads": [
        {
            "thread_id": 100,
            "thread_name": None,
            "frames": [
                {
                    "frame": 0,
                    "module": "my_service",
                    "module_offset": "0x0000000000001000",
                    "offset": "0x0000000000401000",
                    "function": None,
                    "file": None,
                    "line": None,
                    "missing_symbols": True,
                    "trust": "context",
                },
                {
                    "frame": 1,
                    "module": "libc.so",
                    "module_offset": "0x0000000000002000",
                    "offset": "0x00007f0000002000",
                    "function": None,
                    "file": None,
                    "line": None,
                    "missing_symbols": True,
                    "trust": "scan",
                },
            ],
        },
        {
            "thread_id": 101,
            "thread_name": "worker",
            "frames": [
                {
                    "frame": 0,
                    "module": "libc.so",
                    "module_offset": "0x0000000000003000",
                    "offset": "0x00007f0000003000",
                    "function": None,
                    "file": None,
                    "line": None,
                    "missing_symbols": True,
                    "trust": "scan",
                },
            ],
        },
    ],
    "unloaded_modules": [],
}

_JSON_OUTPUT_WITH_SYMBOLS: dict = {
    **_JSON_OUTPUT,
    "threads": [
        {
            **_JSON_OUTPUT["threads"][0],
            "frames": [
                {
                    **_JSON_OUTPUT["threads"][0]["frames"][0],
                    "function": "main",
                    "file": "src/main.cpp",
                    "line": 42,
                    "missing_symbols": False,
                },
                _JSON_OUTPUT["threads"][0]["frames"][1],
            ],
        },
        _JSON_OUTPUT["threads"][1],
    ],
}

_JSON_STR = json.dumps(_JSON_OUTPUT)
_JSON_STR_WITH_SYMBOLS = json.dumps(_JSON_OUTPUT_WITH_SYMBOLS)

_SYM_CONTENT = "MODULE Linux x86_64 DEADBEEF0 my_service\nFILE 0 main.cpp\nFUNC 0 10 0 main\n"


# ---------------------------------------------------------------------------
# parse_stackwalk_json
# ---------------------------------------------------------------------------


def test_parse_json_pid():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.pid == 1234


def test_parse_json_signal_info():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.signal_info == "SIGSEGV"


def test_parse_json_signal_number():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.signal_number == 11


def test_parse_json_fault_addr():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.fault_addr == 4


def test_parse_json_process_name():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.process_name == "my_service"


def test_parse_json_crashing_thread_first():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.threads[0].is_crashing
    assert t.threads[0].tid == 100


def test_parse_json_crashing_tid_on_tombstone():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.crashing_tid == 100


def test_parse_json_thread_count():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert len(t.threads) == 2


def test_parse_json_frame_count():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert len(t.threads[0].frames) == 2
    assert len(t.threads[1].frames) == 1


def test_parse_json_frame_module():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.threads[0].frames[0].module_path == "my_service"
    assert t.threads[0].frames[1].module_path == "libc.so"


def test_parse_json_frame_offset():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.threads[0].frames[0].module_offset == 0x1000


def test_parse_json_thread_name():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.threads[1].name == "worker"


def test_parse_json_null_thread_name_becomes_empty():
    t, _ = parse_stackwalk_json(_JSON_OUTPUT)
    assert t.threads[0].name == ""


def test_parse_json_symbol_populated():
    t, syms = parse_stackwalk_json(_JSON_OUTPUT_WITH_SYMBOLS)
    assert (100, 0) in syms
    assert syms[(100, 0)].function == "main"


def test_parse_json_symbol_source_location():
    t, syms = parse_stackwalk_json(_JSON_OUTPUT_WITH_SYMBOLS)
    assert syms[(100, 0)].source_file == "src/main.cpp"
    assert syms[(100, 0)].source_line == 42


def test_parse_json_unsymbolicated_frame_not_in_table():
    t, syms = parse_stackwalk_json(_JSON_OUTPUT)
    assert (100, 0) not in syms


def test_parse_json_missing_threads_raises():
    with pytest.raises(ValueError):
        parse_stackwalk_json({})


def test_parse_json_empty_threads_raises():
    with pytest.raises(ValueError):
        parse_stackwalk_json({"threads": []})


# ---------------------------------------------------------------------------
# format_raw_tombstone (smoke test — exercises format_symbolicated with no syms)
# ---------------------------------------------------------------------------


def _make_simple_tombstone() -> ParsedTombstone:
    t = ParsedTombstone(
        pid=1,
        crashing_tid=1,
        process_name="my_service",
        signal_info="SIGSEGV",
        signal_number=11,
        fault_addr=4,
    )
    thread = ParsedThread(tid=1, is_crashing=True)
    thread.frames = [ParsedFrame(index=0, module_offset=0x1000, module_path="my_service")]
    t.threads = [thread]
    return t


def test_format_raw_tombstone_contains_sep():
    out = format_raw_tombstone(_make_simple_tombstone())
    assert "*** *** ***" in out


def test_format_raw_tombstone_contains_signal():
    out = format_raw_tombstone(_make_simple_tombstone())
    assert "SIGSEGV" in out


def test_format_raw_tombstone_contains_frame():
    out = format_raw_tombstone(_make_simple_tombstone())
    assert "my_service" in out


# ---------------------------------------------------------------------------
# mode_minidump_store
# ---------------------------------------------------------------------------


def test_mode_minidump_store_passes_args():
    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(stdout=_JSON_STR, returncode=0)
        result = mode_minidump_store("/sym/store", "crash.dmp", "minidump-stackwalk")
    assert mock_run.call_count == 1
    cmd = mock_run.call_args[0][0]
    assert cmd == ["minidump-stackwalk", "--json", "crash.dmp", "--symbols-path", "/sym/store"]
    assert "*** *** ***" in result


def test_mode_minidump_store_single_subprocess_call():
    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(stdout=_JSON_STR, returncode=0)
        mode_minidump_store("/store", "crash.dmp", "minidump-stackwalk")
    assert mock_run.call_count == 1


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
            mode_minidump_store("/store", "crash.dmp", "minidump-stackwalk")


def test_mode_minidump_store_invalid_json_raises():
    import json

    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(stdout="not json at all", returncode=0)
        with pytest.raises(json.JSONDecodeError):
            mode_minidump_store("/store", "crash.dmp", "minidump-stackwalk")


# ---------------------------------------------------------------------------
# mode_sym_file_minidump
# ---------------------------------------------------------------------------


def test_mode_sym_file_minidump_creates_temp_store(tmp_path):
    sym_file = tmp_path / "my_service.sym"
    sym_file.write_text(_SYM_CONTENT)

    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(stdout=_JSON_STR, returncode=0)
        result = mode_sym_file_minidump(str(sym_file), "crash.dmp", "minidump-stackwalk")

    assert mock_run.call_count == 1
    cmd = mock_run.call_args[0][0]
    assert cmd[0] == "minidump-stackwalk"
    assert cmd[1] == "--json"
    assert cmd[2] == "crash.dmp"
    assert cmd[3] == "--symbols-path"
    assert "*** *** ***" in result


def test_mode_sym_file_minidump_missing_sym_raises(tmp_path):
    with pytest.raises(OSError):
        mode_sym_file_minidump(str(tmp_path / "missing.sym"), "crash.dmp", "sw")


# ---------------------------------------------------------------------------
# mode_raw_tombstone
# ---------------------------------------------------------------------------


def test_mode_raw_tombstone_uses_json_flag():
    with patch("tools.analyze.analyze.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(stdout=_JSON_STR, returncode=0)
        result = mode_raw_tombstone("crash.dmp", "minidump-stackwalk")

    assert mock_run.call_count == 1
    cmd = mock_run.call_args[0][0]
    assert cmd == ["minidump-stackwalk", "--json", "crash.dmp"]
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
    dump_syms = shutil.which("breakpad_dump_syms") or shutil.which("dump_syms")
    if dump_syms is None:
        pytest.skip("dump_syms not found")

    sysroot = tmp_path / "sysroot"
    lib_dir = sysroot / "usr" / "lib"
    lib_dir.mkdir(parents=True)

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
    sysroot = tmp_path / "sysroot"
    (sysroot / "usr" / "lib").mkdir(parents=True)
    store = tmp_path / "store"

    _extract_sysroot_symbols(
        {"/usr/lib/libnonexistent.so.1"},
        sysroot,
        store,
        "dump_syms",
    )

    assert not list(store.rglob("*.sym")) if store.exists() else True


def test_extract_sysroot_symbols_dump_syms_failure_is_silent(tmp_path: Path) -> None:
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
