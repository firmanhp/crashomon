"""Tests for web/auth.py — optional authentication."""

from __future__ import annotations

import base64

import pytest

from web.app import create_app


def _basic_header(user: str, password: str) -> str:
    creds = base64.b64encode(f"{user}:{password}".encode()).decode()
    return f"Basic {creds}"


@pytest.fixture
def app_no_auth(tmp_path):
    """App with no auth env vars set."""
    return create_app(
        symbol_store_path=str(tmp_path / "symbols"),
        db_path=str(tmp_path / "crashes.db"),
        testing=True,
    )


@pytest.fixture
def app_api_key(tmp_path, monkeypatch):
    monkeypatch.setenv("CRASHOMON_API_KEY", "secret-key-123")
    return create_app(
        symbol_store_path=str(tmp_path / "symbols"),
        db_path=str(tmp_path / "crashes.db"),
        testing=True,
    )


@pytest.fixture
def app_basic_auth(tmp_path, monkeypatch):
    monkeypatch.setenv("CRASHOMON_AUTH_USER", "admin")
    monkeypatch.setenv("CRASHOMON_AUTH_PASS", "hunter2")
    return create_app(
        symbol_store_path=str(tmp_path / "symbols"),
        db_path=str(tmp_path / "crashes.db"),
        testing=True,
    )


# ---------------------------------------------------------------------------
# No auth configured — everything accessible
# ---------------------------------------------------------------------------


def test_no_auth_web_route_accessible(app_no_auth):
    with app_no_auth.test_client() as c:
        assert c.get("/").status_code == 200


def test_no_auth_api_route_accessible(app_no_auth):
    with app_no_auth.test_client() as c:
        assert c.post("/api/symbols/upload").status_code == 400  # missing file, not 401


# ---------------------------------------------------------------------------
# API key auth
# ---------------------------------------------------------------------------


def test_api_key_required_when_set(app_api_key):
    with app_api_key.test_client() as c:
        resp = c.post("/api/symbols/upload")
        assert resp.status_code == 401
        assert resp.get_json()["status"] == "error"


def test_api_key_correct_passes(app_api_key):
    with app_api_key.test_client() as c:
        resp = c.post("/api/symbols/upload", headers={"X-API-Key": "secret-key-123"})
        assert resp.status_code == 400  # missing file, not 401


def test_api_key_wrong_rejected(app_api_key):
    with app_api_key.test_client() as c:
        resp = c.post("/api/symbols/upload", headers={"X-API-Key": "wrong"})
        assert resp.status_code == 401


def test_api_key_does_not_affect_web_routes(app_api_key):
    with app_api_key.test_client() as c:
        assert c.get("/").status_code == 200


# ---------------------------------------------------------------------------
# Basic auth
# ---------------------------------------------------------------------------


def test_basic_auth_required_when_set(app_basic_auth):
    with app_basic_auth.test_client() as c:
        resp = c.get("/")
        assert resp.status_code == 401
        assert "WWW-Authenticate" in resp.headers


def test_basic_auth_correct_passes(app_basic_auth):
    with app_basic_auth.test_client() as c:
        resp = c.get("/", headers={"Authorization": _basic_header("admin", "hunter2")})
        assert resp.status_code == 200


def test_basic_auth_wrong_password_rejected(app_basic_auth):
    with app_basic_auth.test_client() as c:
        resp = c.get("/", headers={"Authorization": _basic_header("admin", "wrong")})
        assert resp.status_code == 401


def test_basic_auth_wrong_user_rejected(app_basic_auth):
    with app_basic_auth.test_client() as c:
        resp = c.get("/", headers={"Authorization": _basic_header("hacker", "hunter2")})
        assert resp.status_code == 401


def test_basic_auth_missing_header_rejected(app_basic_auth):
    with app_basic_auth.test_client() as c:
        assert c.get("/crashes").status_code == 401


def test_basic_auth_does_not_affect_api_routes(app_basic_auth):
    """API routes are not protected by basic auth (only by API key)."""
    with app_basic_auth.test_client() as c:
        resp = c.post("/api/symbols/upload")
        assert resp.status_code == 400  # missing file, not 401
