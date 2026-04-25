FROM python:3.12-slim

RUN useradd --system --create-home crashomon

WORKDIR /app

COPY pyproject.toml uv.lock ./
RUN pip install --no-cache-dir uv && \
    uv sync --no-dev --frozen && \
    pip uninstall -y uv

COPY web/ web/
COPY tools/ tools/

ENV PATH="/app/.venv/bin:$PATH"

RUN mkdir -p /data && chown crashomon:crashomon /data

USER crashomon

EXPOSE 5000

CMD ["gunicorn", "web.app:create_app()", "--bind", "0.0.0.0:5000", "--workers", "2", "--timeout", "120"]
