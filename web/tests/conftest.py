"""Pytest fixtures for crashomon-web tests."""

import pytest


@pytest.fixture
def app():
    """Create a Flask test application."""
    # Import deferred until app.py is implemented (Phase 4).
    # from web.app import create_app
    # app = create_app(testing=True)
    # yield app
    pytest.skip("app.py not yet implemented (Phase 4)")


@pytest.fixture
def client(app):
    """Flask test client."""
    return app.test_client()


@pytest.fixture
def tmp_symbol_store(tmp_path):
    """Empty symbol store in a temp directory."""
    store = tmp_path / "symbols"
    store.mkdir()
    return store


@pytest.fixture
def tmp_db(tmp_path):
    """Empty SQLite database path in a temp directory."""
    return tmp_path / "crashes.db"
