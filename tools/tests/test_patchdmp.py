"""Tests for crashomon_tools.patchdmp."""
from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

import pytest

from crashomon_tools.patchdmp import compute_elf_build_id


# ── Helpers ───────────────────────────────────────────────────────────────────


def _gcc(*args: str) -> None:
    subprocess.run(["gcc", *args], check=True, capture_output=True)


def _dump_syms_path() -> str:
    import shutil

    p = shutil.which("dump_syms")
    if p is not None:
        return p
    here = Path(__file__).resolve().parent
    for candidate in [
        here.parent / "syms" / "dump_syms",
        here.parent.parent / "_host_toolkit" / "dump_syms",
        here.parent.parent / "_dump_syms_build" / "dump_syms",
    ]:
        if candidate.exists():
            return str(candidate)
    pytest.skip("dump_syms not found")


@pytest.fixture
def tmp(tmp_path: Path) -> Path:
    return tmp_path


@pytest.fixture
def dump_syms() -> str:
    return _dump_syms_path()


def _raw_code_id_from_dump_syms(dump_syms_bin: str, elf: Path) -> str:
    """Return raw code ID from dump_syms INFO CODE_ID line (uppercase hex, no endian-swap).

    The MODULE line uses Breakpad's UUID endian-swap format (33 chars with age
    digit).  INFO CODE_ID contains the unmodified raw bytes (32 chars for the
    16-byte XOR fallback, 40 chars for a 20-byte SHA-1 GNU build ID).  This is
    what we must match to pass the right bytes into the BpEL CodeView record.
    """
    r = subprocess.run(
        [dump_syms_bin, str(elf)], capture_output=True, text=True, timeout=30
    )
    assert r.returncode == 0, r.stderr
    for line in r.stdout.splitlines():
        if line.startswith("INFO CODE_ID "):
            return line.split()[2].upper()
    pytest.fail(f"dump_syms output for {elf} has no INFO CODE_ID line")


# ── ELF build-ID tests ────────────────────────────────────────────────────────


def test_build_id_no_note_matches_dump_syms(tmp: Path, dump_syms: str) -> None:
    src = tmp / "libfoo.c"
    src.write_text("int foo(void) { int x = 1; return x + 1; }\n")
    lib = tmp / "libfoo.so"
    _gcc("-shared", "-fPIC", "-g", "-Wl,--build-id=none", str(src), "-o", str(lib))

    expected = _raw_code_id_from_dump_syms(dump_syms, lib)
    assert len(expected) == 32, f"expected 32-char XOR fallback, got {expected!r}"

    our = compute_elf_build_id(lib)
    assert our is not None
    assert our.hex().upper() == expected


def test_build_id_gnu_note_matches_dump_syms(tmp: Path, dump_syms: str) -> None:
    src = tmp / "libbar.c"
    src.write_text("int bar(void) { return 42; }\n")
    lib = tmp / "libbar.so"
    _gcc("-shared", "-fPIC", "-g", "-Wl,--build-id=sha1", str(src), "-o", str(lib))

    expected = _raw_code_id_from_dump_syms(dump_syms, lib)
    assert len(expected) == 40, f"expected 40-char SHA-1, got {expected!r}"

    our = compute_elf_build_id(lib)
    assert our is not None
    assert our.hex().upper() == expected


def test_build_id_non_elf_returns_none(tmp: Path) -> None:
    f = tmp / "notelf.bin"
    f.write_bytes(b"\x00\x01\x02\x03" * 32)
    assert compute_elf_build_id(f) is None


def test_build_id_missing_file_returns_none(tmp: Path) -> None:
    assert compute_elf_build_id(tmp / "nonexistent.so") is None


# ── Synthetic minidump helpers ────────────────────────────────────────────────

_PDB70_SIG = 0x53445352  # MD_CVINFOPDB70_SIGNATURE; "RSDS" in memory
_BPEL_SIG = 0x4270454C  # MD_CVINFOELF_SIGNATURE; "LEpB" in memory
_MODULE_LIST_STREAM = 4  # MD_MODULE_LIST_STREAM


def _pdb70_record(pdb_path: str = "") -> bytes:
    guid = b"\x00" * 16
    path_bytes = pdb_path.encode() + b"\x00"
    return struct.pack("<I16sI", _PDB70_SIG, guid, 0) + path_bytes


def _build_test_minidump(module_path: str, cv_bytes: bytes) -> bytearray:
    """Minimal valid minidump: 1 module with given CV record bytes."""
    name_utf16 = module_path.encode("utf-16-le")
    name_length = len(name_utf16)  # byte count, no null

    HEADER_SIZE = 32
    DIR_ENTRY_SIZE = 12
    MODULE_SIZE = 108

    stream_rva = HEADER_SIZE + DIR_ENTRY_SIZE  # 44
    name_rva = stream_rva + 4 + MODULE_SIZE  # 156
    cv_rva = name_rva + 4 + name_length + 2  # after UTF-16 + null term
    total = cv_rva + len(cv_bytes)
    stream_data_size = 4 + MODULE_SIZE

    buf = bytearray(total)

    # MINIDUMP_HEADER: Signature(I), Version(I), NumberOfStreams(I),
    #   StreamDirectoryRva(I), CheckSum(I), TimeDateStamp(I), Flags(Q)
    struct.pack_into("<IIIIIIQ", buf, 0, 0x504D444D, 0x0000A793, 1, HEADER_SIZE, 0, 0, 0)

    # MINIDUMP_DIRECTORY[0]: StreamType(I), DataSize(I), Rva(I)
    struct.pack_into("<III", buf, HEADER_SIZE, 4, stream_data_size, stream_rva)

    # Module list: count
    struct.pack_into("<I", buf, stream_rva, 1)

    # MDRawModule at stream_rva + 4 = 48
    mod_off = stream_rva + 4
    # BaseOfImage(Q), SizeOfImage(I), CheckSum(I), TimeDateStamp(I), ModuleNameRva(I)
    struct.pack_into("<QIIII", buf, mod_off, 0x7F000000, 0x10000, 0, 0, name_rva)
    # VersionInfo (52 bytes at mod_off+24) — zeros already.
    # CvRecord.DataSize at mod_off+76, CvRecord.Rva at mod_off+80
    struct.pack_into("<II", buf, mod_off + 76, len(cv_bytes), cv_rva)

    # Module name: length(4) + UTF-16LE + null(2)
    struct.pack_into("<I", buf, name_rva, name_length)
    buf[name_rva + 4 : name_rva + 4 + name_length] = name_utf16

    # CV record
    buf[cv_rva : cv_rva + len(cv_bytes)] = cv_bytes

    return buf


# Offsets for reading back fields from a test minidump built by _build_test_minidump:
# stream_rva=44, mod_off=48, cv_size at 48+76=124, cv_rva at 48+80=128
_MOD_CV_SIZE_OFF = 124
_MOD_CV_RVA_OFF = 128


# ── Patcher tests ─────────────────────────────────────────────────────────────


def test_patch_replaces_pdb70_with_bpel(tmp: Path) -> None:
    from crashomon_tools.patchdmp import patch_minidump

    src = tmp / "libp.c"
    src.write_text("int p(void) { int x = 1; return x + 1; }\n")
    lib = tmp / "libp.so"
    _gcc("-shared", "-fPIC", "-g", "-Wl,--build-id=none", str(src), "-o", str(lib))

    expected_bid = compute_elf_build_id(lib)
    assert expected_bid is not None

    cv = _pdb70_record(str(lib))
    data = _build_test_minidump(str(lib), cv)

    patched = patch_minidump(data)
    assert patched == 1

    cv_rva = struct.unpack_from("<I", data, _MOD_CV_RVA_OFF)[0]
    cv_size = struct.unpack_from("<I", data, _MOD_CV_SIZE_OFF)[0]
    assert cv_size == 4 + len(expected_bid)
    sig = struct.unpack_from("<I", data, cv_rva)[0]
    assert sig == _BPEL_SIG
    bid_in_file = bytes(data[cv_rva + 4 : cv_rva + 4 + len(expected_bid)])
    assert bid_in_file == expected_bid


def test_patch_skips_already_bpel(tmp: Path) -> None:
    from crashomon_tools.patchdmp import patch_minidump

    src = tmp / "libbpel.c"
    src.write_text("int x(void) { return 0; }\n")
    lib = tmp / "libbpel.so"
    _gcc("-shared", "-fPIC", "-g", "-Wl,--build-id=sha1", str(src), "-o", str(lib))

    fake_bid = bytes(range(20))
    bpel_cv = struct.pack("<I", _BPEL_SIG) + fake_bid
    data = _build_test_minidump(str(lib), bpel_cv)
    original = bytes(data)

    patched = patch_minidump(data)
    assert patched == 0
    assert bytes(data) == original


def test_patch_with_sysroot(tmp: Path) -> None:
    from crashomon_tools.patchdmp import patch_minidump

    sysroot = tmp / "sysroot"
    (sysroot / "usr" / "lib").mkdir(parents=True)
    src = tmp / "libsr.c"
    src.write_text("int sr(void) { int x = 2; return x + 1; }\n")
    lib = sysroot / "usr" / "lib" / "libsr.so"
    _gcc("-shared", "-fPIC", "-g", "-Wl,--build-id=none", str(src), "-o", str(lib))

    expected_bid = compute_elf_build_id(lib)
    assert expected_bid is not None

    cv = _pdb70_record("/usr/lib/libsr.so")
    data = _build_test_minidump("/usr/lib/libsr.so", cv)

    patched = patch_minidump(data, sysroot=sysroot)
    assert patched == 1

    cv_rva = struct.unpack_from("<I", data, _MOD_CV_RVA_OFF)[0]
    sig = struct.unpack_from("<I", data, cv_rva)[0]
    assert sig == _BPEL_SIG
    bid_in_file = bytes(data[cv_rva + 4 : cv_rva + 4 + len(expected_bid)])
    assert bid_in_file == expected_bid


def test_patch_missing_elf_skips(tmp: Path) -> None:
    from crashomon_tools.patchdmp import patch_minidump

    cv = _pdb70_record("/nonexistent/lib.so")
    data = _build_test_minidump("/nonexistent/lib.so", cv)
    patched = patch_minidump(data)
    assert patched == 0


# ── CLI tests ─────────────────────────────────────────────────────────────────


def _run_patchdmp(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-m", "crashomon_tools.patchdmp.cli", *args],
        capture_output=True,
        text=True,
    )


def test_cli_in_place(tmp: Path) -> None:
    src = tmp / "libcli.c"
    src.write_text("int cli(void) { int x = 3; return x + 1; }\n")
    lib = tmp / "libcli.so"
    _gcc("-shared", "-fPIC", "-g", "-Wl,--build-id=none", str(src), "-o", str(lib))

    cv = _pdb70_record(str(lib))
    dmp = tmp / "test.dmp"
    dmp.write_bytes(bytes(_build_test_minidump(str(lib), cv)))

    r = _run_patchdmp("--in-place", str(dmp))
    assert r.returncode == 0, r.stderr

    patched_data = dmp.read_bytes()
    cv_rva = struct.unpack_from("<I", patched_data, _MOD_CV_RVA_OFF)[0]
    sig = struct.unpack_from("<I", patched_data, cv_rva)[0]
    assert sig == _BPEL_SIG


def test_cli_output_file(tmp: Path) -> None:
    src = tmp / "libout.c"
    src.write_text("int out(void) { int x = 4; return x + 1; }\n")
    lib = tmp / "libout.so"
    _gcc("-shared", "-fPIC", "-g", "-Wl,--build-id=none", str(src), "-o", str(lib))

    cv = _pdb70_record(str(lib))
    dmp = tmp / "orig.dmp"
    dmp.write_bytes(bytes(_build_test_minidump(str(lib), cv)))
    out = tmp / "patched.dmp"

    r = _run_patchdmp("--output", str(out), str(dmp))
    assert r.returncode == 0, r.stderr
    assert out.exists()

    patched_data = out.read_bytes()
    cv_rva = struct.unpack_from("<I", patched_data, _MOD_CV_RVA_OFF)[0]
    sig = struct.unpack_from("<I", patched_data, cv_rva)[0]
    assert sig == _BPEL_SIG


def test_cli_dry_run_no_write(tmp: Path) -> None:
    src = tmp / "libdr.c"
    src.write_text("int dr(void) { int x = 5; return x + 1; }\n")
    lib = tmp / "libdr.so"
    _gcc("-shared", "-fPIC", "-g", "-Wl,--build-id=none", str(src), "-o", str(lib))

    cv = _pdb70_record(str(lib))
    dmp = tmp / "dryrun.dmp"
    original = bytes(_build_test_minidump(str(lib), cv))
    dmp.write_bytes(original)

    r = _run_patchdmp("--dry-run", str(dmp))
    assert r.returncode == 0, r.stderr
    assert dmp.read_bytes() == original  # file unchanged


# ── Override build-ID tests ───────────────────────────────────────────────────


def test_patch_with_build_id_override_by_basename(tmp: Path) -> None:
    from crashomon_tools.patchdmp import patch_minidump

    fake_bid = bytes(range(16))
    cv = _pdb70_record("/nonexistent/libover.so")
    data = _build_test_minidump("/nonexistent/libover.so", cv)

    patched = patch_minidump(data, build_id_overrides={"libover.so": fake_bid})
    assert patched == 1

    cv_rva = struct.unpack_from("<I", data, _MOD_CV_RVA_OFF)[0]
    sig = struct.unpack_from("<I", data, cv_rva)[0]
    assert sig == _BPEL_SIG
    assert bytes(data[cv_rva + 4 : cv_rva + 4 + 16]) == fake_bid


def test_patch_with_build_id_override_by_full_path(tmp: Path) -> None:
    from crashomon_tools.patchdmp import patch_minidump

    fake_bid = bytes(range(16, 32))
    cv = _pdb70_record("/nonexistent/libfull.so")
    data = _build_test_minidump("/nonexistent/libfull.so", cv)

    patched = patch_minidump(data, build_id_overrides={"/nonexistent/libfull.so": fake_bid})
    assert patched == 1

    cv_rva = struct.unpack_from("<I", data, _MOD_CV_RVA_OFF)[0]
    bid_in_file = bytes(data[cv_rva + 4 : cv_rva + 4 + 16])
    assert bid_in_file == fake_bid


def test_patch_override_takes_priority_over_elf(tmp: Path) -> None:
    from crashomon_tools.patchdmp import compute_elf_build_id, patch_minidump

    src = tmp / "libpri.c"
    src.write_text("int pri(void) { int x = 7; return x + 1; }\n")
    lib = tmp / "libpri.so"
    _gcc("-shared", "-fPIC", "-g", "-Wl,--build-id=none", str(src), "-o", str(lib))

    real_bid = compute_elf_build_id(lib)
    assert real_bid is not None

    fake_bid = bytes([0xAB] * 16)
    assert fake_bid != real_bid

    cv = _pdb70_record(str(lib))
    data = _build_test_minidump(str(lib), cv)

    patched = patch_minidump(data, build_id_overrides={"libpri.so": fake_bid})
    assert patched == 1

    cv_rva = struct.unpack_from("<I", data, _MOD_CV_RVA_OFF)[0]
    bid_in_file = bytes(data[cv_rva + 4 : cv_rva + 4 + 16])
    assert bid_in_file == fake_bid  # override wins, not the real ELF build ID


def test_cli_build_id_override(tmp: Path) -> None:
    fake_bid = bytes(range(16))
    cv = _pdb70_record("/nonexistent/libcliover.so")
    dmp = tmp / "over.dmp"
    dmp.write_bytes(bytes(_build_test_minidump("/nonexistent/libcliover.so", cv)))

    r = _run_patchdmp(
        "--build-id", f"libcliover.so={fake_bid.hex()}",
        "--in-place", str(dmp),
    )
    assert r.returncode == 0, r.stderr

    patched_data = dmp.read_bytes()
    cv_rva = struct.unpack_from("<I", patched_data, _MOD_CV_RVA_OFF)[0]
    sig = struct.unpack_from("<I", patched_data, cv_rva)[0]
    assert sig == _BPEL_SIG
    assert bytes(patched_data[cv_rva + 4 : cv_rva + 4 + 16]) == fake_bid


def test_cli_build_id_invalid_hex(tmp: Path) -> None:
    cv = _pdb70_record("/nonexistent/lib.so")
    dmp = tmp / "bad.dmp"
    dmp.write_bytes(bytes(_build_test_minidump("/nonexistent/lib.so", cv)))

    r = _run_patchdmp("--build-id", "lib.so=ZZZZ", "--in-place", str(dmp))
    assert r.returncode != 0
    assert "valid hex" in r.stderr.lower() or "hex" in r.stderr.lower()


def test_cli_build_id_odd_length(tmp: Path) -> None:
    cv = _pdb70_record("/nonexistent/lib.so")
    dmp = tmp / "odd.dmp"
    dmp.write_bytes(bytes(_build_test_minidump("/nonexistent/lib.so", cv)))

    r = _run_patchdmp("--build-id", "lib.so=abc", "--in-place", str(dmp))
    assert r.returncode != 0
    assert "odd" in r.stderr.lower()
