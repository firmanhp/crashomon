"""SQLite crash history — schema and queries."""

from __future__ import annotations

import sqlite3
from pathlib import Path

_SCHEMA = """
CREATE TABLE IF NOT EXISTS crashes (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    ts       TEXT    NOT NULL,
    process  TEXT    NOT NULL,
    signal   TEXT    NOT NULL,
    pid      INTEGER NOT NULL DEFAULT 0,
    report   TEXT    NOT NULL,
    dmp_path TEXT    NOT NULL DEFAULT ''
);
"""


def _connect(db_path: str | Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row
    return conn


def init_db(db_path: str | Path) -> None:
    """Create schema if not already present."""
    with _connect(db_path) as conn:
        conn.executescript(_SCHEMA)


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
    with _connect(db_path) as conn:
        cur = conn.execute(
            "INSERT INTO crashes (ts, process, signal, pid, report, dmp_path) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (ts, process, signal, pid, report, dmp_path),
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
        row = conn.execute(
            "SELECT * FROM crashes WHERE id = ?", (crash_id,)
        ).fetchone()
    return dict(row) if row else None


def delete_crash(db_path: str | Path, crash_id: int) -> None:
    """Delete a crash record by id."""
    with _connect(db_path) as conn:
        conn.execute("DELETE FROM crashes WHERE id = ?", (crash_id,))


def get_frequency(db_path: str | Path) -> list[dict]:
    """Return per-process crash counts, sorted by count desc."""
    sql = (
        "SELECT process, COUNT(*) as count FROM crashes "
        "GROUP BY process ORDER BY count DESC"
    )
    with _connect(db_path) as conn:
        rows = conn.execute(sql).fetchall()
    return [dict(r) for r in rows]
