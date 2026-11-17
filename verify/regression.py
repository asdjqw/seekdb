"""Verify exact TopK edge cases and recovery of the binary WAL in a Docker image."""
import argparse
import array
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


def check_insert_paths(session):
    sql(session, "CREATE TABLE literals(id BIGINT PRIMARY KEY,txt VARCHAR(20),v VECTOR(2))")
    sql(session, """INSERT INTO literals VALUES (-9223372036854775808,'甲,乙','[1e-45,3]'),
                    (9,"plain","[2,4]");""")
    sql(session, """INSERT INTO literals(v,txt,id) VALUES ("[2,4]",'can''t',7)""")
    for statement, error in (
        ("INSERT INTO literals VALUES (10,'a','[1,2]'),(11,'b','[3e40,4]')", "1366"),
        ("INSERT INTO literals VALUES (10,'a','[1,2]'),(11,'b','[3,4]') x", "1064"),
        ("INSERT INTO literals VALUES (10,'a','[1,2]'),(9223372036854775808,'b','[3,4]')", "1264"),
    ):
        response = session.request(statement)
        assert not response["ok"] and response["error"]["code"] == error, response
    rows = sql(session, "SELECT * FROM literals ORDER BY id")
    assert rows == [[-2 ** 63, "甲,乙", [2 ** -149, 3.0]],
                    [7, "can't", [2.0, 4.0]], [9, "plain", [2.0, 4.0]]], rows
    print(f"Plain INSERT, parser fallback, rejected batches: {rows}", flush=True)


def check_near_cutoff(session):
    sql(session, "CREATE TABLE close_scores(id BIGINT PRIMARY KEY,grp BIGINT,v VECTOR(128))")
    vectors = {}
    for key in range(31, 0, -1):
        vector = [1.0] * 127 + [1 + (key * 7 % 11 - 5) * 2 ** -23]
        vectors[key] = list(array.array("f", vector))
    values = ",".join(f"({key},{key % 2},'{vector}')" for key, vector in vectors.items())
    sql(session, "INSERT INTO close_scores VALUES " + values)
    for target in ([0] * 128, [2] * 127 + [-1]):
        for group in (None, 1):
            where = "" if group is None else " WHERE grp=1"
            candidates = [(key, math.dist(vector, target)) for key, vector in vectors.items()
                          if group is None or key % 2 == group]
            for desc in (False, True):
                order = "DESC" if desc else "ASC"
                statement = (f"SELECT id,L2_DISTANCE(v,'{target}') AS d FROM close_scores"
                             f"{where} ORDER BY d {order} LIMIT 6")
                expected = sorted(candidates, key=lambda hit: (-hit[1] if desc else hit[1], hit[0]))[:6]
                actual = sql(session, statement)
                assert [key for key, _ in actual] == [key for key, _ in expected], actual
                assert all(math.isclose(got[1], ref[1], rel_tol=1e-14)
                           for got, ref in zip(actual, expected)), actual
    print("Near-cutoff float32 scores, rebinding vector literal, filtered/unfiltered ASC/DESC: 8/8 PASS",
          flush=True)


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
            print(f"Recovery of snapshot/WAL from {legacy}: {rows}", flush=True)

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
        check_insert_paths(session)
        check_near_cutoff(session)

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
    parser.add_argument("--legacy", help="optional image that created the initial snapshot/WAL")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mvdb-regression-") as directory:
        run(args.candidate, args.legacy, Path(directory))


if __name__ == "__main__":
    main()
