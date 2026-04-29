# Upload Error Display — Design

## Problem

Symbol and crash upload routes in `crashomon-web` already produce error messages via Flask's `flash()` mechanism, but `base.html` never renders them. As a result, all errors are silently swallowed. The crash upload route also uses a bare `abort(400)` for two error cases instead of user-facing messages.

## Scope

- `web/templates/base.html` — render flash messages
- `web/app.py` — fix `upload_crash` error paths

Out of scope: symbolication failures that are recorded in the report body (those are visible on the crash detail page and require no change).

## Design

### Flash message rendering (`base.html`)

Add a flash message block between `<nav>` and `<main>`. Messages are categorized by Flask category (`"error"`, `"success"`, `"warning"`):

- **error**: red banner (`#e07070` text, `#3a1a1a` background, `#7a2a2a` border)
- **success**: green banner (`#7ec87e` text, `#1a3a1a` background, `#2a7a2a` border)
- **warning**: amber (unused currently, but natural to include)

Styled inline with the existing dark monospace theme. No JavaScript.

### `upload_crash` route fixes (`app.py`)

Two error cases currently call `abort(400)`:

| Case | Current | After |
|------|---------|-------|
| `secure_filename` returns empty string | `return abort(400)` | `flash("Invalid filename.", "error")` + redirect to `upload_form` |
| No file in form | `return redirect(url_for("upload_form"))` (silent) | `flash("No file selected.", "error")` + redirect to `upload_form` |

### No backend changes to `upload_symbols`

That route already calls `flash()` correctly. Once `base.html` renders messages, they will appear automatically.

## Testing

Existing `test_routes.py` tests cover the happy path. Add two new test cases:
- `POST /crashes/upload` with empty form → redirects to upload, flash error present
- `POST /crashes/upload` with a file whose `secure_filename` is empty → redirects, flash error present
