"""CSRF enforcement tests.

These tests run with CSRF enabled (no WTF_CSRF_ENABLED=False override) to verify
that the protection is actually enforced at the HTTP level, not just declared.

Strategy for each route:
  1. Reject — POST without any token must return 400.
  2. Reject — POST with a forged token must return 400.
  3. Accept — GET the form page, extract the embedded token, POST with it → success.

API routes (csrf.exempt) are also verified: they must NOT require a CSRF token.
"""

from __future__ import annotations

import io
import re
import textwrap

import pytest

from web import models, symbol_store
from web.app import create_app

SAMPLE_SYM = textwrap.dedent("""\
    MODULE Linux x86_64 AABBCCDD0011223344556677889900AA0 my_service
    FILE 0 src/main.cpp
    FUNC 1a0 20 0 main
    1a0 10 42 0
""")


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def csrf_app(tmp_path):
    """App with CSRF fully enabled."""
    a = create_app(
        symbol_store_path=str(tmp_path / "symbols"),
        db_path=str(tmp_path / "crashes.db"),
        testing=True,
    )
    a.config["SECRET_KEY"] = "stable-test-secret-do-not-use-in-production"
    a.config["WTF_CSRF_TIME_LIMIT"] = None  # no time limit so token never expires mid-test
    return a


@pytest.fixture
def csrf_client(csrf_app):
    return csrf_app.test_client()


def _token_from_html(html: bytes) -> str:
    """Extract the CSRF token value from a rendered form page."""
    m = re.search(rb'name="csrf_token"\s+value="([^"]+)"', html)
    assert m, "CSRF hidden input not found in page HTML"
    return m.group(1).decode()


# ---------------------------------------------------------------------------
# Missing token → 400
# ---------------------------------------------------------------------------


def test_upload_crash_without_csrf_rejected(csrf_client):
    resp = csrf_client.post("/crashes/upload")
    assert resp.status_code == 400


def test_delete_crash_without_csrf_rejected(csrf_client, csrf_app):
    crash_id = models.insert_crash(
        csrf_app.config["DB_PATH"],
        ts="2026-01-01T00:00:00Z",
        process="svc",
        signal="SIGSEGV",
        report="r",
    )
    resp = csrf_client.post(f"/crashes/{crash_id}/delete")
    assert resp.status_code == 400


def test_upload_symbols_without_csrf_rejected(csrf_client):
    resp = csrf_client.post("/symbols/upload")
    assert resp.status_code == 400


def test_delete_symbol_without_csrf_rejected(csrf_client):
    resp = csrf_client.post("/symbols/no_module/DEADBEEF/delete")
    assert resp.status_code == 400


# ---------------------------------------------------------------------------
# Forged token → 400
# ---------------------------------------------------------------------------


def test_upload_crash_with_forged_token_rejected(csrf_client):
    resp = csrf_client.post("/crashes/upload", data={"csrf_token": "forged-token"})
    assert resp.status_code == 400


def test_delete_crash_with_forged_token_rejected(csrf_client, csrf_app):
    crash_id = models.insert_crash(
        csrf_app.config["DB_PATH"],
        ts="2026-01-01T00:00:00Z",
        process="svc",
        signal="SIGSEGV",
        report="r",
    )
    resp = csrf_client.post(f"/crashes/{crash_id}/delete", data={"csrf_token": "x" * 64})
    assert resp.status_code == 400


def test_delete_symbol_with_forged_token_rejected(csrf_client):
    resp = csrf_client.post(
        "/symbols/no_module/DEADBEEF/delete", data={"csrf_token": "x" * 64}
    )
    assert resp.status_code == 400


# ---------------------------------------------------------------------------
# Full browser-equivalent flow: GET form → extract token → POST → success
# ---------------------------------------------------------------------------


def test_upload_crash_with_valid_token_accepted(csrf_client):
    # GET the upload form — server sets session cookie + renders token in HTML
    form_resp = csrf_client.get("/upload")
    assert form_resp.status_code == 200
    token = _token_from_html(form_resp.data)

    # POST with the token (no file provided → redirect back to upload, not an error)
    resp = csrf_client.post("/crashes/upload", data={"csrf_token": token})
    assert resp.status_code in (302, 303)


def test_delete_crash_with_valid_token_accepted(csrf_client, csrf_app):
    crash_id = models.insert_crash(
        csrf_app.config["DB_PATH"],
        ts="2026-01-01T00:00:00Z",
        process="svc",
        signal="SIGSEGV",
        report="r",
    )
    # GET the crash detail page to obtain the token embedded in the Delete form
    form_resp = csrf_client.get(f"/crashes/{crash_id}")
    assert form_resp.status_code == 200
    token = _token_from_html(form_resp.data)

    resp = csrf_client.post(f"/crashes/{crash_id}/delete", data={"csrf_token": token})
    assert resp.status_code in (302, 303)
    assert models.get_crash(csrf_app.config["DB_PATH"], crash_id) is None


def test_delete_symbol_with_valid_token_accepted(csrf_client, csrf_app):
    symbol_store.add_sym_text(csrf_app.config["SYMBOL_STORE"], SAMPLE_SYM)

    # GET the symbols page to obtain the token embedded in the remove form
    form_resp = csrf_client.get("/symbols")
    assert form_resp.status_code == 200
    token = _token_from_html(form_resp.data)

    resp = csrf_client.post(
        "/symbols/my_service/AABBCCDD0011223344556677889900AA0/delete",
        data={"csrf_token": token},
    )
    assert resp.status_code in (302, 303)
    assert (
        symbol_store.find_sym(
            csrf_app.config["SYMBOL_STORE"], "my_service", "AABBCCDD0011223344556677889900AA0"
        )
        is None
    )


def test_upload_symbols_with_valid_token_accepted(csrf_client):
    form_resp = csrf_client.get("/symbols")
    assert form_resp.status_code == 200
    token = _token_from_html(form_resp.data)

    # POST with no file → redirect back (no error, just nothing uploaded)
    resp = csrf_client.post("/symbols/upload", data={"csrf_token": token})
    assert resp.status_code in (302, 303)


# ---------------------------------------------------------------------------
# GET requests never need a CSRF token
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("url", ["/", "/crashes", "/upload", "/symbols", "/groups"])
def test_get_routes_never_require_csrf(csrf_client, url):
    assert csrf_client.get(url).status_code == 200


# ---------------------------------------------------------------------------
# API endpoints are csrf.exempt — no token required
# ---------------------------------------------------------------------------


def test_api_upload_symbols_is_csrf_exempt(csrf_client):
    resp = csrf_client.post(
        "/api/symbols/upload",
        data={"sym": (io.BytesIO(SAMPLE_SYM.encode()), "my_service.sym")},
        content_type="multipart/form-data",
    )
    assert resp.status_code == 201


def test_api_upload_crash_is_csrf_exempt(csrf_client):
    resp = csrf_client.post(
        "/api/crashes/upload",
        data={"minidump": (io.BytesIO(b"MDMP" + b"\x00" * 100), "crash.dmp")},
        content_type="multipart/form-data",
    )
    assert resp.status_code == 201


# ---------------------------------------------------------------------------
# Token is bound to session — a token from a different session is rejected
# ---------------------------------------------------------------------------


def test_token_from_different_session_rejected(csrf_app):
    """A CSRF token from one session must not be accepted by another session."""
    client_a = csrf_app.test_client()
    client_b = csrf_app.test_client()

    # client_a gets a token
    resp_a = client_a.get("/upload")
    token_a = _token_from_html(resp_a.data)

    # client_b tries to use client_a's token (cross-session reuse)
    resp = client_b.post("/crashes/upload", data={"csrf_token": token_a})
    assert resp.status_code == 400
