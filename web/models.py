"""SQLite crash history — schema and queries."""

from __future__ import annotations

import hashlib
import re
import sqlite3
from pathlib import Path

_SCHEMA = """
CREATE TABLE IF NOT EXISTS crashes (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        TEXT    NOT NULL,
    process   TEXT    NOT NULL,
    signal    TEXT    NOT NULL,
    pid       INTEGER NOT NULL DEFAULT 0,
    report    TEXT    NOT NULL,
    dmp_path  TEXT    NOT NULL DEFAULT '',
    signature TEXT    NOT NULL DEFAULT ''
);
"""

# Migration: add signature column to existing databases that pre-date this field.
_MIGRATE_SIGNATURE = "ALTER TABLE crashes ADD COLUMN signature TEXT NOT NULL DEFAULT ''"


def _connect(db_path: str | Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row
    return conn


def init_db(db_path: str | Path) -> None:
    """Create schema if not already present; migrate legacy databases."""
    with _connect(db_path) as conn:
        conn.executescript(_SCHEMA)
        # Add signature column if the database predates this field.
        cols = {row[1] for row in conn.execute("PRAGMA table_info(crashes)")}
        if "signature" not in cols:
            conn.execute(_MIGRATE_SIGNATURE)


def compute_signature(process: str, signal: str, report: str) -> str:
    """Return a 16-char hex crash signature for grouping identical crashes.

    The signature is a truncated SHA-256 of process + signal + the first three
    symbolicated stack frames (module basename + hex offset).  Frames without a
    module path are skipped.  Returns an empty string when no frames are found.
    """
    frames: list[str] = []
    for line in report.splitlines():
        # Match lines like:  " 0  libfoo.so + 0x1234"
        m = re.search(r"^\s+\d+\s+(\S+)\s*\+\s*(0x[0-9a-fA-F]+)", line)
        if m:
            frames.append(f"{m.group(1)}+{m.group(2).lower()}")
            if len(frames) >= 3:
                break
    if not frames:
        return ""
    payload = f"{process}:{signal}:{','.join(frames)}"
    return hashlib.sha256(payload.encode()).hexdigest()[:16]


def insert_crash(
    db_path: str | Path,
    *,
    ts: str,
    process: str,
    signal: str,
    pid: int = 0,
    report: str,
    dmp_path: str = "",
) -> int:
    """Insert a crash record; return its row id."""
    sig = compute_signature(process, signal, report)
    with _connect(db_path) as conn:
        cur = conn.execute(
            "INSERT INTO crashes (ts, process, signal, pid, report, dmp_path, signature) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (ts, process, signal, pid, report, dmp_path, sig),
        )
        return cur.lastrowid  # type: ignore[return-value]


def get_crashes(
    db_path: str | Path,
    *,
    process: str = "",
    signal: str = "",
    limit: int = 50,
    offset: int = 0,
) -> list[dict]:
    """Return crashes ordered by ts desc, with optional filters."""
    sql = "SELECT id, ts, process, signal, pid, dmp_path FROM crashes"
    params: list[str | int] = []
    conditions: list[str] = []
    if process:
        conditions.append("process = ?")
        params.append(process)
    if signal:
        conditions.append("signal = ?")
        params.append(signal)
    if conditions:
        sql += " WHERE " + " AND ".join(conditions)
    sql += " ORDER BY ts DESC LIMIT ? OFFSET ?"
    params += [limit, offset]
    with _connect(db_path) as conn:
        rows = conn.execute(sql, params).fetchall()
    return [dict(r) for r in rows]


def get_crash(db_path: str | Path, crash_id: int) -> dict | None:
    """Return full crash record or None."""
    with _connect(db_path) as conn:
        row = conn.execute("SELECT * FROM crashes WHERE id = ?", (crash_id,)).fetchone()
    return dict(row) if row else None


def delete_crash(db_path: str | Path, crash_id: int) -> None:
    """Delete a crash record by id."""
    with _connect(db_path) as conn:
        conn.execute("DELETE FROM crashes WHERE id = ?", (crash_id,))


def get_frequency(db_path: str | Path) -> list[dict]:
    """Return per-process crash counts, sorted by count desc."""
    sql = "SELECT process, COUNT(*) as count FROM crashes GROUP BY process ORDER BY count DESC"
    with _connect(db_path) as conn:
        rows = conn.execute(sql).fetchall()
    return [dict(r) for r in rows]


def get_crash_groups(db_path: str | Path) -> list[dict]:
    """Return crash groups (by signature) with occurrence counts.

    Each row contains: signature, process, signal, count, first_seen,
    last_seen, sample_crash_id (most recent crash in the group).
    Crashes without a signature (legacy rows or no-frame reports) are excluded.
    """
    sql = """
        SELECT
            signature,
            process,
            signal,
            COUNT(*) AS count,
            MIN(ts)  AS first_seen,
            MAX(ts)  AS last_seen,
            MAX(id)  AS sample_crash_id
        FROM crashes
        WHERE signature != ''
        GROUP BY signature
        ORDER BY count DESC
    """
    with _connect(db_path) as conn:
        rows = conn.execute(sql).fetchall()
    return [dict(r) for r in rows]


def get_daily_counts(db_path: str | Path, days: int = 14) -> list[dict]:
    """Return crash counts per calendar day for the last *days* days.

    Each row contains: date (YYYY-MM-DD), count.  Days with no crashes are
    omitted.  Results are ordered from oldest to newest for chart display.
    """
    sql = """
        SELECT DATE(ts) AS date, COUNT(*) AS count
        FROM crashes
        GROUP BY DATE(ts)
        ORDER BY date DESC
        LIMIT ?
    """
    with _connect(db_path) as conn:
        rows = conn.execute(sql, (days,)).fetchall()
    return list(reversed([dict(r) for r in rows]))
