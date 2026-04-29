# crashomon-web

Flask web UI for crash analysis. Manages a Breakpad symbol store, stores crash
history in SQLite, and auto-symbolicates uploaded minidumps.

## Prerequisites

- [uv](https://docs.astral.sh/uv/) — Python package and project manager
- Python 3.11+
- `crashomon-analyze` and `minidump_stackwalk` binaries (built from project root)

## Setup

From the **project root** (where `pyproject.toml` and `uv.lock` live):

```bash
# Create .venv and install all deps (runtime + dev) in one step
uv sync --extra dev
```

`uv sync` reads `pyproject.toml` + `uv.lock` and produces a fully reproducible
environment in `.venv/`. No pip, no manual activation needed for running commands.

## Run the development server

```bash
uv run flask --app web.app:create_app run --debug
```

Environment variables (all optional):

| Variable | Default | Description |
|---|---|---|
| `CRASHOMON_SYMBOL_STORE` | `/var/crashomon/symbols` | Breakpad symbol store root |
| `CRASHOMON_DB` | `/var/crashomon/crashes.db` | SQLite crash history database |
| `SECRET_KEY` | `dev-only-key` | Flask secret key (set in production) |

## Run tests

```bash
uv run pytest
```

## Update dependencies

To add a new runtime dependency:

```bash
uv add <package>          # adds to pyproject.toml and regenerates uv.lock
```

To add a new dev dependency:

```bash
uv add --optional dev <package>
```

After editing `pyproject.toml` manually, regenerate the lockfile:

```bash
uv lock
```

## Lint and format

```bash
uv run ruff check web/
uv run ruff format --check web/
```

## Docker

Build and run with Docker Compose from the `web/` directory:

```bash
cp .env.example .env   # then edit .env to set SECRET_KEY at minimum
docker compose up --build
```

The container exposes port 5000 and persists crash data and symbols in the
`crashomon-data` Docker volume (mounted at `/data` inside the container).

The image builds `dump_syms` and `minidump-stackwalk` from source during
`docker build`, so auto-symbolication works out of the box with no bind-mounts.

Environment variables (set in `.env` or via `docker compose run -e`):

| Variable | Default | Description |
|---|---|---|
| `SECRET_KEY` | *(required in production)* | Flask secret key |
| `CRASHOMON_SYMBOL_STORE` | `/data/symbols` | Breakpad symbol store root |
| `CRASHOMON_DB` | `/data/crashes.db` | SQLite crash history database |
| `CRASHOMON_AUTH_USER` + `CRASHOMON_AUTH_PASS` | *(unset)* | HTTP Basic Auth for the web UI |
| `CRASHOMON_API_KEY` | *(unset)* | API key for `/api/*` endpoints |
