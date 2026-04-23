# Docker Compose Design — crashomon-web

**Date:** 2026-04-23

## Summary

Add a Docker Compose setup for `crashomon-web` (the Flask analysis UI). The watcherd continues to run natively on the target device. Developers manually upload minidumps and symbol files through the browser.

## Files

```
docker/
  Dockerfile          # Python 3.12-slim image; gunicorn entrypoint; non-root user
  docker-compose.yml  # single crashomon-web service; named volume; env_file
  .env.example        # template — users copy to .env before first run
```

## Architecture

- **Service**: `crashomon-web` (gunicorn on port 5000)
- **Data volume**: named `crashomon-data` mounted at `/data` inside the container
  - `CRASHOMON_SYMBOL_STORE=/data/symbols`
  - `CRASHOMON_DB=/data/crashes.db`
- **No code changes**: paths are already env-var-configurable

## Environment variables

| Variable | Required | Default in `.env.example` | Notes |
|---|---|---|---|
| `SECRET_KEY` | yes | placeholder | Must be replaced; `openssl rand -hex 32` |
| `CRASHOMON_SYMBOL_STORE` | no | `/data/symbols` | Inside the named volume |
| `CRASHOMON_DB` | no | `/data/crashes.db` | Inside the named volume |
| `CRASHOMON_AUTH_USER` / `CRASHOMON_AUTH_PASS` | no | commented out | HTTP Basic Auth for web routes |
| `CRASHOMON_API_KEY` | no | commented out | Bearer token for `/api/*` routes |

## Native binaries (symbolication)

`minidump-stackwalk` and `dump_syms` are searched on `PATH`. The image does not include them (avoids a multi-GB build stage). Without them:
- `.sym` file uploads work
- Raw (unsymbolicated) tombstone display works
- Auto-symbolication of `.dmp` uploads is skipped

To enable symbolication, bind-mount the pre-built binaries into `/usr/local/bin/` via an additional volume entry in docker-compose.yml (documented as a comment).

## Build context

The Dockerfile lives in `docker/` but needs `web/` and `tools/` from the project root. The Compose file sets `build.context: ..` so Docker uses the repo root as the build context.

Users run from the `docker/` directory:
```bash
cp .env.example .env
# edit .env — set SECRET_KEY at minimum
docker compose up -d
```

## No changes to existing files

This design touches only newly-created files in `docker/` and does not modify any source code, systemd units, or existing documentation.
