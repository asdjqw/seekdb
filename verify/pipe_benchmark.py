"""Measure JSONL queries with the driver and engine in the same container."""
import argparse
import json
import math
import sys
import tempfile
import time
from pathlib import Path

from benchmark import CONFIG, Session, check_ok, latency, probe_protocol, workload


def run(program, rounds):
    _, inserts, queries, truth = workload(42)
    with tempfile.TemporaryDirectory() as directory:
        session = Session(
            CONFIG, Path(directory) / "stderr.log",
            command=[sys.executable, program, "--data-dir", "{data_dir}"],
        )
        try:
            session.start()
            check_ok(session, "CREATE TABLE bench(id BIGINT PRIMARY KEY,grp BIGINT,v VECTOR(128))")
            for statement in inserts:
                check_ok(session, statement)
            for statement, _, _ in queries[:20]:
                check_ok(session, statement)
            for number in range(rounds):
                measurements = {False: [], True: []}
                responses = []
                probe = probe_protocol(session, 100)
                start = time.monotonic()
                for statement, group, _ in queries:
                    tick = time.monotonic()
                    responses.append(check_ok(session, statement)["rows"])
                    measurements[group is not None].append(time.monotonic() - tick)
                elapsed = time.monotonic() - start
                for index, expected in truth.items():
                    actual = responses[index]
                    assert len(actual) == len(expected)
                    for (got_key, got_dist), (want_dist, want_key) in zip(actual, expected):
                        assert got_key == want_key and math.isclose(got_dist, want_dist, abs_tol=1e-7)
                print(json.dumps(dict(round=number + 1, query_s=elapsed, **probe,
                                      **latency("filtered", measurements[True]),
                                      **latency("unfiltered", measurements[False]))), flush=True)
            session.shutdown()
        finally:
            session.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", default="/app/minivectordb.py")
    parser.add_argument("--rounds", type=int, default=5)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    run(args.program, args.rounds)
