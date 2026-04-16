"""Crash symbolication helpers used by the Flask web app."""

from __future__ import annotations

from pathlib import Path

from tools.analyze.analyze import mode_minidump_store, mode_stdin_store


def analyze_minidump(
    store_path: str | Path,
    dmp_path: str | Path,
    *,
    stackwalk: str = "minidump_stackwalk",
) -> str:
    """Run minidump_stackwalk against a symbol store and return the output.

    Raises RuntimeError on failure (binary not found, timeout, no output).
    """
    return mode_minidump_store(str(store_path), str(dmp_path), stackwalk)


def analyze_tombstone_text(
    text: str,
    *,
    store_path: str | Path | None = None,
    addr2line: str = "eu-addr2line",
    analyze_binary: str = "crashomon-analyze",  # kept for API compatibility
) -> str:
    """Symbolicate pasted tombstone text using eu-addr2line.

    Falls back to returning the original text on parse errors or if
    eu-addr2line is not available — symbolication is best-effort.
    """
    store = str(store_path) if store_path is not None else "/tmp"  # noqa: S108
    try:
        return mode_stdin_store(text, store)
    except (ValueError, RuntimeError):
        return text
