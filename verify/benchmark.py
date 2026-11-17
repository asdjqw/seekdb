"""Compare MiniVectorDB images with a deterministic local JSONL workload."""
import argparse
import array
import heapq
import json
import math
import os
import random
import statistics
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "protocol_check"))
from _transport import Session

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())
ROWS, DIM, QUERIES, K, BATCH = 10_000, 128, 1000, 10, 100


def vector_literal(rng):
    components = [f"{rng.uniform(-1, 1):.6f}" for _ in range(DIM)]
    rounded = tuple(array.array("f", map(float, components)))
    return "'[" + ",".join(components) + "]'", rounded


def workload(seed):
    rng = random.Random(seed)
    records, pending, inserts = [], [], []
    for key in range(ROWS):
        label = rng.randrange(10)
        literal, vector = vector_literal(rng)
        records.append((label, vector))
        pending.append(f"({key},{label},{literal})")
        if len(pending) == BATCH:
            inserts.append("INSERT INTO bench VALUES " + ",".join(pending))
            pending.clear()
    queries, truth = [], {}
    for index in range(QUERIES):
        literal, target = vector_literal(rng)
        label = index % 10 if index % 2 else None
        where = "" if label is None else f" WHERE grp={label}"
        statement = f"SELECT id,L2_DISTANCE(v,{literal}) AS d FROM bench{where} ORDER BY d LIMIT {K}"
        queries.append((statement, label, target))
        if index < 40:
            distances = ((math.dist(vector, target), key)
                         for key, (group, vector) in enumerate(records)
                         if label is None or group == label)
            truth[index] = heapq.nsmallest(K, distances)
    return records, inserts, queries, truth


def check_ok(session, sql):
    response = session.request(sql)
    assert response["ok"], response
    return response


def cgroup(session):
    pid = int(session.docker("inspect", "--format", "{{.State.Pid}}", session.name).stdout)
    entries = Path(f"/proc/{pid}/cgroup").read_text().splitlines()
    return Path("/sys/fs/cgroup") / next(line[3:].lstrip("/") for line in entries
                                        if line.startswith("0::"))


def read_peak(path):
    try:
        return int((path / "memory.peak").read_text())
    except FileNotFoundError:
        return None


def read_device_io(path, data_dir):
    device = data_dir.stat().st_dev
    prefix = f"{os.major(device)}:{os.minor(device)} "
    stats = path / "io.stat"
    if not stats.exists():
        return None
    for line in stats.read_text().splitlines():
        if line.startswith(prefix):
            fields = dict(field.split("=") for field in line.split()[1:])
            return int(fields["rbytes"]) + int(fields["wbytes"])
    return None


def run(image, data):
    records, inserts, queries, truth = data
    with tempfile.TemporaryDirectory(prefix="mvdb-bench-") as directory:
        root = Path(directory)
        session = Session(CONFIG, root / "stderr.log", image=image, data_root=root)
        files = Path(session.bind_data.name)
        try:
            start = time.perf_counter()
            session.start()
            startup = time.perf_counter() - start
            group = cgroup(session)
            check_ok(session, "CREATE TABLE bench(id BIGINT PRIMARY KEY,grp BIGINT,v VECTOR(128))")

            start = time.perf_counter()
            for statement in inserts:
                response = check_ok(session, statement)
                assert response["affected_rows"] == BATCH, response
            write_time = time.perf_counter() - start
            write_peak = read_peak(group)
            write_device_io = read_device_io(group, files)

            check_ok(session, "CREATE VECTOR INDEX idx ON bench(v)")
            check_ok(session, queries[0][0])
            check_ok(session, queries[1][0])
            start = time.perf_counter()
            results = [check_ok(session, statement)["rows"] for statement, _, _ in queries]
            query_time = time.perf_counter() - start
            query_peak = read_peak(group)
            query_device_io = read_device_io(group, files)
            before_close_bytes = sum(file.stat().st_size for file in files.iterdir())

            for rows, (_, label, target) in zip(results, queries):
                assert len(rows) == K and len({row[0] for row in rows}) == K
                for key, distance in rows:
                    group, vector = records[key]
                    assert label is None or group == label, (key, label, group)
                    assert math.isclose(distance, math.dist(vector, target), abs_tol=1e-6), (
                        key, distance
                    )
            for index, expected in truth.items():
                for actual, (distance, key) in zip(results[index], expected):
                    assert actual[0] == key and math.isclose(actual[1], distance, abs_tol=1e-6), (
                        index, actual, distance, key
                    )
            start = time.perf_counter()
            session.shutdown()
            shutdown = time.perf_counter() - start
            durable_bytes = sum(file.stat().st_size for file in files.iterdir())

            start = time.perf_counter()
            session.start()
            reopen = time.perf_counter() - start
            for key in (0, 17, ROWS - 1):
                label, vector = records[key]
                rows = check_ok(session, f"SELECT grp,v FROM bench WHERE id={key}")["rows"]
                assert rows == [[label, list(vector)]], rows
            session.shutdown()
        finally:
            session.close()
    return dict(startup_s=startup, write_s=write_time, query_s=query_time, shutdown_s=shutdown,
                reopen_s=reopen, write_peak_bytes=write_peak, query_peak_bytes=query_peak,
                write_device_io_bytes=write_device_io, query_device_io_bytes=query_device_io,
                data_bytes_before_shutdown=before_close_bytes, data_bytes_after_shutdown=durable_bytes,
                checked_exact_queries=len(truth))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="+")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    data = workload(args.seed)
    results = {image: [] for image in args.image}
    for number in range(1, args.rounds + 1):
        for image in args.image:
            result = run(image, data)
            results[image].append(result)
            print(json.dumps(dict(image=image, round=number, **result)), flush=True)
    for image, runs in results.items():
        median = {key: statistics.median(run[key] for run in runs)
                  for key in runs[0] if all(run[key] is not None for run in runs)}
        print(json.dumps(dict(image=image, median=median)), flush=True)


if __name__ == "__main__":
    main()
