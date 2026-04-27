"""Minidump build-ID fallback patcher for crashomon."""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator


# ── ELF constants ─────────────────────────────────────────────────────────────

_ELF_MAGIC = b"\x7fELF"
_ELFCLASS32 = 1
_ELFCLASS64 = 2
_SHT_PROGBITS = 1
_SHT_NOTE = 7
_NT_GNU_BUILD_ID = 3

# ── Minidump constants ────────────────────────────────────────────────────────

_MINIDUMP_SIGNATURE = 0x504D444D
_MD_MODULE_LIST_STREAM = 4
_BPEL_SIGNATURE = 0x4270454C  # MD_CVINFOELF_SIGNATURE; in-memory bytes: 4C 45 70 42
_XOR_HASH_SIZE = 16  # kMDGUIDSize
_XOR_MAX_BYTES = 4096
_MODULE_SIZE = 108
# Offsets within MDRawModule:
_MOD_NAME_RVA_OFF = 20
_MOD_CV_DATA_SIZE_OFF = 76
_MOD_CV_RVA_OFF = 80


# ── ELF build-ID reader ───────────────────────────────────────────────────────


def compute_elf_build_id(elf_path: Path) -> bytes | None:
    """Return the build ID for an ELF binary.

    Priority mirrors dump_syms: GNU .note.gnu.build-id first, then Breakpad's
    XOR-text-section fallback (first 4096 bytes of .text XOR'd in 16-byte
    chunks, zero-padded to a 16-byte boundary).  Returns raw bytes, or None if
    the file is not a valid ELF or computation fails.
    """
    try:
        data = elf_path.read_bytes()
    except OSError:
        return None

    if len(data) < 16 or data[:4] != _ELF_MAGIC:
        return None

    is64 = data[4] == _ELFCLASS64

    if is64:
        if len(data) < 64:
            return None
        (e_shoff,) = struct.unpack_from("<Q", data, 40)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 58)
    else:
        if len(data) < 52:
            return None
        (e_shoff,) = struct.unpack_from("<I", data, 32)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)

    if e_shoff == 0 or e_shnum == 0 or e_shstrndx == 0:
        return None
    if e_shoff + e_shnum * e_shentsize > len(data):
        return None

    shstrtab_off = _get_shstrtab_offset(data, e_shoff, e_shentsize, e_shstrndx, is64)
    if shstrtab_off is None:
        return None

    text_off: int | None = None
    text_size: int | None = None

    for i in range(e_shnum):
        sh_name, sh_type, sh_offset, sh_size = _read_shdr(
            data, e_shoff, e_shentsize, i, is64
        )
        name_off = shstrtab_off + sh_name
        if name_off >= len(data):
            continue
        sec_name = _c_str(data, name_off)

        if sh_type == _SHT_NOTE and sec_name == ".note.gnu.build-id":
            if sh_offset + sh_size <= len(data):
                bid = _parse_build_id_note(data, sh_offset, sh_size)
                if bid is not None:
                    return bid

        if sh_type == _SHT_PROGBITS and sec_name == ".text" and text_off is None:
            text_off = sh_offset
            text_size = sh_size

    if text_off is None or not text_size:
        return None

    return _xor_text_hash(data, text_off, int(text_size))


def _get_shstrtab_offset(
    data: bytes, e_shoff: int, e_shentsize: int, e_shstrndx: int, is64: bool
) -> int | None:
    _, _, sh_offset, _ = _read_shdr(data, e_shoff, e_shentsize, e_shstrndx, is64)
    return sh_offset if sh_offset != 0 else None


def _read_shdr(
    data: bytes, e_shoff: int, e_shentsize: int, index: int, is64: bool
) -> tuple[int, int, int, int]:
    """Return (sh_name, sh_type, sh_offset, sh_size) for section at index."""
    off = e_shoff + index * e_shentsize
    if is64:
        sh_name, sh_type = struct.unpack_from("<II", data, off)
        sh_offset, sh_size = struct.unpack_from("<QQ", data, off + 24)
    else:
        sh_name, sh_type = struct.unpack_from("<II", data, off)
        sh_offset, sh_size = struct.unpack_from("<II", data, off + 16)
    return sh_name, sh_type, sh_offset, sh_size


def _c_str(data: bytes, off: int) -> str:
    try:
        end = data.index(b"\x00", off)
    except ValueError:
        end = len(data)
    return data[off:end].decode("ascii", errors="replace")


def _parse_build_id_note(data: bytes, sh_offset: int, sh_size: int) -> bytes | None:
    """Extract NT_GNU_BUILD_ID desc bytes from a SHT_NOTE section."""
    end = sh_offset + sh_size
    pos = sh_offset
    while pos + 12 <= end:
        namesz, descsz, ntype = struct.unpack_from("<III", data, pos)
        pos += 12
        namesz_padded = (namesz + 3) & ~3
        descsz_padded = (descsz + 3) & ~3
        if ntype == _NT_GNU_BUILD_ID and descsz > 0:
            desc_off = pos + namesz_padded
            if desc_off + descsz <= end:
                return data[desc_off : desc_off + descsz]
        pos += namesz_padded + descsz_padded
    return None


def _xor_text_hash(data: bytes, text_off: int, text_size: int) -> bytes:
    """Compute Breakpad's XOR-text-section fallback build ID.

    Replicates HashElfTextSection() from Breakpad's common/linux/file_id.cc:
    XOR first min(text_size, 4096) bytes in 16-byte chunks.  When text_size is
    not a multiple of 16, the final partial chunk reads actual file bytes past
    text_size — matching Breakpad's mmap-backed ptr[j+k] without bounds clamping.
    Zeros are used only if the file genuinely ends before the chunk boundary.
    """
    limit = min(text_size, _XOR_MAX_BYTES)
    # Round up to the next 16-byte boundary to include real file bytes past
    # text_size in the last chunk — matching Breakpad's unclamped ptr[j+k] read.
    pad = (-limit) % _XOR_HASH_SIZE
    chunk = data[text_off : text_off + limit + pad]
    # Zero-pad only if the file ends before the chunk boundary.
    rem = len(chunk) % _XOR_HASH_SIZE
    if rem:
        chunk = chunk + bytes(_XOR_HASH_SIZE - rem)
    acc = bytearray(_XOR_HASH_SIZE)
    for i in range(0, len(chunk), _XOR_HASH_SIZE):
        for j in range(_XOR_HASH_SIZE):
            acc[j] ^= chunk[i + j]
    return bytes(acc)


# ── Minidump module iteration ─────────────────────────────────────────────────


@dataclass
class _ModuleRecord:
    name: str
    cv_rva: int
    cv_size: int
    cv_sig: int  # first uint32 of CV record bytes (0 if cv_rva == 0)
    cv_size_field_off: int  # file offset of CvRecord.DataSize field (for patching)


def _iter_modules(data: bytes) -> Iterator[_ModuleRecord]:
    """Yield module records from a minidump's MD_MODULE_LIST_STREAM."""
    if len(data) < 32:
        return
    sig, = struct.unpack_from("<I", data, 0)
    if sig != _MINIDUMP_SIGNATURE:
        return

    stream_count, dir_rva = struct.unpack_from("<II", data, 8)

    module_stream_rva = 0
    for i in range(stream_count):
        entry_off = dir_rva + i * 12
        if entry_off + 12 > len(data):
            break
        stype, _, srva = struct.unpack_from("<III", data, entry_off)
        if stype == _MD_MODULE_LIST_STREAM:
            module_stream_rva = srva
            break

    if module_stream_rva == 0 or module_stream_rva + 4 > len(data):
        return

    (module_count,) = struct.unpack_from("<I", data, module_stream_rva)
    if module_count > 4096:
        return

    for i in range(module_count):
        mod_off = module_stream_rva + 4 + i * _MODULE_SIZE
        if mod_off + _MODULE_SIZE > len(data):
            break

        (name_rva,) = struct.unpack_from("<I", data, mod_off + _MOD_NAME_RVA_OFF)
        cv_size, cv_rva = struct.unpack_from("<II", data, mod_off + _MOD_CV_DATA_SIZE_OFF)

        name = _read_module_name(data, name_rva)
        cv_sig = 0
        if cv_rva != 0 and cv_size >= 4 and cv_rva + 4 <= len(data):
            (cv_sig,) = struct.unpack_from("<I", data, cv_rva)

        yield _ModuleRecord(
            name=name,
            cv_rva=cv_rva,
            cv_size=cv_size,
            cv_sig=cv_sig,
            cv_size_field_off=mod_off + _MOD_CV_DATA_SIZE_OFF,
        )


def _read_module_name(data: bytes, rva: int) -> str:
    if rva == 0 or rva + 4 > len(data):
        return ""
    (length,) = struct.unpack_from("<I", data, rva)
    if length == 0 or rva + 4 + length > len(data):
        return ""
    return data[rva + 4 : rva + 4 + length].decode("utf-16-le", errors="replace")


# ── Minidump patcher ──────────────────────────────────────────────────────────


def patch_minidump(data: bytearray, sysroot: Path | None = None) -> int:
    """Patch modules with missing BpEL CodeView records in a minidump buffer.

    For each module whose CV record is not already BpEL, resolves the ELF file
    (via sysroot if provided, else the absolute path from the minidump), computes
    the build ID (GNU note or XOR fallback), and overwrites the CV record
    in-place.  The existing record must have DataSize >= 4 + len(build_id) (PDB70
    always satisfies this: 4-byte sig + 16-byte GUID + 4-byte age + path >= 25).

    Returns the number of modules patched.
    """
    patched = 0
    for mod in _iter_modules(bytes(data)):
        if mod.cv_sig == _BPEL_SIGNATURE:
            continue
        if mod.cv_rva == 0 or mod.cv_size < 4:
            continue

        elf_path = _resolve_elf(mod.name, sysroot)
        if elf_path is None:
            continue

        build_id = compute_elf_build_id(elf_path)
        if build_id is None:
            continue

        bpel_size = 4 + len(build_id)
        if mod.cv_size < bpel_size:
            continue

        struct.pack_into("<I", data, mod.cv_rva, _BPEL_SIGNATURE)
        data[mod.cv_rva + 4 : mod.cv_rva + 4 + len(build_id)] = build_id
        struct.pack_into("<I", data, mod.cv_size_field_off, bpel_size)

        patched += 1

    return patched


def _resolve_elf(module_path: str, sysroot: Path | None) -> Path | None:
    if not module_path:
        return None
    p = Path(module_path)
    if sysroot is not None:
        rel = p.relative_to("/") if p.is_absolute() else p
        candidate = sysroot / rel
        if candidate.exists():
            return candidate
    if p.exists():
        return p
    return None
