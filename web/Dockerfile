# ── Stage 1: Build dump_syms (Breakpad) ──────────────────────────────────────
FROM debian:bookworm-slim AS builder-dump-syms

RUN apt-get update && apt-get install -y --no-install-recommends \
        git cmake make g++ ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY cmake/ cmake/

# -DCARGO=skip satisfies the find_program guard in host_toolkit/CMakeLists.txt
# without installing Rust here; we build only the dump_syms target so the
# rust_minidump_build ExternalProject never runs.
RUN cmake -B /build -S cmake/host_toolkit \
        -DCMAKE_BUILD_TYPE=Release \
        -DCARGO=skip \
    && cmake --build /build --target dump_syms -j"$(nproc)" \
    && strip /build/bin/dump_syms

# ── Stage 2: Build minidump-stackwalk (Rust) ──────────────────────────────────
FROM rust:slim-bookworm AS builder-stackwalk

RUN apt-get update && apt-get install -y --no-install-recommends \
        git \
    && rm -rf /var/lib/apt/lists/*

RUN cargo install \
        --git https://github.com/rust-minidump/rust-minidump \
        --tag v0.26.1 \
        minidump-stackwalk \
    && strip /usr/local/cargo/bin/minidump-stackwalk

# ── Final image ───────────────────────────────────────────────────────────────
FROM python:3.12-slim

RUN useradd --system --create-home crashomon

WORKDIR /app

COPY --from=builder-dump-syms /build/bin/dump_syms /usr/local/bin/dump_syms
COPY --from=builder-stackwalk /usr/local/cargo/bin/minidump-stackwalk /usr/local/bin/minidump-stackwalk

# tools/ must be present before uv sync — crashomon-tools is a path dependency.
COPY pyproject.toml uv.lock ./
COPY tools/ tools/
RUN pip install --no-cache-dir uv && \
    uv sync --no-dev --frozen && \
    pip uninstall -y uv

COPY web/ web/

ENV PATH="/app/.venv/bin:$PATH"
ENV CRASHOMON_SYMBOL_STORE=/data/symbols
ENV CRASHOMON_DB=/data/crashes.db

RUN mkdir -p /data && chown crashomon:crashomon /data

USER crashomon

EXPOSE 5000

CMD ["gunicorn", "web.app:create_app()", "--bind", "0.0.0.0:5000", "--workers", "2", "--timeout", "120"]
