FROM python:3.12-slim

RUN useradd --system --create-home crashomon

WORKDIR /app

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
