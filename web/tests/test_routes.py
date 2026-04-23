"""Integration tests for Flask routes."""

from __future__ import annotations

import io
import textwrap

from web import models, symbol_store

SAMPLE_SYM = textwrap.dedent("""\
    MODULE Linux x86_64 AABBCCDD0011223344556677889900AA0 my_service
    FILE 0 src/main.cpp
    FUNC 1a0 20 0 main
    1a0 10 42 0
""")


# ---------------------------------------------------------------------------
# Dashboard
# ---------------------------------------------------------------------------


def test_dashboard_ok(client):
    assert client.get("/").status_code == 200


def test_dashboard_contains_nav(client):
    resp = client.get("/")
    assert b"crashomon" in resp.data


def test_dashboard_empty_state(client):
    resp = client.get("/")
    assert b"No crashes" in resp.data


def test_dashboard_shows_frequency(client, app):
    for _ in range(2):
        models.insert_crash(
            app.config["DB_PATH"],
            ts="2026-03-28T10:00:00Z",
            process="svc_a",
            signal="SIGSEGV",
            report="r",
        )
    resp = client.get("/")
    assert b"svc_a" in resp.data


# ---------------------------------------------------------------------------
# Groups
# ---------------------------------------------------------------------------


def test_groups_page_ok(client):
    assert client.get("/groups").status_code == 200


def test_groups_page_empty_state(client):
    resp = client.get("/groups")
    assert b"No crash groups" in resp.data


def test_groups_page_shows_group(client, app):
    report = (
        "backtrace:\n"
        " 0  /usr/bin/svc + 0x1234\n"
        " 1  /usr/lib/liba.so + 0xabcd\n"
        " 2  /usr/lib/libb.so + 0x5678\n"
    )
    models.insert_crash(
        app.config["DB_PATH"],
        ts="2026-03-28T10:00:00Z",
        process="svc",
        signal="SIGSEGV",
        report=report,
    )
    resp = client.get("/groups")
    assert b"svc" in resp.data
    assert b"SIGSEGV" in resp.data


# ---------------------------------------------------------------------------
# Crashes list
# ---------------------------------------------------------------------------


def test_crashes_list_ok(client):
    assert client.get("/crashes").status_code == 200


def test_crashes_list_empty(client):
    assert b"No crashes" in client.get("/crashes").data


def test_crashes_list_shows_entry(client, app):
    models.insert_crash(
        app.config["DB_PATH"],
        ts="2026-03-28T10:00:00Z",
        process="svc_a",
        signal="SIGSEGV",
        report="r",
    )
    assert b"svc_a" in client.get("/crashes").data


def test_crashes_filter_by_process(client, app):
    db = app.config["DB_PATH"]
    models.insert_crash(
        db, ts="2026-03-28T10:00:00Z", process="svc_a", signal="SIGSEGV", report="r"
    )
    models.insert_crash(
        db, ts="2026-03-28T11:00:00Z", process="svc_b", signal="SIGABRT", report="r"
    )
    resp = client.get("/crashes?process=svc_a")
    assert b"svc_a" in resp.data
    assert b"svc_b" not in resp.data


def test_crashes_filter_by_signal(client, app):
    db = app.config["DB_PATH"]
    models.insert_crash(
        db, ts="2026-03-28T10:00:00Z", process="svc_a", signal="SIGSEGV", report="r"
    )
    models.insert_crash(
        db, ts="2026-03-28T11:00:00Z", process="svc_b", signal="SIGABRT", report="r"
    )
    resp = client.get("/crashes?signal=SIGABRT")
    assert b"SIGABRT" in resp.data
    assert b"SIGSEGV" not in resp.data


def test_crashes_pagination_page_param(client, app):
    for i in range(30):
        models.insert_crash(
            app.config["DB_PATH"],
            ts=f"2026-03-28T{i // 24:02d}:{i % 60:02d}:00Z",
            process="svc",
            signal="SIGSEGV",
            report="r",
        )
    assert client.get("/crashes?page=2").status_code == 200


# ---------------------------------------------------------------------------
# Crash detail
# ---------------------------------------------------------------------------


def test_crash_detail_ok(client, app):
    crash_id = models.insert_crash(
        app.config["DB_PATH"],
        ts="2026-03-28T10:00:00Z",
        process="svc",
        signal="SIGSEGV",
        report="the report text",
    )
    resp = client.get(f"/crashes/{crash_id}")
    assert resp.status_code == 200
    assert b"the report text" in resp.data


def test_crash_detail_404(client):
    assert client.get("/crashes/99999").status_code == 404


def test_delete_crash_post(client, app):
    crash_id = models.insert_crash(
        app.config["DB_PATH"],
        ts="2026-03-28T10:00:00Z",
        process="svc",
        signal="SIGSEGV",
        report="r",
    )
    resp = client.post(f"/crashes/{crash_id}/delete")
    assert resp.status_code in (302, 303)
    assert models.get_crash(app.config["DB_PATH"], crash_id) is None


# ---------------------------------------------------------------------------
# Upload form
# ---------------------------------------------------------------------------


def test_upload_form_ok(client):
    assert client.get("/upload").status_code == 200


def test_upload_empty_redirects(client):
    resp = client.post("/crashes/upload", data={"tombstone": "   "})
    assert resp.status_code in (302, 303)
    # No crash stored.
    assert models.get_crashes


# ---------------------------------------------------------------------------
# Symbols
# ---------------------------------------------------------------------------


def test_symbols_page_ok(client):
    assert client.get("/symbols").status_code == 200


def test_symbols_page_empty(client):
    assert b"No symbols" in client.get("/symbols").data


def test_symbols_page_shows_entry(client, app):
    symbol_store.add_sym_text(app.config["SYMBOL_STORE"], SAMPLE_SYM)
    assert b"my_service" in client.get("/symbols").data


def test_delete_symbol_via_http_delete(client, app):
    symbol_store.add_sym_text(app.config["SYMBOL_STORE"], SAMPLE_SYM)
    resp = client.delete("/symbols/my_service/AABBCCDD0011223344556677889900AA0")
    assert resp.status_code == 204
    assert (
        symbol_store.find_sym(
            app.config["SYMBOL_STORE"], "my_service", "AABBCCDD0011223344556677889900AA0"
        )
        is None
    )


def test_delete_symbol_nonexistent_is_ok(client):
    resp = client.delete("/symbols/no_module/DEADBEEF")
    assert resp.status_code == 204


# ---------------------------------------------------------------------------
# Tecken symbol server: /symbols/<module>/<build_id>/<filename>
# ---------------------------------------------------------------------------


def test_serve_symbol_returns_sym_content(client, app):
    symbol_store.add_sym_text(app.config["SYMBOL_STORE"], SAMPLE_SYM)
    resp = client.get("/symbols/my_service/AABBCCDD0011223344556677889900AA0/my_service.sym")
    assert resp.status_code == 200
    assert b"MODULE Linux x86_64" in resp.data


def test_serve_symbol_missing_returns_404(client):
    resp = client.get("/symbols/no_module/DEADBEEF/no_module.sym")
    assert resp.status_code == 404


def test_serve_symbol_path_traversal_rejected(client):
    resp = client.get("/symbols/../../../etc/passwd/x/x.sym")
    assert resp.status_code in (400, 404)


# ---------------------------------------------------------------------------
# API: /api/symbols/upload
# ---------------------------------------------------------------------------


def test_api_upload_sym_file_ok(client):
    resp = client.post(
        "/api/symbols/upload",
        data={"sym": (io.BytesIO(SAMPLE_SYM.encode()), "my_service.sym")},
        content_type="multipart/form-data",
    )
    assert resp.status_code == 201
    data = resp.get_json()
    assert data["status"] == "ok"
    assert "my_service" in data["stored"]


def test_api_upload_sym_file_invalid(client):
    resp = client.post(
        "/api/symbols/upload",
        data={"sym": (io.BytesIO(b"garbage"), "bad.sym")},
        content_type="multipart/form-data",
    )
    assert resp.status_code == 400
    assert resp.get_json()["status"] == "error"


def test_api_upload_symbols_no_file(client):
    resp = client.post("/api/symbols/upload")
    assert resp.status_code == 400


# ---------------------------------------------------------------------------
# API: /api/crashes/upload
# ---------------------------------------------------------------------------


def test_api_upload_crash_no_file(client):
    resp = client.post("/api/crashes/upload")
    assert resp.status_code == 400
    assert resp.get_json()["status"] == "error"


def test_api_upload_crash_with_file(client, app, tmp_path):
    # minidump_stackwalk is not available in test, so the report will contain
    # the failure message — but the crash must still be stored.
    dummy_dmp = tmp_path / "crash.dmp"
    dummy_dmp.write_bytes(b"MDMP" + b"\x00" * 100)
    resp = client.post(
        "/api/crashes/upload",
        data={"minidump": (io.BytesIO(dummy_dmp.read_bytes()), "crash.dmp")},
        content_type="multipart/form-data",
    )
    assert resp.status_code == 201
    data = resp.get_json()
    assert data["status"] == "ok"
    assert isinstance(data["crash_id"], int)
    # Crash must be in the database.
    assert models.get_crash(app.config["DB_PATH"], data["crash_id"]) is not None
