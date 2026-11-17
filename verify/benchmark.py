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


def vector_literal(rng, number_format):
    components = [format(rng.uniform(-1, 1), number_format) for _ in range(DIM)]
    rounded = tuple(array.array("f", map(float, components)))
    return "'[" + ",".join(components) + "]'", rounded


def workload(seed, shuffle_keys=False, exponential=False, oracle_queries=40):
    rng = random.Random(seed)
    keys = list(range(ROWS))
    if shuffle_keys:
        random.Random(seed).shuffle(keys)
    number_format = ".10e" if exponential else ".6f"
    records, pending, inserts = {}, [], []
    for key in keys:
        label = rng.randrange(10)
        literal, vector = vector_literal(rng, number_format)
        records[key] = (label, vector)
        pending.append(f"({key},{label},{literal})")
        if len(pending) == BATCH:
            inserts.append("INSERT INTO bench VALUES " + ",".join(pending))
            pending.clear()
    queries, truth = [], {}
    for index in range(QUERIES):
        literal, target = vector_literal(rng, number_format)
        label = index % 10 if index % 2 else None
        where = "" if label is None else f" WHERE grp={label}"
        statement = f"SELECT id,L2_DISTANCE(v,{literal}) AS d FROM bench{where} ORDER BY d LIMIT {K}"
        queries.append((statement, label, target))
        if index < oracle_queries:
            distances = ((math.dist(vector, target), key)
                         for key, (group, vector) in records.items()
                         if label is None or group == label)
            truth[index] = heapq.nsmallest(K, distances)
    return records, inserts, queries, truth


def check_ok(session, sql):
    response = session.request(sql)
    assert response["ok"], response
    return response


def cgroup(session):
    pid = int(session.docker("inspect", "--format", "{{.State.Pid}}", session.name).stdout)
    try:
        entries = Path(f"/proc/{pid}/cgroup").read_text().splitlines()
    except FileNotFoundError:
        return None
    for entry in entries:
        if entry.startswith("0::"):
            return Path("/sys/fs/cgroup") / entry[3:].lstrip("/")
    return None


def read_cgroup_file(path, filename):
    if path is None:
        return ""
    try:
        return (path / filename).read_text()
    except FileNotFoundError:
        return ""


def read_peak(path):
    value = read_cgroup_file(path, "memory.peak")
    return int(value) if value else None


def read_cpu_usec(path):
    for line in read_cgroup_file(path, "cpu.stat").splitlines():
        name, value = line.split()
        if name == "usage_usec":
            return int(value)
    return None


def read_device_io(path, data_dir):
    device = data_dir.stat().st_dev
    prefix = f"{os.major(device)}:{os.minor(device)} "
    for line in read_cgroup_file(path, "io.stat").splitlines():
        if line.startswith(prefix):
            fields = dict(field.split("=") for field in line.split()[1:])
            return int(fields["rbytes"]) + int(fields["wbytes"])
    return None


def delta(end, start):
    return end - start if end is not None and start is not None else None


def latency(prefix, measurements):
    ordered = sorted(measurements)
    return {
        f"{prefix}_requests": len(ordered),
        f"{prefix}_request_s": sum(ordered),
        f"{prefix}_p50_ms": statistics.median(ordered) * 1000,
        f"{prefix}_p95_ms": ordered[math.ceil(0.95 * len(ordered)) - 1] * 1000,
    }


def probe_protocol(session, count):
    measurements = []
    for _ in range(count):
        start = time.perf_counter()
        response = check_ok(session, "SELECT 1")
        measurements.append(time.perf_counter() - start)
        assert response["rows"] == [[1]], response
    return latency("protocol_probe", measurements) if measurements else {}


def run(image, data, protocol_probe=0):
    records, inserts, queries, truth = data
    with tempfile.TemporaryDirectory(prefix="mvdb-bench-") as directory:
        root = Path(directory)
        session = Session(CONFIG, root / "stderr.log", image=image, data_root=root)
        files = Path(session.bind_data.name)
        try:
            start = time.perf_counter()
            session.start()
            startup = time.perf_counter() - start
            stats_group = cgroup(session)
            ready_device_io = read_device_io(stats_group, files) or 0
            check_ok(session, "CREATE TABLE bench(id BIGINT PRIMARY KEY,grp BIGINT,v VECTOR(128))")

            write_cpu_start = read_cpu_usec(stats_group)
            write_requests = []
            start = time.perf_counter()
            for statement in inserts:
                begin_request = time.perf_counter()
                response = check_ok(session, statement)
                write_requests.append(time.perf_counter() - begin_request)
                assert response["affected_rows"] == BATCH, response
            write_time = time.perf_counter() - start
            write_cpu = delta(read_cpu_usec(stats_group), write_cpu_start)
            write_peak = read_peak(stats_group)
            after_write_device_io = read_device_io(stats_group, files)

            protocol_metrics = probe_protocol(session, protocol_probe)
            check_ok(session, "CREATE VECTOR INDEX idx ON bench(v)")
            check_ok(session, queries[0][0])
            check_ok(session, queries[1][0])
            query_cpu_start = read_cpu_usec(stats_group)
            before_query_device_io = read_device_io(stats_group, files)
            query_requests = {False: [], True: []}
            results = []
            start = time.perf_counter()
            for statement, label, _ in queries:
                begin_request = time.perf_counter()
                rows = check_ok(session, statement)["rows"]
                query_requests[label is not None].append(time.perf_counter() - begin_request)
                results.append(rows)
            query_time = time.perf_counter() - start
            query_cpu = delta(read_cpu_usec(stats_group), query_cpu_start)
            query_peak = read_peak(stats_group)
            after_query_device_io = read_device_io(stats_group, files)
            before_close_bytes = sum(file.stat().st_size for file in files.iterdir())

            for rows, (_, label, target) in zip(results, queries):
                assert len(rows) == K and len({row[0] for row in rows}) == K
                assert rows == sorted(rows, key=lambda row: (row[1], row[0])), rows
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
                write_device_io_bytes=delta(after_write_device_io, ready_device_io),
                query_device_io_bytes=delta(after_query_device_io, ready_device_io),
                query_only_device_io_bytes=delta(after_query_device_io, before_query_device_io),
                write_cpu_usec=write_cpu, query_cpu_usec=query_cpu,
                **protocol_metrics,
                **latency("write", write_requests),
                **latency("filtered_query", query_requests[True]),
                **latency("unfiltered_query", query_requests[False]),
                data_bytes_before_shutdown=before_close_bytes, data_bytes_after_shutdown=durable_bytes,
                checked_exact_queries=len(truth))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="+")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--shuffle-keys", action="store_true",
                        help="insert randomly ordered primary keys")
    parser.add_argument("--exponential", action="store_true",
                        help="encode input numbers in exponential notation")
    parser.add_argument("--oracle-queries", type=int, default=40,
                        help="number of leading queries to check with an exhaustive oracle")
    parser.add_argument("--protocol-probe", type=int, default=0, metavar="REQUESTS",
                        help="measure this many SELECT 1 round trips separately before query warmup")
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    if not 0 <= args.oracle_queries <= QUERIES:
        parser.error(f"--oracle-queries must be between 0 and {QUERIES}")
    if args.protocol_probe < 0:
        parser.error("--protocol-probe cannot be negative")
    data = workload(args.seed, args.shuffle_keys, args.exponential, args.oracle_queries)
    workload_options = dict(seed=args.seed, shuffle_keys=args.shuffle_keys, exponential=args.exponential)
    results = {image: [] for image in args.image}
    for number in range(1, args.rounds + 1):
        for image in args.image:
            result = run(image, data, args.protocol_probe)
            results[image].append(result)
            print(json.dumps(dict(image=image, round=number, **workload_options, **result)), flush=True)
    for image, runs in results.items():
        median = {key: statistics.median(run[key] for run in runs)
                  for key in runs[0] if all(run[key] is not None for run in runs)}
        print(json.dumps(dict(image=image, **workload_options, median=median)), flush=True)


if __name__ == "__main__":
    main()
