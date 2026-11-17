"""Verify exact TopK edge cases and recovery of the binary WAL in a Docker image."""
import argparse
import json
import math
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "protocol_check"))
from _transport import Session

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())


def sql(session, text, mode="exact"):
    response = session.request(text, mode)
    assert response["ok"], response
    return response.get("rows")


def run(candidate, legacy, root):
    session = Session(CONFIG, root / "stderr.log", image=legacy or candidate, data_root=root)
    try:
        session.start()
        sql(session, "CREATE TABLE t(id BIGINT PRIMARY KEY,grp VARCHAR(8),v VECTOR(2))")
        sql(session, "INSERT INTO t VALUES (99,'保留','[1,0]'), (8,'保留','[-1,0]'), "
                     "(3,'保留','[0,1]'), (1,'排除','[0,0]'), (6,'保留','[0,-1]')")
        sql(session, "CREATE VECTOR INDEX vec_idx ON t(v)")
        if legacy:
            session.shutdown()
            session.start()
            sql(session, "INSERT INTO t VALUES (10,'排除','[16777217,1e-45]')")
            session.crash()
            session.image = candidate
            session.start()
            rows = sql(session, "SELECT v FROM t WHERE id = 10")
            assert rows == [[[16777216.0, 2 ** -149]]], rows
            print("Legacy JSON snapshot + committed WAL -> binary format: PASS", flush=True)

        topk = "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE grp='保留' ORDER BY d LIMIT 2"
        rows = sql(session, topk)
        assert rows == [[3, 1.0], [6, 1.0]], rows
        assert sql(session, topk.replace("LIMIT 2", "DESC LIMIT 2")) == rows
        assert sql(session, topk.replace("grp='保留'", "'保留'=grp"), "ann") == rows
        assert sql(session, topk.replace("grp='保留'", "grp='无'")) == []
        print(f"Filtered TopK tie at K (ASC/DESC/ANN): {rows}", flush=True)

        sql(session, "CREATE TABLE extremes(id BIGINT PRIMARY KEY,v VECTOR(2))")
        sql(session, "INSERT INTO extremes VALUES (1,'[3e38,0]'), (2,'[2e38,0]'), "
                     "(3,'[-2e38,0]'), (4,'[3.00000002e38,1]')")
        rows = sql(session, "SELECT id,L2_DISTANCE(v,'[-3e38,0]') AS d FROM extremes ORDER BY d LIMIT 3")
        assert [row[0] for row in rows] == [3, 2, 1] and all(math.isfinite(row[1]) for row in rows), rows
        print(f"Extreme float32 differences produce finite distances in exact order: {rows}", flush=True)

        sql(session, "CREATE TABLE mixed(id BIGINT PRIMARY KEY,v VECTOR(31))")
        vectors = [[1] * 31 for _ in range(2)]
        vectors[0][0] = 2 ** 27
        vectors[1][16] = 2 ** 27
        values = ",".join(f"({key},'{vector}')" for key, vector in enumerate(vectors, 1))
        sql(session, "INSERT INTO mixed VALUES " + values)
        zero = [0] * 31
        rows = sql(session, f"SELECT id,L2_DISTANCE(v,'{zero}') AS d FROM mixed ORDER BY d LIMIT 1")
        assert rows == [[1, math.dist(vectors[0], zero)]], rows
        print(f"Mixed-magnitude tie across SIMD and tail dimensions: {rows}", flush=True)

        sql(session, "BEGIN")
        sql(session, "UPDATE t SET grp='排除' WHERE id=3")
        sql(session, "DELETE FROM t WHERE id=6")
        sql(session, "INSERT INTO t VALUES (0,'保留','[0,0]')")
        assert sql(session, topk) == [[0, 0.0], [8, 1.0]]
        session.crash()

        log = Path(session.bind_data.name) / "wal.v3"
        committed_size = log.stat().st_size
        with log.open("ab") as handle:
            handle.write(b"\x00" * 7)
        session.start()
        assert log.stat().st_size == committed_size
        assert sql(session, topk) == [[3, 1.0], [6, 1.0]]
        sql(session, "BEGIN")
        sql(session, "INSERT INTO t VALUES (0,'保留','[0,0]'), (-1,'保留','[0,0]')")
        sql(session, "COMMIT")
        session.crash()
        session.start()
        rows = sql(session, topk)
        assert rows == [[-1, 0.0], [0, 0.0]], rows
        session.shutdown()
        print(f"Kill, partial WAL frame, reopen, commit, kill, reopen: {rows}", flush=True)
    finally:
        session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate")
    parser.add_argument("--legacy", help="optional image with the pre-upgrade JSON WAL")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mvdb-regression-") as directory:
        run(args.candidate, args.legacy, Path(directory))


if __name__ == "__main__":
    main()
