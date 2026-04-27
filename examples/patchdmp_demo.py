#!/usr/bin/env python3
"""patchdmp_demo.py — end-to-end demo of crashomon-patchdmp.

Demonstrates that patchdmp correctly patches the Breakpad XOR-fallback build
ID into a minidump for a module compiled without --build-id, enabling
minidump-stackwalk to resolve the module's symbol file.

Run from the repo root:
    python3 examples/patchdmp_demo.py

Prerequisites:
    pip install -e tools/
    cmake -B _dump_syms_build -S cmake/dump_syms_host/
    cmake --build _dump_syms_build -j$(nproc)
"""

from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# ── Tool discovery ─────────────────────────────────────────────────────────────

REPO = Path(__file__).resolve().parent.parent


def _find(name: str, candidates: list[Path]) -> str:
    p = shutil.which(name)
    if p:
        return p
    for c in candidates:
        if c.exists():
            return str(c)
    sys.exit(f"error: {name!r} not found — see script prerequisites")


DUMP_SYMS = _find("dump_syms", [
    REPO / "_dump_syms_build" / "dump_syms",
    REPO / "_host_toolkit" / "dump_syms",
])
STACKWALK = _find("minidump-stackwalk", [
    REPO / "_host_toolkit" / "bin" / "minidump-stackwalk",
])
PATCHDMP = _find("crashomon-patchdmp", [
    REPO / "tools" / "patchdmp" / "crashomon-patchdmp",
])
CRASHOMON_SYMS = _find("crashomon-syms", [
    REPO / "tools" / "syms" / "crashomon-syms",
])


# ── Minidump builder ───────────────────────────────────────────────────────────
#
# Constructs a minimal but standards-compliant minidump with three streams:
#   type 7  — MD_SYSTEM_INFO_STREAM (56 bytes, Linux/amd64)
#   type 3  — MD_THREAD_LIST_STREAM (one placeholder thread, no stack)
#   type 4  — MD_MODULE_LIST_STREAM (one module with a CV record)
#
# Stream types come from minidump_format.h (google_breakpad/common/).

def _build_minidump(module_path: str, cv_bytes: bytes) -> bytes:
    """Return a minimal valid minidump bytes with one module."""
    name_utf16 = module_path.encode("utf-16-le")
    name_len = len(name_utf16)

    # ── Layout ────────────────────────────────────────────────────────────────
    HEADER = 32           # MINIDUMP_HEADER
    DIR_ENTRY = 12        # MINIDUMP_DIRECTORY entry
    NUM_STREAMS = 3

    SYSINFO_SIZE = 56     # MDRawSystemInfo
    THREAD_LIST_SIZE = 4 + 48  # count(4) + MDRawThread(48)
    MODULE_SIZE = 108

    dir_rva = HEADER                               # 32
    sysinfo_rva = dir_rva + NUM_STREAMS * DIR_ENTRY  # 68
    threadlist_rva = sysinfo_rva + SYSINFO_SIZE      # 124
    modulelist_rva = threadlist_rva + THREAD_LIST_SIZE  # 176
    name_rva = modulelist_rva + 4 + MODULE_SIZE         # 288
    cv_rva = name_rva + 4 + name_len + 2               # after UTF-16 name + null
    total = cv_rva + len(cv_bytes)

    buf = bytearray(total)

    def w32(off: int, val: int) -> None:
        struct.pack_into("<I", buf, off, val)

    def w16(off: int, val: int) -> None:
        struct.pack_into("<H", buf, off, val)

    def w64(off: int, val: int) -> None:
        struct.pack_into("<Q", buf, off, val)

    # MINIDUMP_HEADER
    w32(0, 0x504D444D)   # Signature "MDMP"
    w32(4, 0x0000A793)   # Version
    w32(8, NUM_STREAMS)
    w32(12, dir_rva)
    # CheckSum=0, TimeDateStamp=0, Flags=0 already

    # MINIDUMP_DIRECTORY[0]: MD_SYSTEM_INFO_STREAM (type 7)
    w32(dir_rva + 0, 7)
    w32(dir_rva + 4, SYSINFO_SIZE)
    w32(dir_rva + 8, sysinfo_rva)

    # MINIDUMP_DIRECTORY[1]: MD_THREAD_LIST_STREAM (type 3)
    w32(dir_rva + 12 + 0, 3)
    w32(dir_rva + 12 + 4, THREAD_LIST_SIZE)
    w32(dir_rva + 12 + 8, threadlist_rva)

    # MINIDUMP_DIRECTORY[2]: MD_MODULE_LIST_STREAM (type 4)
    w32(dir_rva + 24 + 0, 4)
    w32(dir_rva + 24 + 4, 4 + MODULE_SIZE)
    w32(dir_rva + 24 + 8, modulelist_rva)

    # MD_SYSTEM_INFO_STREAM — Linux/amd64 minimal values
    # processor_architecture=9 (AMD64), platform_id=0 (Linux = 0 in Breakpad)
    w16(sysinfo_rva + 0, 9)   # processor_architecture (AMD64)
    w16(sysinfo_rva + 2, 6)   # processor_level
    w16(sysinfo_rva + 4, 0)   # processor_revision
    buf[sysinfo_rva + 6] = 4  # number_of_processors
    buf[sysinfo_rva + 7] = 1  # product_type (workstation)
    w32(sysinfo_rva + 8, 5)   # major_version (Linux 5.x)
    w32(sysinfo_rva + 12, 15) # minor_version
    w32(sysinfo_rva + 16, 0)  # build_number
    w32(sysinfo_rva + 20, 0)  # platform_id (0 = Linux in Breakpad)

    # MD_THREAD_LIST_STREAM — one placeholder thread (no real stack)
    w32(threadlist_rva, 1)  # thread count
    # MDRawThread: thread_id, suspend_count, priority_class, priority,
    #              teb(Q), stack.start_of_memory_range(Q), stack.memory.data_size,
    #              stack.memory.rva, context.data_size, context.rva
    w32(threadlist_rva + 4 + 0, 1)  # thread_id

    # MD_MODULE_LIST_STREAM — one module
    w32(modulelist_rva, 1)  # module count
    mod_off = modulelist_rva + 4
    w64(mod_off + 0, 0x7F000000)    # BaseOfImage
    w32(mod_off + 8, 0x10000)       # SizeOfImage
    w32(mod_off + 20, name_rva)     # ModuleNameRva
    w32(mod_off + 76, len(cv_bytes))  # CvRecord.DataSize
    w32(mod_off + 80, cv_rva)         # CvRecord.Rva

    # Module name: length(4) + UTF-16LE + null(2)
    w32(name_rva, name_len)
    buf[name_rva + 4 : name_rva + 4 + name_len] = name_utf16

    # CV record
    buf[cv_rva : cv_rva + len(cv_bytes)] = cv_bytes

    return bytes(buf)


def _pdb70_cv(pdb_path: str) -> bytes:
    """PDB70 CV record — what Crashpad writes for modules without .note.gnu.build-id."""
    sig = struct.pack("<I", 0x53445352)   # MD_CVINFOPDB70_SIGNATURE "RSDS"
    guid = b"\x00" * 16
    age = struct.pack("<I", 0)
    return sig + guid + age + pdb_path.encode() + b"\x00"


# ── Demo ───────────────────────────────────────────────────────────────────────

def _stackwalk_module_info(dmp: Path, sym_store: Path) -> dict[str, dict[str, str]]:
    """Return {module_name: {code_id, debug_id}} from minidump-stackwalk JSON."""
    r = subprocess.run(
        [STACKWALK, "--json", str(dmp), "--symbols-path", str(sym_store)],
        capture_output=True, text=True,
    )
    if not r.stdout:
        return {}
    data = json.loads(r.stdout)
    result = {}
    for mod in data.get("modules", []):
        name = Path(mod.get("filename", "?")).name
        result[name] = {
            "code_id": mod.get("code_id", ""),
            "debug_id": mod.get("debug_id", ""),
        }
    return result


def _section(title: str) -> None:
    print(f"\n{'─' * 60}")
    print(f"  {title}")
    print('─' * 60)


def main() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="patchdmp_demo_"))
    sym_store = tmp / "symbols"
    sym_store.mkdir()

    print("crashomon-patchdmp demo")
    print(f"Working directory: {tmp}")

    # ── Step 1: compile a shared library without a build-ID note ──────────────
    _section("Step 1: compile libdemo.so with --build-id=none")
    src = tmp / "libdemo.c"
    src.write_text("""\
/* demo library — compiled without .note.gnu.build-id */
int demo_add(int a, int b) { return a + b; }
int demo_mul(int a, int b) { return a * b; }
""")
    lib = tmp / "libdemo.so"
    subprocess.run(
        ["gcc", "-shared", "-fPIC", "-g", "-Wl,--build-id=none", str(src), "-o", str(lib)],
        check=True,
    )
    print(f"Compiled: {lib}")

    # ── Step 2: ingest symbols with crashomon-syms ─────────────────────────────
    _section("Step 2: ingest symbols into the symbol store")
    subprocess.run(
        [CRASHOMON_SYMS, "add", f"--dump-syms={DUMP_SYMS}", f"--store={sym_store}", str(lib)],
        check=True,
    )

    # Find the .sym file to show the MODULE line build ID
    sym_file = next(sym_store.rglob("*.sym"))
    module_line = sym_file.read_text().splitlines()[0]
    module_build_id = module_line.split()[3]
    print(f"Symbol store: {sym_file.parent.parent.name}/{sym_file.parent.name}/")
    print(f"MODULE line build ID: {module_build_id!r}")

    # ── Step 3: create a minidump with PDB70 CV record ────────────────────────
    _section("Step 3: create a minidump with PDB70 CV record (no build ID)")
    cv = _pdb70_cv(str(lib))
    dmp = tmp / "crash.dmp"
    dmp.write_bytes(_build_minidump(str(lib), cv))
    print(f"Minidump: {dmp}")
    print("CV record signature: RSDS (PDB70) — Breakpad returns 'id' for this module")

    # ── Step 4: symbolicate BEFORE patching ───────────────────────────────────
    _section("Step 4: minidump-stackwalk BEFORE patching")
    before = _stackwalk_module_info(dmp, sym_store)
    lib_name = lib.name
    info_before = before.get(lib_name, {})
    code_id_before = info_before.get("code_id", "")
    debug_id_before = info_before.get("debug_id", "")
    print(f"  {lib_name}")
    print(f"    code_id  = {code_id_before!r}")
    print(f"    debug_id = {debug_id_before!r}")
    if not code_id_before:
        print("  → code_id is empty: symbol store lookup FAILS")

    # ── Step 5: patch the minidump ─────────────────────────────────────────────
    _section("Step 5: patch minidump with crashomon-patchdmp")
    r = subprocess.run(
        [PATCHDMP, "--in-place", str(dmp)],
        capture_output=True, text=True,
    )
    print(r.stdout.rstrip())
    if r.returncode != 0:
        print(r.stderr.rstrip(), file=sys.stderr)
        sys.exit(1)

    # Show what the patched CV record contains
    data = dmp.read_bytes()
    # module list stream: dir[2] rva is at dir_rva + 24 + 8 = 32 + 24 + 8 = 64
    dir_rva = struct.unpack_from("<I", data, 12)[0]
    mod_stream_rva = struct.unpack_from("<I", data, dir_rva + 24 + 8)[0]
    mod_off = mod_stream_rva + 4
    cv_rva = struct.unpack_from("<I", data, mod_off + 80)[0]
    cv_sig = struct.unpack_from("<I", data, cv_rva)[0]
    build_id_bytes = data[cv_rva + 4 : cv_rva + 20]   # first 16 bytes
    raw_hex = build_id_bytes.hex().upper()
    # Simulate debug_identifier(): guid_and_age_to_debug_id on first 16 bytes
    d1, d2, d3 = struct.unpack_from("<IHH", build_id_bytes, 0)
    d4 = build_id_bytes[8:]
    debug_id = f"{d1:08X}{d2:04X}{d3:04X}" + "".join(f"{b:02X}" for b in d4) + "0"
    print(f"CV signature after patch: {'BpEL' if cv_sig == 0x4270454C else hex(cv_sig)}")
    print(f"Build ID (raw hex)       : {raw_hex}")
    print(f"debug_identifier()       : {debug_id!r}")
    print(f"dump_syms MODULE build ID: {module_build_id!r}")
    match = debug_id == module_build_id
    print(f"Match: {'YES ✓' if match else 'NO ✗ — BUG'}")

    # ── Step 6: symbolicate AFTER patching ────────────────────────────────────
    _section("Step 6: minidump-stackwalk AFTER patching")
    after = _stackwalk_module_info(dmp, sym_store)
    info_after = after.get(lib_name, {})
    code_id_after = info_after.get("code_id", "")
    debug_id_after = info_after.get("debug_id", "")
    print(f"  {lib_name}")
    print(f"    code_id  = {code_id_after!r}")
    print(f"    debug_id = {debug_id_after!r}")
    # debug_id matches the symbol store directory name → lookup will succeed
    sym_dir_name = sym_file.parent.name
    debug_id_match = debug_id_after == sym_dir_name
    if debug_id_match:
        print(f"  → debug_id matches sym store dir '{sym_dir_name}'")
        print(f"  → symbol store lookup SUCCEEDS ✓")
    else:
        print(f"  → debug_id {debug_id_after!r} does NOT match sym dir '{sym_dir_name}' ✗")

    _section("Summary")
    print(f"  Before: code_id={code_id_before!r}  debug_id={debug_id_before!r}")
    print(f"  After:  code_id={code_id_after!r}  debug_id={debug_id_after!r}")
    print(f"\n  Symbol store path: {sym_store}/libdemo.so/{sym_dir_name}/libdemo.so.sym")
    print(f"  debug_id → store dir match: {'YES ✓' if debug_id_match else 'NO ✗'}")


if __name__ == "__main__":
    main()
