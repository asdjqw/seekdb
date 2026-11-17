FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONIOENCODING=utf-8

WORKDIR /app

# numpy is only used for the vector scan fast path; the implementation falls
# back to pure Python when it is missing.
RUN pip install --no-cache-dir numpy==2.4.4

COPY minivectordb/minivectordb.py /app/minivectordb/minivectordb.py

RUN mkdir -p /data
VOLUME ["/data"]

ENTRYPOINT ["python", "/app/minivectordb/minivectordb.py"]
CMD ["--data-dir", "/data"]
