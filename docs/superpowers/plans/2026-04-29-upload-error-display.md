# Upload Error Display Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render Flask flash messages in the web UI so upload errors (invalid filename, no file selected, symbol extraction failures) are visible to users.

**Architecture:** Flask's `flash()` mechanism is already used in `upload_symbols` but `base.html` never calls `get_flashed_messages()`. The `upload_crash` route uses a bare `abort(400)` and a silent redirect instead of `flash()`. Fix both: add flash rendering to the shared template, convert the two `upload_crash` error paths to `flash()` + redirect.

**Tech Stack:** Flask, Jinja2, pytest

---

### Task 1: Write failing tests for `upload_crash` error paths

**Files:**
- Modify: `web/tests/test_routes.py`

- [ ] **Step 1: Add tests to `web/tests/test_routes.py`**

Append these two test functions after `test_upload_empty_redirects` (line 195):

```python
def test_upload_crash_no_file_flashes_error(client):
    resp = client.post("/crashes/upload", data={}, follow_redirects=True)
    assert resp.status_code == 200
    assert b"No file selected" in resp.data


def test_upload_crash_invalid_filename_flashes_error(client):
    resp = client.post(
        "/crashes/upload",
        data={"minidump": (io.BytesIO(b"data"), "@#$%")},
        content_type="multipart/form-data",
        follow_redirects=True,
    )
    assert resp.status_code == 200
    assert b"Invalid filename" in resp.data
```

(`"@#$%"` contains no alphanumeric characters, so `secure_filename` returns `""`.)

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd /data/pub/crashomon && uv run pytest web/tests/test_routes.py::test_upload_crash_no_file_flashes_error web/tests/test_routes.py::test_upload_crash_invalid_filename_flashes_error -v
```

Expected: both FAIL — the redirect currently lands on a page with no flash message rendered.

---

### Task 2: Fix `upload_crash` error paths in `app.py`

**Files:**
- Modify: `web/app.py`

- [ ] **Step 1: Replace `abort(400)` with flash + redirect for invalid filename**

In `web/app.py`, in the `upload_crash` function, replace:

```python
        if not safe_name:
                return abort(400)
```

with:

```python
        if not safe_name:
                flash("Invalid filename.", "error")
                return redirect(url_for("upload_form"))
```

- [ ] **Step 2: Replace silent redirect with flash + redirect for no file**

Still in `upload_crash`, the final line is a silent redirect when no file is in the request. Replace:

```python
        return redirect(url_for("upload_form"))
```

with:

```python
        flash("No file selected.", "error")
        return redirect(url_for("upload_form"))
```

- [ ] **Step 3: Run route tests to verify flash messages are produced**

The tests still fail at this point (flash messages exist but aren't rendered in HTML yet), but confirm the routes no longer 400:

```bash
cd /data/pub/crashomon && uv run pytest web/tests/test_routes.py::test_upload_crash_no_file_flashes_error web/tests/test_routes.py::test_upload_crash_invalid_filename_flashes_error -v 2>&1 | head -40
```

Expected: tests still FAIL, but now with `assert b"No file selected" in resp.data` / `assert b"Invalid filename" in resp.data` (not a 400 error).

---

### Task 3: Render flash messages in `base.html`

**Files:**
- Modify: `web/templates/base.html`

- [ ] **Step 1: Add flash block between `<nav>` and `<main>`**

In `web/templates/base.html`, replace:

```html
  <main>
    {% block content %}{% endblock %}
  </main>
```

with:

```html
  {% with messages = get_flashed_messages(with_categories=true) %}
    {% if messages %}
      <div style="padding: 0.5rem 2rem;">
        {% for category, message in messages %}
          <div style="
            margin-bottom: 0.4rem;
            padding: 0.4rem 0.8rem;
            border: 1px solid {% if category == 'error' %}#7a2a2a{% elif category == 'success' %}#2a7a2a{% else %}#7a5a1a{% endif %};
            background: {% if category == 'error' %}#3a1a1a{% elif category == 'success' %}#1a3a1a{% else %}#2a200a{% endif %};
            color: {% if category == 'error' %}#e07070{% elif category == 'success' %}#7ec87e{% else %}#e0b870{% endif %};
            font-size: 0.9em;
          ">{{ message }}</div>
        {% endfor %}
      </div>
    {% endif %}
  {% endwith %}
  <main>
    {% block content %}{% endblock %}
  </main>
```

- [ ] **Step 2: Run the two new tests — both should pass now**

```bash
cd /data/pub/crashomon && uv run pytest web/tests/test_routes.py::test_upload_crash_no_file_flashes_error web/tests/test_routes.py::test_upload_crash_invalid_filename_flashes_error -v
```

Expected:
```
PASSED web/tests/test_routes.py::test_upload_crash_no_file_flashes_error
PASSED web/tests/test_routes.py::test_upload_crash_invalid_filename_flashes_error
```

- [ ] **Step 3: Run the full test suite to check for regressions**

```bash
cd /data/pub/crashomon && uv run pytest web/tests/ -v
```

Expected: all existing tests pass alongside the two new ones.

---

### Task 4: Commit

**Files:** all modified above

- [ ] **Step 1: Stage and commit**

```bash
cd /data/pub/crashomon && git add web/app.py web/templates/base.html web/tests/test_routes.py docs/superpowers/specs/2026-04-29-upload-error-display-design.md docs/superpowers/plans/2026-04-29-upload-error-display.md && git commit -m "feat(web): show flash error messages on upload failure"
```
