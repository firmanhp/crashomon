"""Tests for web/symbol_store.py"""

from __future__ import annotations

import os
import textwrap

import pytest

from web import symbol_store

SAMPLE_SYM = textwrap.dedent("""\
    MODULE Linux x86_64 AABBCCDD0011223344556677889900AA0 my_service
    FILE 0 src/main.cpp
    FUNC 1a0 20 0 main
    1a0 10 42 0
""")


def _alt_sym(
    build_id: str = "AABBCCDD0011223344556677889900AA0", module: str = "my_service"
) -> str:
    return SAMPLE_SYM.replace("AABBCCDD0011223344556677889900AA0", build_id).replace(
        "my_service", module
    )


# ---------------------------------------------------------------------------
# add_sym_text
# ---------------------------------------------------------------------------


def test_add_sym_text_creates_file(tmp_symbol_store):
    dest = symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    assert dest.exists()
    assert dest.name == "my_service.sym"
    assert dest.parent.name == "AABBCCDD0011223344556677889900AA0"
    assert dest.parent.parent.name == "my_service"


def test_add_sym_text_content_preserved(tmp_symbol_store):
    dest = symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    assert dest.read_text() == SAMPLE_SYM


def test_add_sym_text_invalid_raises(tmp_symbol_store):
    with pytest.raises(ValueError):
        symbol_store.add_sym_text(tmp_symbol_store, "not a sym file")


def test_add_sym_text_empty_raises(tmp_symbol_store):
    with pytest.raises(ValueError):
        symbol_store.add_sym_text(tmp_symbol_store, "")


def test_add_sym_text_overwrite(tmp_symbol_store):
    symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    updated = SAMPLE_SYM + "FUNC 200 10 0 other\n"
    dest = symbol_store.add_sym_text(tmp_symbol_store, updated)
    assert dest.read_text() == updated


# ---------------------------------------------------------------------------
# add_binary (requires dump_syms — tested via error path only)
# ---------------------------------------------------------------------------


def test_add_binary_missing_file_raises(tmp_symbol_store):
    with pytest.raises(FileNotFoundError):
        symbol_store.add_binary(tmp_symbol_store, "/no/such/binary")


def test_add_binary_missing_dump_syms_raises(tmp_symbol_store, tmp_path):
    # Create a dummy file so the path check passes.
    dummy = tmp_path / "dummy_bin"
    dummy.write_bytes(b"\x7fELF" + b"\x00" * 12)
    with pytest.raises(RuntimeError, match="dump_syms not found"):
        symbol_store.add_binary(tmp_symbol_store, dummy, dump_syms="/no/such/dump_syms")


# ---------------------------------------------------------------------------
# list_symbols
# ---------------------------------------------------------------------------


def test_list_symbols_empty_store(tmp_symbol_store):
    assert symbol_store.list_symbols(tmp_symbol_store) == []


def test_list_symbols_nonexistent_dir(tmp_path):
    assert symbol_store.list_symbols(tmp_path / "does_not_exist") == []


def test_list_symbols_single_entry(tmp_symbol_store):
    symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    entries = symbol_store.list_symbols(tmp_symbol_store)
    assert len(entries) == 1
    assert entries[0].module == "my_service"
    assert entries[0].build_id == "AABBCCDD0011223344556677889900AA0"
    assert entries[0].size_bytes > 0
    assert entries[0].mtime > 0


def test_list_symbols_multiple_modules(tmp_symbol_store):
    symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    symbol_store.add_sym_text(tmp_symbol_store, _alt_sym(module="other_svc"))
    entries = symbol_store.list_symbols(tmp_symbol_store)
    modules = {e.module for e in entries}
    assert modules == {"my_service", "other_svc"}


def test_list_symbols_multiple_build_ids(tmp_symbol_store):
    symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    symbol_store.add_sym_text(
        tmp_symbol_store, _alt_sym(build_id="1122334455667788990011223344556600")
    )
    entries = symbol_store.list_symbols(tmp_symbol_store)
    assert len(entries) == 2


def test_list_symbols_ignores_non_sym_files(tmp_symbol_store):
    symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    # Stray file in module dir should be ignored.
    stray = tmp_symbol_store / "my_service" / "AABBCCDD0011223344556677889900AA0" / "README"
    stray.write_text("stray")
    entries = symbol_store.list_symbols(tmp_symbol_store)
    assert len(entries) == 1


# ---------------------------------------------------------------------------
# find_sym
# ---------------------------------------------------------------------------


def test_find_sym_found(tmp_symbol_store):
    symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    path = symbol_store.find_sym(
        tmp_symbol_store, "my_service", "AABBCCDD0011223344556677889900AA0"
    )
    assert path is not None
    assert path.exists()


def test_find_sym_missing(tmp_symbol_store):
    assert symbol_store.find_sym(tmp_symbol_store, "no_module", "DEADBEEF0000") is None


def test_find_sym_wrong_build_id(tmp_symbol_store):
    symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    assert symbol_store.find_sym(tmp_symbol_store, "my_service", "WRONGBUILDID") is None


# ---------------------------------------------------------------------------
# prune
# ---------------------------------------------------------------------------


def test_prune_invalid_keep_raises(tmp_symbol_store):
    with pytest.raises(ValueError):
        symbol_store.prune(tmp_symbol_store, keep=0)


def test_prune_nonexistent_dir_returns_empty(tmp_path):
    assert symbol_store.prune(tmp_path / "no_dir") == []


def test_prune_no_op_when_under_limit(tmp_symbol_store):
    symbol_store.add_sym_text(tmp_symbol_store, SAMPLE_SYM)
    removed = symbol_store.prune(tmp_symbol_store, keep=3)
    assert removed == []


def test_prune_removes_oldest(tmp_symbol_store):
    ids = [
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
    ]
    for i, bid in enumerate(ids):
        dest = symbol_store.add_sym_text(tmp_symbol_store, _alt_sym(build_id=bid))
        os.utime(dest, (i * 1000, i * 1000))

    removed = symbol_store.prune(tmp_symbol_store, keep=2)
    assert len(removed) == 1
    # Oldest (mtime=0, ids[0]) removed; newer two kept.
    assert symbol_store.find_sym(tmp_symbol_store, "my_service", ids[0]) is None
    assert symbol_store.find_sym(tmp_symbol_store, "my_service", ids[1]) is not None
    assert symbol_store.find_sym(tmp_symbol_store, "my_service", ids[2]) is not None


def test_prune_removes_multiple_old(tmp_symbol_store):
    ids = [
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
        "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD",
    ]
    for i, bid in enumerate(ids):
        dest = symbol_store.add_sym_text(tmp_symbol_store, _alt_sym(build_id=bid))
        os.utime(dest, (i * 1000, i * 1000))

    removed = symbol_store.prune(tmp_symbol_store, keep=2)
    assert len(removed) == 2
    assert symbol_store.find_sym(tmp_symbol_store, "my_service", ids[2]) is not None
    assert symbol_store.find_sym(tmp_symbol_store, "my_service", ids[3]) is not None


def test_prune_keep_1(tmp_symbol_store):
    ids = ["AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"]
    for i, bid in enumerate(ids):
        dest = symbol_store.add_sym_text(tmp_symbol_store, _alt_sym(build_id=bid))
        os.utime(dest, (i * 1000, i * 1000))

    removed = symbol_store.prune(tmp_symbol_store, keep=1)
    assert len(removed) == 1
    assert symbol_store.find_sym(tmp_symbol_store, "my_service", ids[1]) is not None
