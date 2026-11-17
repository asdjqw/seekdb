FROM python:3.12-trixie@sha256:cad37dd20c7cac861af9003af8796d03ea7a998ec1e3a664429740fc9d7d2691 AS build
COPY minivectordb/native.c /src/native.c
RUN gcc -O3 -pthread -ffp-contract=off -fPIC -shared $(python3-config --includes) \
    /src/native.c -lm -o /_mvdb_native.so

FROM python:3.12-slim@sha256:5e2bdb1b0d542d5039cffb8181c65930093949194a65c365dfb1b0fe13a4fa5b

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONIOENCODING=utf-8

WORKDIR /app

COPY --from=build /_mvdb_native.so /app/_mvdb_native.so
COPY minivectordb/minivectordb.py /app/minivectordb.py

RUN mkdir -p /data
VOLUME ["/data"]

ENTRYPOINT ["python", "/app/minivectordb.py"]
CMD ["--data-dir", "/data"]
