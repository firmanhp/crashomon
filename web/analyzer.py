"""Crash symbolication helpers used by the Flask web app."""

from __future__ import annotations

from pathlib import Path

from tools.analyze.analyze import mode_minidump_store


def analyze_minidump(
    store_path: str | Path,
    dmp_path: str | Path,
    *,
    stackwalk: str = "minidump-stackwalk",
) -> str:
    """Run minidump-stackwalk against a symbol store and return the output.

    Raises RuntimeError on failure (binary not found, timeout, no output).
    """
    return mode_minidump_store(str(store_path), str(dmp_path), stackwalk)
