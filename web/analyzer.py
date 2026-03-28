"""Subprocess wrappers for crash symbolication tools."""

from __future__ import annotations

import subprocess
from pathlib import Path


def analyze_minidump(
    store_path: str | Path,
    dmp_path: str | Path,
    *,
    stackwalk: str = "minidump_stackwalk",
) -> str:
    """Run minidump_stackwalk against a symbol store and return the output.

    Raises RuntimeError on failure (binary not found, timeout, no output).
    """
    try:
        result = subprocess.run(
            [stackwalk, str(dmp_path), str(store_path)],
            capture_output=True,
            text=True,
            timeout=60,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(f"minidump_stackwalk not found: {stackwalk!r}") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("minidump_stackwalk timed out") from exc

    if not result.stdout:
        raise RuntimeError(
            f"minidump_stackwalk produced no output (exit {result.returncode})"
        )
    return result.stdout


def analyze_tombstone_text(
    text: str,
    *,
    store_path: str | Path | None = None,
    addr2line: str = "eu-addr2line",
    analyze_binary: str = "crashomon-analyze",
) -> str:
    """Symbolicate pasted tombstone text via crashomon-analyze --stdin.

    Falls back to returning the original text if the binary is not found
    or times out — symbolication is best-effort.
    """
    store = str(store_path) if store_path is not None else "/tmp"
    cmd = [
        analyze_binary,
        f"--store={store}",
        f"--addr2line-binary={addr2line}",
        "--stdin",
    ]
    try:
        result = subprocess.run(
            cmd,
            input=text,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return text
    return result.stdout if result.stdout else text
