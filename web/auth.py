"""Optional authentication for crashomon-web.

Controlled entirely via environment variables — when unset, no auth is applied
and the app behaves exactly as before.

  CRASHOMON_API_KEY        — if set, /api/* routes require the header
                             X-API-Key: <value>
  CRASHOMON_AUTH_USER      — if set (together with CRASHOMON_AUTH_PASS),
  CRASHOMON_AUTH_PASS        HTTP Basic Auth is required for all non-/api/* routes
"""

from __future__ import annotations

import base64
import os
import secrets

from flask import Flask, Response, request


def init_auth(app: Flask) -> None:
    """Register a before_request hook that enforces auth when env vars are set."""
    api_key = os.environ.get("CRASHOMON_API_KEY", "")
    basic_user = os.environ.get("CRASHOMON_AUTH_USER", "")
    basic_pass = os.environ.get("CRASHOMON_AUTH_PASS", "")

    if not api_key and not (basic_user and basic_pass):
        return  # nothing configured — no-op

    @app.before_request
    def _check_auth() -> Response | None:  # type: ignore[return]
        path = request.path

        if path.startswith("/api/"):
            if api_key:
                provided = request.headers.get("X-API-Key", "")
                if not secrets.compare_digest(provided, api_key):
                    return Response(
                        '{"status":"error","message":"unauthorized"}',
                        status=401,
                        mimetype="application/json",
                    )
            return None

        if basic_user and basic_pass:
            auth_header = request.headers.get("Authorization", "")
            if not _check_basic(auth_header, basic_user, basic_pass):
                return Response(
                    "Authentication required",
                    status=401,
                    headers={"WWW-Authenticate": 'Basic realm="crashomon"'},
                )

        return None


def _check_basic(header: str, expected_user: str, expected_pass: str) -> bool:
    if not header.startswith("Basic "):
        return False
    try:
        decoded = base64.b64decode(header[6:]).decode("utf-8", errors="replace")
    except Exception:
        return False
    colon = decoded.find(":")
    if colon < 0:
        return False
    user = decoded[:colon]
    password = decoded[colon + 1 :]
    user_ok = secrets.compare_digest(user, expected_user)
    pass_ok = secrets.compare_digest(password, expected_pass)
    return user_ok and pass_ok
