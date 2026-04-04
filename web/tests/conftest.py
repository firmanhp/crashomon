"""Shared pytest fixtures for crashomon-web tests."""

from __future__ import annotations

import pytest

from web.app import create_app
from web import models


@pytest.fixture
def app(tmp_path):
    a = create_app(
        symbol_store_path=str(tmp_path / "symbols"),
        db_path=str(tmp_path / "crashes.db"),
        testing=True,
    )
    # Disable CSRF in tests — forms don't carry CSRF tokens.
    a.config["WTF_CSRF_ENABLED"] = False
    return a


@pytest.fixture
def client(app):
    return app.test_client()


@pytest.fixture
def tmp_symbol_store(tmp_path):
    store = tmp_path / "symbols"
    store.mkdir()
    return store


@pytest.fixture
def tmp_db(tmp_path):
    db = tmp_path / "crashes.db"
    models.init_db(db)
    return db
