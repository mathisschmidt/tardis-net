FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    TARDIS_STATE_FILE=/app/data/state.json

WORKDIR /app

RUN addgroup --system app && adduser --system --ingroup app --home /app app

COPY pyproject.toml README.md ./
COPY app ./app

RUN pip install --no-cache-dir . \
    && mkdir -p /app/data \
    && chown app:app /app/data

USER app

EXPOSE 8000

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/api/health', timeout=2)" || exit 1

CMD ["tardis-net", "run", "--host", "0.0.0.0", "--port", "8000"]