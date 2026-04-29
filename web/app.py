"""crashomon-web — Flask application factory."""

from __future__ import annotations

import datetime
import os
import re
import secrets
import shutil
from pathlib import Path

from flask import Flask, abort, flash, redirect, render_template, request, send_file, url_for
from flask_wtf.csrf import CSRFProtect
from werkzeug.utils import secure_filename

from web import analyzer, auth, models, symbol_store


def create_app(
    *,
    symbol_store_path: str | None = None,
    db_path: str | None = None,
    dump_syms: str = "dump_syms",
    stackwalk: str = "minidump-stackwalk",
    testing: bool = False,
) -> Flask:
    """Create and configure the Flask application."""
    app = Flask(__name__, template_folder="templates")
    app.config["TESTING"] = testing
    app.config["SECRET_KEY"] = os.environ.get("SECRET_KEY", secrets.token_hex(32))
    csrf = CSRFProtect(app)
    app.config["SYMBOL_STORE"] = Path(
        symbol_store_path or os.environ.get("CRASHOMON_SYMBOL_STORE", "/var/crashomon/symbols")
    )
    app.config["DB_PATH"] = Path(
        db_path or os.environ.get("CRASHOMON_DB", "/var/crashomon/crashes.db")
    )
    app.config["DUMP_SYMS"] = dump_syms
    app.config["STACKWALK"] = stackwalk

    if not testing:
        app.config["SYMBOL_STORE"].mkdir(parents=True, exist_ok=True)
        app.config["DB_PATH"].parent.mkdir(parents=True, exist_ok=True)
    models.init_db(app.config["DB_PATH"])
    auth.init_auth(app)

    # ── Routes ────────────────────────────────────────────────────────────────

    @app.get("/")
    def dashboard():
        frequency = models.get_frequency(app.config["DB_PATH"])
        recent = models.get_crashes(app.config["DB_PATH"], limit=10)
        crash_groups = models.get_crash_groups(app.config["DB_PATH"])
        daily_counts = models.get_daily_counts(app.config["DB_PATH"])
        return render_template(
            "dashboard.html",
            frequency=frequency,
            recent=recent,
            crash_groups=crash_groups,
            daily_counts=daily_counts,
        )

    @app.get("/groups")
    def groups():
        crash_groups = models.get_crash_groups(app.config["DB_PATH"])
        return render_template("groups.html", crash_groups=crash_groups)

    @app.get("/crashes")
    def crashes():
        process = request.args.get("process", "")
        signal = request.args.get("signal", "")
        try:
            page = max(1, int(request.args.get("page", 1)))
        except ValueError:
            page = 1
        per_page = 25
        offset = (page - 1) * per_page
        rows = models.get_crashes(
            app.config["DB_PATH"],
            process=process,
            signal=signal,
            limit=per_page + 1,
            offset=offset,
        )
        has_next = len(rows) > per_page
        return render_template(
            "crashes.html",
            crashes=rows[:per_page],
            page=page,
            has_next=has_next,
            process=process,
            signal=signal,
        )

    @app.get("/crashes/<int:crash_id>")
    def crash_detail(crash_id: int):
        crash = models.get_crash(app.config["DB_PATH"], crash_id)
        if crash is None:
            abort(404)
        return render_template("crash_detail.html", crash=crash)

    @app.post("/crashes/<int:crash_id>/delete")
    def delete_crash(crash_id: int):
        models.delete_crash(app.config["DB_PATH"], crash_id)
        return redirect(url_for("crashes"))

    @app.get("/upload")
    def upload_form():
        return render_template("upload.html")

    @app.post("/crashes/upload")
    def upload_crash():
        """Handle minidump file upload or tombstone text paste."""
        store = app.config["SYMBOL_STORE"]
        ts = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")

        dmp_file = request.files.get("minidump")
        if dmp_file and dmp_file.filename:
            safe_name = secure_filename(dmp_file.filename)
            if not safe_name:
                flash("Invalid filename.", "error")
                return redirect(url_for("upload_form"))
            tmp_dir = app.config["DB_PATH"].parent / "tmp"
            tmp_dir.mkdir(parents=True, exist_ok=True)
            dmp_path = tmp_dir / safe_name
            dmp_file.save(str(dmp_path))
            try:
                report = analyzer.analyze_minidump(
                    store, dmp_path, stackwalk=app.config["STACKWALK"]
                )
            except RuntimeError as exc:
                report = f"(symbolication failed: {exc})\n"
            process, sig = _extract_meta(report)
            crash_id = models.insert_crash(
                app.config["DB_PATH"],
                ts=ts,
                process=process,
                signal=sig,
                report=report,
                dmp_path=str(dmp_path),
            )
            return redirect(url_for("crash_detail", crash_id=crash_id))

        flash("No file selected.", "error")
        return redirect(url_for("upload_form"))

    @app.get("/symbols")
    def symbols():
        entries = symbol_store.list_symbols(app.config["SYMBOL_STORE"])
        return render_template("symbols.html", entries=entries)

    @app.post("/symbols/upload")
    def upload_symbols():
        """Accept a symbol archive or debug binary; store in symbol store."""
        arc = request.files.get("archive")
        if arc and arc.filename:
            safe_name = secure_filename(arc.filename)
            if not safe_name:
                flash("Invalid filename.", "error")
                return redirect(url_for("symbols"))
            tmp_dir = app.config["DB_PATH"].parent / "tmp"
            tmp_dir.mkdir(parents=True, exist_ok=True)
            arc_path = tmp_dir / safe_name
            arc.save(str(arc_path))
            try:
                stored, errors = symbol_store.add_archive(app.config["SYMBOL_STORE"], arc_path)
                if stored:
                    flash(f"Imported {len(stored)} symbol(s).", "success")
                for err in errors:
                    flash(err, "error")
            except (ValueError, RuntimeError) as exc:
                flash(str(exc), "error")
            finally:
                arc_path.unlink(missing_ok=True)
            return redirect(url_for("symbols"))

        f = request.files.get("binary")
        if f and f.filename:
            safe_name = secure_filename(f.filename)
            if not safe_name:
                flash("Invalid filename.", "error")
                return redirect(url_for("symbols"))
            tmp_dir = app.config["DB_PATH"].parent / "tmp"
            tmp_dir.mkdir(parents=True, exist_ok=True)
            bin_path = tmp_dir / safe_name
            f.save(str(bin_path))
            try:
                symbol_store.add_binary(
                    app.config["SYMBOL_STORE"],
                    bin_path,
                    dump_syms=app.config["DUMP_SYMS"],
                )
            except (RuntimeError, FileNotFoundError) as exc:
                flash(f"Symbol extraction failed: {exc}", "error")
            finally:
                bin_path.unlink(missing_ok=True)
        return redirect(url_for("symbols"))

    @app.post("/symbols/<module>/<build_id>/delete")
    def delete_symbol(module: str, build_id: str):
        """Remove a specific symbol version."""
        store_root = app.config["SYMBOL_STORE"].resolve()
        sym_dir = (app.config["SYMBOL_STORE"] / module / build_id).resolve()
        if str(sym_dir).startswith(str(store_root) + "/") and sym_dir.is_dir():
            shutil.rmtree(sym_dir)
        return redirect(url_for("symbols"))

    @app.get("/symbols/<module>/<build_id>/<filename>")
    def serve_symbol(module: str, build_id: str, filename: str) -> object:
        """Tecken-protocol symbol file endpoint.

        Serves Breakpad .sym files at the path layout expected by
        minidump-stackwalk --symbols-url.
        """
        store = app.config["SYMBOL_STORE"]
        candidate = (store / module / build_id / filename).resolve()
        if not str(candidate).startswith(str(store.resolve())):
            abort(400)
        if not candidate.is_file():
            abort(404)
        return send_file(candidate, mimetype="text/plain")

    @app.post("/api/symbols/upload")
    @csrf.exempt
    def api_upload_symbols():
        """CI/CD REST endpoint: POST multipart with 'archive', 'sym', or 'binary' field."""
        arc = request.files.get("archive")
        if arc and arc.filename:
            safe_name = secure_filename(arc.filename)
            if not safe_name:
                return {"status": "error", "message": "invalid filename"}, 400
            tmp_dir = app.config["DB_PATH"].parent / "tmp"
            tmp_dir.mkdir(parents=True, exist_ok=True)
            arc_path = tmp_dir / safe_name
            arc.save(str(arc_path))
            try:
                stored, errors = symbol_store.add_archive(app.config["SYMBOL_STORE"], arc_path)
                rels = [str(p.relative_to(app.config["SYMBOL_STORE"])) for p in stored]
                return {"status": "ok", "stored": rels, "errors": errors}, 201
            except ValueError as exc:
                return {"status": "error", "message": str(exc)}, 400
            except RuntimeError as exc:
                return {"status": "error", "message": str(exc)}, 500
            finally:
                arc_path.unlink(missing_ok=True)

        sym_file = request.files.get("sym")
        if sym_file:
            text = sym_file.read().decode("utf-8", errors="replace")
            try:
                dest = symbol_store.add_sym_text(app.config["SYMBOL_STORE"], text)
                rel = str(dest.relative_to(app.config["SYMBOL_STORE"]))
                return {"status": "ok", "stored": rel}, 201
            except ValueError as exc:
                return {"status": "error", "message": str(exc)}, 400

        binary = request.files.get("binary")
        if binary and binary.filename:
            safe_name = secure_filename(binary.filename)
            if not safe_name:
                return {"status": "error", "message": "invalid filename"}, 400
            tmp_dir = app.config["DB_PATH"].parent / "tmp"
            tmp_dir.mkdir(parents=True, exist_ok=True)
            bin_path = tmp_dir / safe_name
            binary.save(str(bin_path))
            try:
                dest = symbol_store.add_binary(
                    app.config["SYMBOL_STORE"], bin_path, dump_syms=app.config["DUMP_SYMS"]
                )
                rel = str(dest.relative_to(app.config["SYMBOL_STORE"]))
                return {"status": "ok", "stored": rel}, 201
            except (RuntimeError, FileNotFoundError) as exc:
                return {"status": "error", "message": str(exc)}, 500
            finally:
                bin_path.unlink(missing_ok=True)

        return {"status": "error", "message": "no file provided"}, 400

    @app.post("/api/crashes/upload")
    @csrf.exempt
    def api_upload_crash():
        """REST endpoint: POST with 'minidump' file, returns crash id."""
        dmp_file = request.files.get("minidump")
        if not dmp_file or not dmp_file.filename:
            return {"status": "error", "message": "no minidump provided"}, 400

        safe_name = secure_filename(dmp_file.filename)
        if not safe_name:
            return {"status": "error", "message": "invalid filename"}, 400
        tmp_dir = app.config["DB_PATH"].parent / "tmp"
        tmp_dir.mkdir(parents=True, exist_ok=True)
        dmp_path = tmp_dir / safe_name
        dmp_file.save(str(dmp_path))

        ts = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
        try:
            report = analyzer.analyze_minidump(
                app.config["SYMBOL_STORE"], dmp_path, stackwalk=app.config["STACKWALK"]
            )
        except RuntimeError as exc:
            report = f"(symbolication failed: {exc})\n"

        process, sig = _extract_meta(report)
        crash_id = models.insert_crash(
            app.config["DB_PATH"],
            ts=ts,
            process=process,
            signal=sig,
            report=report,
            dmp_path=str(dmp_path),
        )
        return {"status": "ok", "crash_id": crash_id}, 201

    return app


def _extract_meta(report: str) -> tuple[str, str]:
    """Extract (process_name, signal_name) from a symbolicated report."""
    process = "unknown"
    signal = "unknown"
    for line in report.splitlines():
        m = re.search(r">>>\s*(\S+)\s*<<<", line)
        if m:
            process = m.group(1)
        m = re.search(r"signal\s+\d+\s+\(([^)]+)\)", line)
        if m:
            signal = m.group(1)
    return process, signal
