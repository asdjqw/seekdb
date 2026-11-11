"""CPU VSAG baseline for the AMD CAGRA build comparison.

Loads a 250K x 128 SIFT subset into seekdb and times the CPU `lib=vsag`
HNSW build. This is a development reference, not the official evaluator.
"""

from __future__ import annotations

import argparse
import time

import numpy as np
import pymysql


def vector_literal(vector: np.ndarray) -> str:
    return "[" + ",".join(f"{value:.6f}" for value in vector.tolist()) + "]"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True, help="raw float32 base vectors")
    parser.add_argument("--query", required=True, help="raw float32 queries")
    parser.add_argument("--gt", required=True, help="int32 exact top-100 ids")
    parser.add_argument("--rows", type=int, default=250000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--port", type=int, default=2881)
    args = parser.parse_args()

    base = np.fromfile(args.base, dtype=np.float32).reshape(-1, args.dim)
    query = np.fromfile(args.query, dtype=np.float32).reshape(-1, args.dim)
    groundtruth = np.fromfile(args.gt, dtype=np.int32).reshape(-1, 100)

    connection = pymysql.connect(
        host="127.0.0.1", port=args.port, user="root", password="", database="t1", autocommit=True
    )
    cursor = connection.cursor()
    cursor.execute("drop table if exists items_cpu")
    cursor.execute(f"create table items_cpu(id bigint primary key, embedding vector({args.dim}))")
    rows = [(index, vector_literal(base[index])) for index in range(min(args.rows, base.shape[0]))]
    started = time.time()
    for offset in range(0, len(rows), 500):
        cursor.executemany("insert into items_cpu values (%s,%s)", rows[offset : offset + 500])
    print(f"load_seconds={time.time() - started:.2f}", flush=True)

    started = time.time()
    cursor.execute(
        "create vector index idx_cpu on items_cpu(embedding) "
        "with (distance=l2, type=hnsw, lib=vsag)"
    )
    print(f"cpu_build_seconds={time.time() - started:.3f}", flush=True)

    started = time.time()
    hits = 0
    subset = min(100, query.shape[0])
    for index in range(subset):
        cursor.execute(
            "select id from items_cpu order by l2_distance(embedding, %s) approximate limit 10",
            (vector_literal(query[index]),),
        )
        ids = {row[0] for row in cursor.fetchall()}
        hits += len(ids & {int(value) for value in groundtruth[index][:10]})
    print(f"cpu_qps={subset / (time.time() - started):.1f} recall10={hits / (subset * 10):.4f}", flush=True)
    connection.close()


if __name__ == "__main__":
    main()
