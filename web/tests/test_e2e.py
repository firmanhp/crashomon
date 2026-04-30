"""End-to-end browser tests using Playwright.

These tests exercise the CSRF flow as a real browser would: the browser submits
the form, which automatically includes the CSRF token injected into the hidden
input by the server. No manual token extraction is needed.

Requirements (not in default dev deps):
    uv pip install "playwright>=1.40" "pytest-playwright>=0.4"
    playwright install chromium

Run:
    uv run pytest -m e2e web/tests/test_e2e.py

The entire file is skipped when playwright is not installed so it never breaks
the default test run.
"""

from __future__ import annotations

import io
import textwrap
import threading
import zipfile

import pytest

# Skip the entire module if playwright is not installed.
playwright_mod = pytest.importorskip(
    "playwright",
    reason="playwright not installed — run: uv pip install 'playwright>=1.40' 'pytest-playwright>=0.4' && playwright install chromium",
)

from werkzeug.serving import make_server  # noqa: E402  (after importorskip guard)

from web import models, symbol_store  # noqa: E402
from web.app import create_app  # noqa: E402

pytestmark = pytest.mark.e2e

SAMPLE_SYM = textwrap.dedent("""\
    MODULE Linux x86_64 AABBCCDD0011223344556677889900AA0 e2e_service
    FILE 0 src/main.cpp
    FUNC 1a0 20 0 main
    1a0 10 42 0
""")

_E2E_PORT = 15432


# ---------------------------------------------------------------------------
# Live server fixture
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def live_server(tmp_path_factory):
    """Start a real Flask server for the browser to hit."""
    tmp = tmp_path_factory.mktemp("e2e")
    app = create_app(
        symbol_store_path=str(tmp / "symbols"),
        db_path=str(tmp / "crashes.db"),
    )
    app.config["SECRET_KEY"] = "e2e-stable-secret-do-not-use-in-production"

    server = make_server("127.0.0.1", _E2E_PORT, app)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield app, f"http://127.0.0.1:{_E2E_PORT}"
    server.shutdown()


@pytest.fixture
def server_url(live_server):
    _, url = live_server
    return url


@pytest.fixture
def app_and_url(live_server):
    return live_server


# ---------------------------------------------------------------------------
# Browser tests
# ---------------------------------------------------------------------------


def test_e2e_dashboard_loads(page, server_url):
    """Browser can reach the dashboard."""
    page.goto(server_url)
    assert "crashomon" in page.title().lower()


def test_e2e_upload_form_has_csrf_token(page, server_url):
    """The upload form renders a non-empty CSRF hidden input."""
    page.goto(f"{server_url}/upload")
    token = page.get_attribute('input[name="csrf_token"]', "value")
    assert token and len(token) > 10


def test_e2e_delete_crash_via_browser(page, app_and_url):  # noqa: F811
    """Clicking Delete on crash detail submits the CSRF-protected POST and deletes the crash."""
    app, base_url = app_and_url
    crash_id = models.insert_crash(
        app.config["DB_PATH"],
        ts="2026-01-01T00:00:00Z",
        process="e2e_svc",
        signal="SIGSEGV",
        report="e2e crash report",
    )

    page.goto(f"{base_url}/crashes/{crash_id}")
    page.click("button:has-text('Delete')")
    page.wait_for_url(f"{base_url}/crashes")

    assert models.get_crash(app.config["DB_PATH"], crash_id) is None


def test_e2e_delete_symbol_via_browser(page, app_and_url):
    """Clicking remove on the symbols page submits the CSRF-protected POST and removes the symbol."""
    app, base_url = app_and_url
    symbol_store.add_sym_text(app.config["SYMBOL_STORE"], SAMPLE_SYM)

    page.goto(f"{base_url}/symbols")
    page.click("button:has-text('remove')")
    page.wait_for_url(f"{base_url}/symbols")

    assert (
        symbol_store.find_sym(
            app.config["SYMBOL_STORE"], "e2e_service", "AABBCCDD0011223344556677889900AA0"
        )
        is None
    )


def test_e2e_csrf_blocks_direct_post_without_token(page, server_url):
    """Verify the server returns 400 when a DELETE-crash request has no CSRF token.

    This uses the browser's fetch API to bypass the form, sending a raw POST.
    """
    page.goto(server_url)  # establish a session context

    status = page.evaluate(
        """async () => {
            const resp = await fetch('/crashes/999/delete', {method: 'POST'});
            return resp.status;
        }"""
    )
    assert status == 400


def test_e2e_archive_upload_shows_success_flash(page, app_and_url, tmp_path):
    """Uploading a valid symbol archive shows a success flash message.

    This guards against the multi-worker SECRET_KEY bug where flash messages
    are silently lost when the POST and redirect-GET hit different workers.
    """
    _, base_url = app_and_url

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("e2e_service.sym", SAMPLE_SYM)
    archive = tmp_path / "syms.zip"
    archive.write_bytes(buf.getvalue())

    page.goto(f"{base_url}/symbols")
    page.set_input_files('input[name="archive"]', str(archive))
    page.click("button:has-text('Import archive')")
    page.wait_for_url(f"{base_url}/symbols")

    flash = page.locator("div[style*='padding: 0.4rem']")
    assert flash.count() > 0
    assert "Imported 1 symbol" in flash.first.inner_text()


def test_e2e_archive_upload_invalid_sym_shows_error_flash(page, app_and_url, tmp_path):
    """Uploading an archive whose .sym file has a bad MODULE line shows an error flash."""
    _, base_url = app_and_url

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("bad.sym", "NOT A VALID MODULE LINE\n")
    archive = tmp_path / "bad.zip"
    archive.write_bytes(buf.getvalue())

    page.goto(f"{base_url}/symbols")
    page.set_input_files('input[name="archive"]', str(archive))
    page.click("button:has-text('Import archive')")
    page.wait_for_url(f"{base_url}/symbols")

    flash = page.locator("div[style*='padding: 0.4rem']")
    assert flash.count() > 0
    assert "Invalid MODULE line" in flash.first.inner_text()
