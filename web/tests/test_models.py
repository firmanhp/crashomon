"""Tests for web/models.py"""

from __future__ import annotations

import sqlite3

from web import models

# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------


def test_init_creates_crashes_table(tmp_db):
    conn = sqlite3.connect(str(tmp_db))
    tables = {r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    conn.close()
    assert "crashes" in tables


def test_init_idempotent(tmp_db):
    models.init_db(tmp_db)  # second call must not raise
    models.init_db(tmp_db)


# ---------------------------------------------------------------------------
# insert / get_crash
# ---------------------------------------------------------------------------


def test_insert_returns_int_id(tmp_db):
    crash_id = models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc", signal="SIGSEGV", report="r"
    )
    assert isinstance(crash_id, int) and crash_id > 0


def test_insert_and_get_roundtrip(tmp_db):
    crash_id = models.insert_crash(
        tmp_db,
        ts="2026-03-28T10:00:00Z",
        process="my_service",
        signal="SIGSEGV",
        pid=1234,
        report="crash report text",
        dmp_path="/var/crashomon/abc.dmp",
    )
    row = models.get_crash(tmp_db, crash_id)
    assert row is not None
    assert row["process"] == "my_service"
    assert row["signal"] == "SIGSEGV"
    assert row["pid"] == 1234
    assert row["report"] == "crash report text"
    assert row["dmp_path"] == "/var/crashomon/abc.dmp"


def test_get_crash_not_found(tmp_db):
    assert models.get_crash(tmp_db, 99999) is None


def test_insert_defaults(tmp_db):
    crash_id = models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc", signal="SIGABRT", report="r"
    )
    row = models.get_crash(tmp_db, crash_id)
    assert row["pid"] == 0
    assert row["dmp_path"] == ""


# ---------------------------------------------------------------------------
# get_crashes
# ---------------------------------------------------------------------------


def test_get_crashes_empty(tmp_db):
    assert models.get_crashes(tmp_db) == []


def test_get_crashes_returns_list(tmp_db):
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc_a", signal="SIGSEGV", report="r"
    )
    models.insert_crash(
        tmp_db, ts="2026-03-28T11:00:00Z", process="svc_b", signal="SIGABRT", report="r"
    )
    rows = models.get_crashes(tmp_db)
    assert len(rows) == 2


def test_get_crashes_ordered_desc(tmp_db):
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc", signal="SIGSEGV", report="r1"
    )
    models.insert_crash(
        tmp_db, ts="2026-03-29T10:00:00Z", process="svc", signal="SIGSEGV", report="r2"
    )
    rows = models.get_crashes(tmp_db)
    assert rows[0]["ts"] > rows[1]["ts"]


def test_get_crashes_filter_process(tmp_db):
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc_a", signal="SIGSEGV", report="r"
    )
    models.insert_crash(
        tmp_db, ts="2026-03-28T11:00:00Z", process="svc_b", signal="SIGABRT", report="r"
    )
    rows = models.get_crashes(tmp_db, process="svc_a")
    assert len(rows) == 1
    assert rows[0]["process"] == "svc_a"


def test_get_crashes_filter_signal(tmp_db):
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc_a", signal="SIGSEGV", report="r"
    )
    models.insert_crash(
        tmp_db, ts="2026-03-28T11:00:00Z", process="svc_b", signal="SIGABRT", report="r"
    )
    rows = models.get_crashes(tmp_db, signal="SIGABRT")
    assert len(rows) == 1
    assert rows[0]["signal"] == "SIGABRT"


def test_get_crashes_filter_both(tmp_db):
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc_a", signal="SIGSEGV", report="r"
    )
    models.insert_crash(
        tmp_db, ts="2026-03-28T11:00:00Z", process="svc_a", signal="SIGABRT", report="r"
    )
    rows = models.get_crashes(tmp_db, process="svc_a", signal="SIGSEGV")
    assert len(rows) == 1


def test_get_crashes_limit(tmp_db):
    for i in range(5):
        models.insert_crash(
            tmp_db, ts=f"2026-03-28T0{i}:00:00Z", process="svc", signal="SIGSEGV", report="r"
        )
    assert len(models.get_crashes(tmp_db, limit=3)) == 3


def test_get_crashes_offset(tmp_db):
    for i in range(4):
        models.insert_crash(
            tmp_db, ts=f"2026-03-28T0{i}:00:00Z", process="svc", signal="SIGSEGV", report="r"
        )
    all_rows = models.get_crashes(tmp_db)
    offset_rows = models.get_crashes(tmp_db, offset=2, limit=50)
    assert len(offset_rows) == 2
    assert all_rows[2]["id"] == offset_rows[0]["id"]


def test_get_crashes_list_cols(tmp_db):
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc", signal="SIGSEGV", report="r"
    )
    row = models.get_crashes(tmp_db)[0]
    # List view should NOT include full report (only detail view does)
    assert "id" in row
    assert "ts" in row
    assert "process" in row
    assert "signal" in row
    assert "report" not in row


# ---------------------------------------------------------------------------
# delete_crash
# ---------------------------------------------------------------------------


def test_delete_crash(tmp_db):
    crash_id = models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc", signal="SIGSEGV", report="r"
    )
    models.delete_crash(tmp_db, crash_id)
    assert models.get_crash(tmp_db, crash_id) is None


def test_delete_nonexistent_is_noop(tmp_db):
    models.delete_crash(tmp_db, 99999)  # must not raise


# ---------------------------------------------------------------------------
# get_frequency
# ---------------------------------------------------------------------------


def test_get_frequency_empty(tmp_db):
    assert models.get_frequency(tmp_db) == []


def test_get_frequency_counts(tmp_db):
    for _ in range(3):
        models.insert_crash(
            tmp_db, ts="2026-03-28T10:00:00Z", process="svc_a", signal="SIGSEGV", report="r"
        )
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc_b", signal="SIGABRT", report="r"
    )
    freq = models.get_frequency(tmp_db)
    assert freq[0]["process"] == "svc_a"
    assert freq[0]["count"] == 3
    assert freq[1]["process"] == "svc_b"
    assert freq[1]["count"] == 1


def test_get_frequency_sorted_desc(tmp_db):
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="rare", signal="SIGSEGV", report="r"
    )
    for _ in range(5):
        models.insert_crash(
            tmp_db, ts="2026-03-28T10:00:00Z", process="common", signal="SIGSEGV", report="r"
        )
    freq = models.get_frequency(tmp_db)
    assert freq[0]["process"] == "common"


# ---------------------------------------------------------------------------
# compute_signature
# ---------------------------------------------------------------------------

_REPORT_WITH_FRAMES = (
    "pid: 1, tid: 1, name: svc  >>> svc <<<\n"
    "signal 11 (SIGSEGV)\n"
    "backtrace:\n"
    " 0  /usr/bin/svc + 0x1234\n"
    " 1  /usr/lib/liba.so + 0xabcd\n"
    " 2  /usr/lib/libb.so + 0x5678\n"
    " 3  /usr/lib/libc.so + 0x9999\n"
)


def test_compute_signature_returns_16_hex_chars():
    sig = models.compute_signature("svc", "SIGSEGV", _REPORT_WITH_FRAMES)
    assert len(sig) == 16
    assert all(c in "0123456789abcdef" for c in sig)


def test_compute_signature_empty_report_returns_empty():
    assert models.compute_signature("svc", "SIGSEGV", "no frames here") == ""


def test_compute_signature_same_inputs_same_output():
    sig1 = models.compute_signature("svc", "SIGSEGV", _REPORT_WITH_FRAMES)
    sig2 = models.compute_signature("svc", "SIGSEGV", _REPORT_WITH_FRAMES)
    assert sig1 == sig2


def test_compute_signature_uses_only_top_3_frames():
    # Report with exactly 3 frames should match report with 4 frames (same top 3).
    report_3 = (
        "backtrace:\n"
        " 0  /usr/bin/svc + 0x1234\n"
        " 1  /usr/lib/liba.so + 0xabcd\n"
        " 2  /usr/lib/libb.so + 0x5678\n"
    )
    report_4 = report_3 + " 3  /usr/lib/libc.so + 0x9999\n"
    assert models.compute_signature("svc", "SIGSEGV", report_3) == models.compute_signature(
        "svc", "SIGSEGV", report_4
    )


def test_compute_signature_different_process_different_sig():
    sig_a = models.compute_signature("svc_a", "SIGSEGV", _REPORT_WITH_FRAMES)
    sig_b = models.compute_signature("svc_b", "SIGSEGV", _REPORT_WITH_FRAMES)
    assert sig_a != sig_b


# ---------------------------------------------------------------------------
# get_crash_groups
# ---------------------------------------------------------------------------


def test_get_crash_groups_empty(tmp_db):
    assert models.get_crash_groups(tmp_db) == []


def test_get_crash_groups_excludes_no_signature(tmp_db):
    # Reports without frames produce empty signature and are excluded.
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc", signal="SIGSEGV", report="no frames"
    )
    assert models.get_crash_groups(tmp_db) == []


def test_get_crash_groups_returns_group(tmp_db):
    models.insert_crash(
        tmp_db,
        ts="2026-03-28T10:00:00Z",
        process="svc",
        signal="SIGSEGV",
        report=_REPORT_WITH_FRAMES,
    )
    groups = models.get_crash_groups(tmp_db)
    assert len(groups) == 1
    row = groups[0]
    assert row["process"] == "svc"
    assert row["signal"] == "SIGSEGV"
    assert row["count"] == 1
    assert row["first_seen"] == row["last_seen"]
    assert "sample_crash_id" in row
    assert "signature" in row


def test_get_crash_groups_count(tmp_db):
    for ts in ("2026-03-28T10:00:00Z", "2026-03-28T11:00:00Z", "2026-03-28T12:00:00Z"):
        models.insert_crash(
            tmp_db, ts=ts, process="svc", signal="SIGSEGV", report=_REPORT_WITH_FRAMES
        )
    groups = models.get_crash_groups(tmp_db)
    assert len(groups) == 1
    assert groups[0]["count"] == 3


def test_get_crash_groups_sorted_by_count_desc(tmp_db):
    for _ in range(3):
        models.insert_crash(
            tmp_db,
            ts="2026-03-28T10:00:00Z",
            process="svc_a",
            signal="SIGSEGV",
            report=_REPORT_WITH_FRAMES,
        )
    models.insert_crash(
        tmp_db,
        ts="2026-03-28T10:00:00Z",
        process="svc_b",
        signal="SIGABRT",
        report=(
            "backtrace:\n"
            " 0  /usr/bin/svc_b + 0xaaaa\n"
            " 1  /usr/lib/other.so + 0xbbbb\n"
            " 2  /usr/lib/another.so + 0xcccc\n"
        ),
    )
    groups = models.get_crash_groups(tmp_db)
    assert groups[0]["count"] >= groups[1]["count"]


# ---------------------------------------------------------------------------
# get_daily_counts
# ---------------------------------------------------------------------------


def test_get_daily_counts_empty(tmp_db):
    assert models.get_daily_counts(tmp_db) == []


def test_get_daily_counts_returns_dates(tmp_db):
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc", signal="SIGSEGV", report="r"
    )
    models.insert_crash(
        tmp_db, ts="2026-03-28T12:00:00Z", process="svc", signal="SIGSEGV", report="r"
    )
    models.insert_crash(
        tmp_db, ts="2026-03-29T10:00:00Z", process="svc", signal="SIGSEGV", report="r"
    )
    counts = models.get_daily_counts(tmp_db)
    assert len(counts) == 2
    dates = [r["date"] for r in counts]
    assert "2026-03-28" in dates
    assert "2026-03-29" in dates


def test_get_daily_counts_ordered_oldest_first(tmp_db):
    models.insert_crash(
        tmp_db, ts="2026-03-28T10:00:00Z", process="svc", signal="SIGSEGV", report="r"
    )
    models.insert_crash(
        tmp_db, ts="2026-03-30T10:00:00Z", process="svc", signal="SIGSEGV", report="r"
    )
    counts = models.get_daily_counts(tmp_db)
    assert counts[0]["date"] < counts[-1]["date"]


def test_get_daily_counts_aggregates_per_day(tmp_db):
    for hour in range(4):
        models.insert_crash(
            tmp_db, ts=f"2026-03-28T{hour:02d}:00:00Z", process="svc", signal="SIGSEGV", report="r"
        )
    counts = models.get_daily_counts(tmp_db)
    assert counts[0]["count"] == 4
