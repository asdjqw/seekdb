#!/usr/bin/env python3
"""MiniVectorDB - a small single-process vector database.

Supports the SQL subset required by the 2026 OceanBase competition round 2:

  CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(16), v VECTOR(3));
  CREATE VECTOR INDEX idx ON t(v);
  INSERT INTO t VALUES (1, 'a', '[1,2,3]'), (2, 'b', '[4,5,6]');
  SELECT id, L2_DISTANCE(v, '[0,0,0]') AS d FROM t WHERE id >= 1
         ORDER BY d LIMIT 10;
  UPDATE t SET name = 'c' WHERE id = 1;
  DELETE FROM t WHERE id = 2;
  BEGIN; ...; COMMIT; / ROLLBACK;

Communication is JSON Lines over stdin/stdout (see STDIO_PROTOCOL.md).
Persistence lives under --data-dir (default /data): a snapshot plus a WAL.
"""

from __future__ import annotations

import argparse
import array
import bisect
import heapq
import itertools
import json
import marshal
import math
import os
import struct
import sys
import zlib
from typing import Any

try:
    import _mvdb_native
except ModuleNotFoundError:
    _mvdb_native = None


# --------------------------------------------------------------------------
# errors
# --------------------------------------------------------------------------
class DBError(Exception):
    def __init__(self, code: str, message: str = "") -> None:
        super().__init__(message or code)
        self.code = code
        self.message = message or code


class StorageError(Exception):
    """Write outcome is uncertain. The server must reopen and recover."""


# MySQL-style numeric codes for the JSONL protocol.  The semantic names above
# stay internal; the judge's optional error_codes mapping matches these strings.
ERROR_CODE_MAP = {
    "duplicate_key": "1062",
    "unsupported_sql": "1235",
    "syntax_error": "1064",
    "table_not_found": "1146",
    "table_exists": "1050",
    "column_not_found": "1054",
    "column_exists": "1060",
    "column_count_mismatch": "1136",
    "dimension_mismatch": "1366",
    "invalid_vector": "1366",
    "type_mismatch": "1366",
    "value_too_long": "1406",
    "out_of_range": "1264",
    "null_not_supported": "1235",
    "primary_key_required": "1364",
    "transaction_error": "1568",
    "index_exists": "1061",
    "unsupported_op": "1235",
    "internal_error": "1105",
}


# --------------------------------------------------------------------------
# tokenizer
# --------------------------------------------------------------------------
class Token:
    __slots__ = ("kind", "value")

    def __init__(self, kind: str, value: Any) -> None:
        self.kind = kind
        self.value = value

    def is_kw(self, *words: str) -> bool:
        return self.kind == "ident" and self.value.upper() in words

    def __repr__(self) -> str:  # pragma: no cover - debugging helper
        return f"Token({self.kind},{self.value!r})"


_SYMBOLS = ("<=", ">=", "<>", "!=", "=", "<", ">", "(", ")", ",", ";", "*", ".")


def tokenize(sql: str) -> list[Token]:
    tokens: list[Token] = []
    i, n = 0, len(sql)
    while i < n:
        ch = sql[i]
        if ch in " \t\r\n":
            i += 1
            continue
        if ch == "-" and i + 1 < n and sql[i + 1] == "-":
            while i < n and sql[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and sql[i + 1] == "*":
            end = sql.find("*/", i + 2)
            if end < 0:
                raise DBError("syntax_error", "unterminated block comment")
            i = end + 2
            continue
        if ch == "'" or ch == '"':
            quote = ch
            i += 1
            end = sql.find(quote, i)
            if end < 0:
                raise DBError("syntax_error", "unterminated string literal")
            if sql.find("\\", i, end) < 0 and not sql.startswith(quote, end + 1):
                tokens.append(Token("string", sql[i:end]))
                i = end + 1
                continue
            buf: list[str] = []
            while True:
                if i >= n:
                    raise DBError("syntax_error", "unterminated string literal")
                c = sql[i]
                if c == quote:
                    if i + 1 < n and sql[i + 1] == quote:
                        buf.append(quote)
                        i += 2
                        continue
                    i += 1
                    break
                if c == "\\" and i + 1 < n:
                    nxt = sql[i + 1]
                    mapping = {"n": "\n", "t": "\t", "r": "\r", "0": "\0",
                               "\\": "\\", "'": "'", '"': '"'}
                    buf.append(mapping.get(nxt, nxt))
                    i += 2
                    continue
                buf.append(c)
                i += 1
            tokens.append(Token("string", "".join(buf)))
            continue
        if ch.isdigit():
            j = i
            while j < n and sql[j].isdigit():
                j += 1
            tokens.append(Token("number", int(sql[i:j])))
            i = j
            continue
        if ch == "-" and i + 1 < n and sql[i + 1].isdigit():
            j = i + 1
            while j < n and sql[j].isdigit():
                j += 1
            tokens.append(Token("number", -int(sql[i + 1:j])))
            i = j
            continue
        if ch.isalpha() or ch == "_":
            j = i
            while j < n and (sql[j].isalnum() or sql[j] in "_$"):
                j += 1
            tokens.append(Token("ident", sql[i:j]))
            i = j
            continue
        if ch == "`":
            j = sql.find("`", i + 1)
            if j < 0:
                raise DBError("syntax_error", "unterminated identifier")
            tokens.append(Token("ident", sql[i + 1:j]))
            i = j + 1
            continue
        for sym in _SYMBOLS:
            if sql.startswith(sym, i):
                tokens.append(Token("symbol", sym))
                i += len(sym)
                break
        else:
            raise DBError("syntax_error", f"unexpected character {ch!r}")
    return tokens


# --------------------------------------------------------------------------
# AST + parser
# --------------------------------------------------------------------------
class Parser:
    def __init__(self, tokens: list[Token]) -> None:
        self.tokens = tokens
        self.pos = 0

    # -- helpers ------------------------------------------------------
    def peek(self, offset: int = 0) -> Token | None:
        idx = self.pos + offset
        return self.tokens[idx] if idx < len(self.tokens) else None

    def next(self) -> Token:
        tok = self.peek()
        if tok is None:
            raise DBError("syntax_error", "unexpected end of statement")
        self.pos += 1
        return tok

    def accept_kw(self, *words: str) -> bool:
        tok = self.peek()
        if tok is not None and tok.is_kw(*words):
            self.pos += 1
            return True
        return False

    def expect_kw(self, word: str) -> None:
        tok = self.next()
        if not tok.is_kw(word):
            raise DBError("syntax_error", f"expected {word}")

    def accept_symbol(self, sym: str) -> bool:
        tok = self.peek()
        if tok is not None and tok.kind == "symbol" and tok.value == sym:
            self.pos += 1
            return True
        return False

    def expect_symbol(self, sym: str) -> None:
        tok = self.next()
        if tok.kind != "symbol" or tok.value != sym:
            raise DBError("syntax_error", f"expected {sym!r}")

    def expect_ident(self) -> str:
        tok = self.next()
        if tok.kind != "ident":
            raise DBError("syntax_error", "expected identifier")
        return str(tok.value)

    # -- statement ----------------------------------------------------
    def parse(self) -> dict[str, Any]:
        tok = self.peek()
        if tok is None:
            raise DBError("syntax_error", "empty statement")
        if tok.is_kw("CREATE"):
            stmt = self.parse_create()
        elif tok.is_kw("DROP"):
            stmt = self.parse_drop()
        elif tok.is_kw("TRUNCATE"):
            stmt = self.parse_truncate()
        elif tok.is_kw("USE"):
            self.next()
            self.expect_ident()
            stmt = {"type": "noop"}
        elif tok.is_kw("SET"):
            self.next()
            while self.peek() is not None and not (self.peek().kind == "symbol" and self.peek().value == ";"):
                self.next()
            stmt = {"type": "noop"}
        elif tok.is_kw("INSERT"):
            stmt = self.parse_insert()
        elif tok.is_kw("SELECT"):
            stmt = self.parse_select()
        elif tok.is_kw("UPDATE"):
            stmt = self.parse_update()
        elif tok.is_kw("DELETE"):
            stmt = self.parse_delete()
        elif tok.is_kw("BEGIN", "START"):
            self.next()
            if stmt_kw := self.peek():
                if stmt_kw.is_kw("TRANSACTION", "WORK"):
                    self.next()
            stmt = {"type": "begin"}
        elif tok.is_kw("COMMIT"):
            self.next()
            if (stmt_kw := self.peek()) and stmt_kw.is_kw("WORK", "TRANSACTION"):
                self.next()
            stmt = {"type": "commit"}
        elif tok.is_kw("ROLLBACK"):
            self.next()
            if (stmt_kw := self.peek()) and stmt_kw.is_kw("WORK", "TRANSACTION"):
                self.next()
            stmt = {"type": "rollback"}
        else:
            raise DBError("unsupported_sql", f"unsupported statement: {tok.value}")
        self.accept_symbol(";")
        if self.peek() is not None:
            raise DBError("syntax_error", "trailing tokens after statement")
        return stmt

    def parse_create(self) -> dict[str, Any]:
        self.expect_kw("CREATE")
        if self.accept_kw("TABLE"):
            if_not_exists = False
            if self.accept_kw("IF"):
                self.expect_kw("NOT")
                self.expect_kw("EXISTS")
                if_not_exists = True
            name = self.expect_ident()
            self.expect_symbol("(")
            columns: list[dict[str, Any]] = []
            table_pk: str | None = None
            while True:
                if self.peek() is not None and self.peek().is_kw("PRIMARY"):
                    self.next()
                    self.expect_kw("KEY")
                    self.expect_symbol("(")
                    table_pk = self.expect_ident()
                    self.expect_symbol(")")
                else:
                    col_name = self.expect_ident()
                    type_tok = self.next()
                    if type_tok.kind != "ident":
                        raise DBError("syntax_error", "expected column type")
                    ctype = str(type_tok.value).upper()
                    size = None
                    if self.accept_symbol("("):
                        num = self.next()
                        if num.kind != "number":
                            raise DBError("syntax_error", "expected type size")
                        size = int(num.value)
                        self.expect_symbol(")")
                    primary = False
                    while True:
                        if self.accept_kw("PRIMARY"):
                            self.expect_kw("KEY")
                            primary = True
                        elif self.accept_kw("NOT"):
                            self.expect_kw("NULL")
                        elif self.accept_kw("UNIQUE"):
                            pass
                        else:
                            break
                    columns.append({"name": col_name, "type": ctype, "size": size,
                                    "primary": primary})
                if self.accept_symbol(","):
                    continue
                self.expect_symbol(")")
                break
            return {"type": "create_table", "name": name, "columns": columns,
                    "if_not_exists": if_not_exists, "table_pk": table_pk}
        if self.accept_kw("VECTOR"):
            self.expect_kw("INDEX")
            index_name = self.expect_ident()
            self.expect_kw("ON")
            table = self.expect_ident()
            self.expect_symbol("(")
            column = self.expect_ident()
            self.expect_symbol(")")
            return {"type": "create_vector_index", "name": index_name,
                    "table": table, "column": column}
        raise DBError("unsupported_sql", "unsupported CREATE form")

    def parse_drop(self) -> dict[str, Any]:
        self.expect_kw("DROP")
        self.expect_kw("TABLE")
        if_exists = False
        if self.accept_kw("IF"):
            self.expect_kw("EXISTS")
            if_exists = True
        return {"type": "drop_table", "name": self.expect_ident(), "if_exists": if_exists}

    def parse_truncate(self) -> dict[str, Any]:
        self.expect_kw("TRUNCATE")
        self.accept_kw("TABLE")
        return {"type": "truncate", "name": self.expect_ident()}

    def parse_insert(self) -> dict[str, Any]:
        self.expect_kw("INSERT")
        self.expect_kw("INTO")
        table = self.expect_ident()
        columns = None
        if self.peek() is not None and self.peek().kind == "symbol" and self.peek().value == "(":
            self.next()
            columns = []
            while True:
                columns.append(self.expect_ident())
                if self.accept_symbol(","):
                    continue
                self.expect_symbol(")")
                break
        self.expect_kw("VALUES") if not self.accept_kw("VALUE") else None
        rows = []
        while True:
            self.expect_symbol("(")
            values = []
            while True:
                values.append(self.parse_literal())
                if self.accept_symbol(","):
                    continue
                self.expect_symbol(")")
                break
            rows.append(values)
            if self.accept_symbol(","):
                continue
            break
        return {"type": "insert", "table": table, "columns": columns, "rows": rows}

    def parse_select(self) -> dict[str, Any]:
        self.expect_kw("SELECT")
        items = []
        while True:
            if self.accept_symbol("*"):
                items.append({"expr": ("star",), "alias": None})
            else:
                expr = self.parse_expr()
                alias = None
                if self.accept_kw("AS"):
                    alias = self.expect_ident()
                elif self.peek() is not None and self.peek().kind == "ident" and not self.peek().is_kw(
                        "FROM", "WHERE", "ORDER", "LIMIT", "AND", "OR", "ASC", "DESC"):
                    alias = self.expect_ident()
                items.append({"expr": expr, "alias": alias})
            if self.accept_symbol(","):
                continue
            break
        table = None
        if self.accept_kw("FROM"):
            table = self.expect_ident()
        where = None
        if table is not None and self.accept_kw("WHERE"):
            where = self.parse_condition()
        order: list[dict[str, Any]] = []
        if self.accept_kw("ORDER"):
            if table is None:
                raise DBError("syntax_error", "ORDER BY needs a table")
            self.expect_kw("BY")
            while True:
                expr = self.parse_expr()
                desc = False
                if self.accept_kw("ASC"):
                    desc = False
                elif self.accept_kw("DESC"):
                    desc = True
                order.append({"expr": expr, "desc": desc})
                if self.accept_symbol(","):
                    continue
                break
        limit = None
        if self.accept_kw("LIMIT"):
            num = self.next()
            if num.kind != "number":
                raise DBError("syntax_error", "expected LIMIT number")
            limit = int(num.value)
        return {"type": "select", "table": table, "items": items, "where": where,
                "order": order, "limit": limit}

    def parse_update(self) -> dict[str, Any]:
        self.expect_kw("UPDATE")
        table = self.expect_ident()
        self.expect_kw("SET")
        sets = []
        while True:
            column = self.expect_ident()
            self.expect_symbol("=")
            sets.append((column, self.parse_literal()))
            if self.accept_symbol(","):
                continue
            break
        where = None
        if self.accept_kw("WHERE"):
            where = self.parse_condition()
        return {"type": "update", "table": table, "sets": sets, "where": where}

    def parse_delete(self) -> dict[str, Any]:
        self.expect_kw("DELETE")
        self.expect_kw("FROM")
        table = self.expect_ident()
        where = None
        if self.accept_kw("WHERE"):
            where = self.parse_condition()
        return {"type": "delete", "table": table, "where": where}

    # -- conditions / expressions -------------------------------------
    def parse_condition(self):
        if self.accept_symbol("("):
            node = self.parse_condition()
            self.expect_symbol(")")
        else:
            node = self.parse_comparison()
        while True:
            if self.accept_kw("AND"):
                node = ("and", node, self.parse_condition_term())
            elif self.accept_kw("OR"):
                node = ("or", node, self.parse_condition_term())
            else:
                break
        return node

    def parse_condition_term(self):
        if self.accept_symbol("("):
            node = self.parse_condition()
            self.expect_symbol(")")
            return node
        return self.parse_comparison()

    def parse_comparison(self):
        left = self.parse_value()
        tok = self.peek()
        if tok is None or tok.kind != "symbol" or tok.value not in ("=", "<", "<=", ">", ">=", "!=", "<>"):
            raise DBError("syntax_error", "expected comparison operator")
        op = self.next().value
        if op == "<>":
            op = "!="
        right = self.parse_value()
        return ("cmp", op, left, right)

    def parse_value(self):
        tok = self.peek()
        if tok is None:
            raise DBError("syntax_error", "expected value")
        if tok.kind == "ident" and self.peek(1) is not None and self.peek(1).kind == "symbol" \
                and self.peek(1).value == "(":
            return self.parse_expr()
        if tok.kind == "number":
            self.next()
            return ("literal", int(tok.value))
        if tok.kind == "string":
            self.next()
            return ("literal", tok.value)
        if tok.is_kw("NULL"):
            self.next()
            return ("literal", None)
        if tok.kind == "ident":
            self.next()
            if self.accept_symbol("."):
                # table-qualified column: ignore the prefix
                self.next()
            return ("column", str(tok.value))
        raise DBError("syntax_error", "expected literal or column")

    def parse_literal(self):
        tok = self.next()
        if tok.kind == "number":
            return ("literal", int(tok.value))
        if tok.kind == "string":
            return ("literal", tok.value)
        if tok.is_kw("NULL"):
            return ("literal", None)
        raise DBError("syntax_error", "expected literal value")

    def parse_expr(self):
        tok = self.peek()
        if tok is None:
            raise DBError("syntax_error", "expected expression")
        if tok.kind == "ident" and self.peek(1) is not None and self.peek(1).kind == "symbol" \
                and self.peek(1).value == "(":
            func = str(tok.value).upper()
            self.next()
            self.expect_symbol("(")
            if func == "L2_DISTANCE":
                column = self.expect_ident()
                self.expect_symbol(",")
                lit = self.parse_literal()
                self.expect_symbol(")")
                return ("l2", column, lit[1])
            raise DBError("unsupported_sql", f"unsupported function {func}")
        if tok.kind == "number":
            self.next()
            return ("literal", int(tok.value))
        if tok.kind == "string":
            self.next()
            return ("literal", tok.value)
        if tok.kind == "ident":
            self.next()
            if self.accept_symbol("."):
                self.next()
            return ("column", str(tok.value))
        raise DBError("syntax_error", "expected expression")


# --------------------------------------------------------------------------
# schema helpers
# --------------------------------------------------------------------------
class Column:
    __slots__ = ("name", "type", "size")

    def __init__(self, name: str, ctype: str, size: int | None) -> None:
        self.name = name
        self.type = ctype
        self.size = size

    def to_json(self) -> dict[str, Any]:
        return {"name": self.name, "type": self.type, "size": self.size}

    @staticmethod
    def from_json(data: dict[str, Any]) -> "Column":
        return Column(data["name"], data["type"], data.get("size"))


def _round_float32(value: float) -> float:
    try:
        packed = struct.pack("<f", float(value))
    except (OverflowError, struct.error) as exc:
        raise DBError("invalid_vector", "value is not representable as float32") from exc
    result = struct.unpack("<f", packed)[0]
    if not math.isfinite(result):
        raise DBError("invalid_vector", "vector value must be finite")
    return result


def parse_vector_literal(text: str, dimension: int) -> bytes:
    if not isinstance(text, str):
        raise DBError("invalid_vector", "vector literal must be a string")
    if _mvdb_native is not None and dimension <= len(text) and "\0" not in text:
        vector = _mvdb_native.parse_vector(text, dimension)
        if vector is not None:
            return vector
    body = text.strip()
    if not (body.startswith("[") and body.endswith("]")):
        raise DBError("invalid_vector", "vector literal must look like [1,2,3]")
    inner = body[1:-1].strip()
    parts = [] if inner == "" else inner.split(",")
    if len(parts) != dimension:
        raise DBError("dimension_mismatch",
                      f"vector dimension {len(parts)} does not match column dimension {dimension}")
    values = array.array("f")
    for part in parts:
        piece = part.strip()
        if not piece:
            raise DBError("invalid_vector", "empty vector component")
        try:
            number = float(piece)
        except ValueError as exc:
            raise DBError("invalid_vector", f"bad vector component {piece!r}") from exc
        if not math.isfinite(number):
            raise DBError("invalid_vector", "vector value must be finite")
        values.append(_round_float32(number))
    return values.tobytes()


# --------------------------------------------------------------------------
# table
# --------------------------------------------------------------------------
class Table:
    def __init__(self, name: str, columns: list[Column], pk_idx: int, vec_idx: int) -> None:
        self.name = name
        self.columns = columns
        self.pk_idx = pk_idx
        self.vec_idx = vec_idx
        self.rows: dict[int, list[Any]] = {}
        self.indexes: dict[str, str] = {}
        self._cache_pks = None
        self._scan = None
        self._groups: dict[int, dict[Any, array.array] | None] = {}

    # -- schema -------------------------------------------------------
    def column_index(self, name: str) -> int:
        lowered = name.lower()
        for idx, column in enumerate(self.columns):
            if column.name.lower() == lowered:
                return idx
        raise DBError("column_not_found", f"unknown column {name}")

    def to_json(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "columns": [c.to_json() for c in self.columns],
            "pk": self.pk_idx,
            "vec": self.vec_idx,
            "indexes": self.indexes,
        }

    @staticmethod
    def from_meta(data: dict[str, Any]) -> "Table":
        table = Table(data["name"], [Column.from_json(c) for c in data["columns"]],
                      data["pk"], data.get("vec", -1))
        table.indexes = dict(data.get("indexes") or {})
        return table

    def add_row(self, row: list[Any]) -> None:
        if self.vec_idx >= 0 and not isinstance(row[self.vec_idx], bytes):
            row[self.vec_idx] = array.array("f", row[self.vec_idx]).tobytes()
        self.rows[row[self.pk_idx]] = row

    # -- vector cache -------------------------------------------------
    def vector_scan(self):
        if self._cache_pks is None:
            self._cache_pks = sorted(self.rows)
        if _mvdb_native is not None and self._scan is None and self._cache_pks:
            vectors = tuple(self.rows[pk][self.vec_idx] for pk in self._cache_pks)
            self._scan = _mvdb_native.build_scan(vectors, self.columns[self.vec_idx].size)
        return self._cache_pks, self._scan

    def equal_slots(self, pks, index, value):
        if index == self.pk_idx:
            if value not in self.rows:
                return array.array("I")
            return array.array("I", [bisect.bisect_left(pks, value)])
        if index not in self._groups:
            groups = {}
            for slot, pk in enumerate(pks):
                key = self.rows[pk][index]
                group = groups.get(key)
                if group is None:
                    # Cap lookup overhead for high-cardinality columns.
                    if len(groups) >= 1024:
                        groups = None
                        break
                    groups[key] = group = array.array("I")
                group.append(slot)
            self._groups[index] = groups
        groups = self._groups[index]
        if groups is None:
            return array.array("I", (i for i, pk in enumerate(pks) if self.rows[pk][index] == value))
        return groups.get(value, array.array("I"))

    def touch(self) -> None:
        self._cache_pks = None
        self._scan = None
        self._groups.clear()


# --------------------------------------------------------------------------
# storage: snapshot + WAL
# --------------------------------------------------------------------------
class Storage:
    SNAPSHOT = "snapshot.v3"
    WAL = "wal.v3"
    FRAME = struct.Struct("<QI")
    CHECKPOINT_BYTES = 64 * 1024 * 1024

    def __init__(self, data_dir: str) -> None:
        self.data_dir = data_dir
        os.makedirs(data_dir, exist_ok=True)
        self.snapshot_path = os.path.join(data_dir, self.SNAPSHOT)
        self.wal_path = os.path.join(data_dir, self.WAL)
        self.lsn = 0
        self.wal_bytes = 0
        self.checkpoint_bytes = self.CHECKPOINT_BYTES

    def load(self) -> dict[str, Table]:
        tables: dict[str, Table] = {}
        if os.path.exists(self.snapshot_path):
            self.checkpoint_bytes = max(self.checkpoint_bytes, os.path.getsize(self.snapshot_path))
            with open(self.snapshot_path, "rb") as handle:
                self.lsn, = struct.unpack("<Q", handle.read(8))
                while True:
                    frame = self._read_frame(handle)
                    if frame is None:
                        break
                    apply_ops(tables, frame)
        else:
            self._import_legacy(tables)
        self._replay_wal(tables)
        return tables

    def _import_legacy(self, tables: dict[str, Table]) -> None:
        snapshot = os.path.join(self.data_dir, "snapshot.jsonl")
        wal = os.path.join(self.data_dir, "wal.log")
        if not (os.path.exists(snapshot) or os.path.exists(wal)):
            return
        if os.path.exists(snapshot):
            with open(snapshot, "r", encoding="utf-8") as handle:
                for line in handle:
                    record = json.loads(line)
                    if record.get("magic") == "mvdb-snapshot-v2":
                        self.lsn = int(record.get("lsn", 0))
                        continue
                    kind = record.get("t")
                    if kind == "table":
                        table = Table.from_meta(record)
                        tables[table.name] = table
                    elif kind == "row":
                        table = tables.get(record.get("table", ""))
                        if table is not None:
                            table.add_row(record["row"])
        if os.path.exists(wal):
            with open(wal, "rb") as handle:
                for raw in handle:
                    if not raw.endswith(b"\n"):
                        break
                    try:
                        record = json.loads(raw)
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        break
                    ops, lsn = record["ops"], record["lsn"]
                    checksum = zlib.crc32(json.dumps(ops, ensure_ascii=False,
                                                     sort_keys=True).encode("utf-8"))
                    if checksum != record["crc"]:
                        break
                    if lsn > self.lsn:
                        apply_ops(tables, ops)
                        self.lsn = lsn
        self.write_snapshot(tables)

    def _read_frame(self, handle):
        header = handle.read(self.FRAME.size)
        if not header:
            return None
        if len(header) != self.FRAME.size:
            raise EOFError("incomplete frame header")
        size, checksum = self.FRAME.unpack(header)
        if size > os.fstat(handle.fileno()).st_size - handle.tell():
            raise EOFError("incomplete frame body")
        payload = handle.read(size)
        if zlib.crc32(payload) != checksum:
            raise ValueError("frame checksum mismatch")
        return marshal.loads(payload)

    def _replay_wal(self, tables: dict[str, Table]) -> None:
        if not os.path.exists(self.wal_path):
            return
        with open(self.wal_path, "r+b") as handle:
            while True:
                good_end = handle.tell()
                try:
                    frame = self._read_frame(handle)
                except (EOFError, ValueError):
                    # Remove the torn suffix before future commits are appended.
                    handle.truncate(good_end)
                    self._sync(handle)
                    break
                if frame is None:
                    break
                lsn, ops = frame
                if lsn > self.lsn:
                    apply_ops(tables, ops)
                    self.lsn = lsn
            self.wal_bytes = good_end

    # -- writes -------------------------------------------------------
    def _write_frame(self, handle, value) -> int:
        payload = marshal.dumps(value, 4)
        handle.write(self.FRAME.pack(len(payload), zlib.crc32(payload)))
        handle.write(payload)
        return self.FRAME.size + len(payload)

    @staticmethod
    def _sync(handle) -> None:
        handle.flush()
        os.fsync(handle.fileno())

    def append_commit(self, ops: list[dict[str, Any]]) -> None:
        lsn = self.lsn + 1
        try:
            first = not os.path.exists(self.wal_path)
            with open(self.wal_path, "ab") as handle:
                size = self._write_frame(handle, (lsn, ops))
                self._sync(handle)
            if first:
                self._fsync_dir()
        except OSError as exc:
            raise StorageError("could not commit WAL record") from exc
        self.wal_bytes += size
        self.lsn = lsn

    def write_snapshot(self, tables: dict[str, Table]) -> None:
        try:
            tmp_path = self.snapshot_path + ".tmp"
            with open(tmp_path, "wb") as handle:
                handle.write(struct.pack("<Q", self.lsn))
                for table in tables.values():
                    self._write_frame(handle, [{"t": "create_table", **table.to_json()}])
                    rows = iter(table.rows.values())
                    while batch := list(itertools.islice(rows, 256)):
                        self._write_frame(handle, [{"t": "insert", "table": table.name,
                                                   "rows": batch}])
                size = handle.tell()
                self._sync(handle)
            os.replace(tmp_path, self.snapshot_path)
            self._fsync_dir()
            with open(self.wal_path, "wb") as handle:
                self._sync(handle)
        except OSError as exc:
            raise StorageError("could not checkpoint database") from exc
        self.checkpoint_bytes = max(self.CHECKPOINT_BYTES, size)
        self.wal_bytes = 0

    def _fsync_dir(self) -> None:
        try:
            fd = os.open(self.data_dir, os.O_RDONLY)
        except OSError:
            return
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def apply_ops(tables: dict[str, Table], ops: list[dict[str, Any]]) -> None:
    for op in ops:
        kind = op.get("t")
        if kind == "create_table":
            tables[op["name"]] = Table.from_meta(op)
        elif kind == "drop_table":
            tables.pop(op["name"], None)
        elif kind == "delete_all":
            table = tables.get(op["table"])
            if table is not None:
                table.rows.clear()
                table.touch()
        elif kind == "create_index":
            table = tables.get(op["table"])
            if table is not None:
                table.indexes[op["name"]] = op["column"]
        elif kind in ("insert", "update"):
            table = tables.get(op["table"])
            if table is None:
                continue
            for row in op["rows"]:
                table.add_row(row)
        elif kind == "delete":
            table = tables.get(op["table"])
            if table is None:
                continue
            for pk in op["pks"]:
                table.rows.pop(pk, None)


# --------------------------------------------------------------------------
# database engine
# --------------------------------------------------------------------------
class Database:
    def __init__(self, data_dir: str) -> None:
        self.storage = Storage(data_dir)
        self.tables: dict[str, Table] = {}
        self.in_txn = False
        self.txn_ops: list[dict[str, Any]] = []
        self.undo: list[tuple[Table, int, list[Any] | None]] = []

    # -- lifecycle ----------------------------------------------------
    def open(self) -> None:
        self.tables = self.storage.load()

    def close(self) -> None:
        if self.in_txn:
            self.rollback()

    # -- dispatch -----------------------------------------------------
    def execute(self, sql: str, search_mode: str = "exact") -> dict[str, Any]:
        statement = Parser(tokenize(sql)).parse()
        kind = statement["type"]
        if kind == "begin":
            return self.begin()
        if kind == "commit":
            return self.commit()
        if kind == "rollback":
            return self.rollback()
        if kind == "create_table":
            return self.create_table(statement)
        if kind == "drop_table":
            return self.drop_table(statement)
        if kind == "truncate":
            return self.truncate(statement)
        if kind == "create_vector_index":
            return self.create_index(statement)
        if kind == "insert":
            return self.insert(statement)
        if kind == "select":
            return self.select(statement, search_mode)
        if kind == "update":
            return self.update(statement)
        if kind == "delete":
            return self.delete(statement)
        if kind == "noop":
            return {}
        raise DBError("unsupported_sql", f"unsupported statement {kind}")

    # -- transaction --------------------------------------------------
    def _require_no_txn_for_ddl(self) -> None:
        if self.in_txn:
            raise DBError("unsupported_sql", "DDL inside a transaction is not supported")

    def begin(self) -> dict[str, Any]:
        if self.in_txn:
            raise DBError("transaction_error", "nested transactions are not supported")
        self.in_txn = True
        self.txn_ops = []
        self.undo = []
        return {}

    def commit(self) -> dict[str, Any]:
        if not self.in_txn:
            raise DBError("transaction_error", "no active transaction")
        if self.txn_ops:
            self.storage.append_commit(self.txn_ops)
        self.in_txn = False
        self.txn_ops = []
        self.undo = []
        self._maybe_snapshot()
        return {}

    def rollback(self) -> dict[str, Any]:
        if not self.in_txn:
            raise DBError("transaction_error", "no active transaction")
        for table, pk, old in reversed(self.undo):
            if old is None:
                table.rows.pop(pk, None)
            else:
                table.rows[pk] = old
            table.touch()
        self.in_txn = False
        self.txn_ops = []
        self.undo = []
        return {}

    def _maybe_snapshot(self) -> None:
        if self.storage.wal_bytes >= self.storage.checkpoint_bytes:
            self.storage.write_snapshot(self.tables)

    def _record(self, ops: list[dict[str, Any]]) -> None:
        if self.in_txn:
            self.txn_ops.extend(ops)
        else:
            self.storage.append_commit(ops)
            self._maybe_snapshot()

    # -- DDL ----------------------------------------------------------
    def create_table(self, stmt: dict[str, Any]) -> dict[str, Any]:
        self._require_no_txn_for_ddl()
        name = stmt["name"]
        if name.lower() in {existing.lower() for existing in self.tables}:
            if stmt.get("if_not_exists"):
                return {}
            raise DBError("table_exists", f"table {name} already exists")
        columns: list[Column] = []
        pk_idx = -1
        vec_idx = -1
        for raw in stmt["columns"]:
            ctype = raw["type"]
            size = raw["size"]
            if ctype == "BIGINT":
                column = Column(raw["name"], "BIGINT", None)
            elif ctype == "VARCHAR":
                if size is None or size <= 0:
                    raise DBError("syntax_error", "VARCHAR requires a positive length")
                column = Column(raw["name"], "VARCHAR", size)
            elif ctype == "VECTOR":
                if size is None or size <= 0:
                    raise DBError("syntax_error", "VECTOR requires a positive dimension")
                if vec_idx >= 0:
                    raise DBError("unsupported_sql", "at most one VECTOR column per table")
                vec_idx = len(columns)
                column = Column(raw["name"], "VECTOR", size)
            else:
                raise DBError("unsupported_sql", f"unsupported column type {ctype}")
            if raw["primary"]:
                if ctype != "BIGINT":
                    raise DBError("unsupported_sql", "primary key must be BIGINT")
                if pk_idx >= 0:
                    raise DBError("unsupported_sql", "only one primary key is supported")
                pk_idx = len(columns)
            columns.append(column)
        table_pk = stmt.get("table_pk")
        seen_names: set[str] = set()
        for column in columns:
            lowered = column.name.lower()
            if lowered in seen_names:
                raise DBError("column_exists", f"duplicate column {column.name}")
            seen_names.add(lowered)
        if table_pk:
            if pk_idx >= 0:
                raise DBError("unsupported_sql", "duplicate primary key definition")
            lowered = table_pk.lower()
            for index, column in enumerate(columns):
                if column.name.lower() == lowered:
                    if column.type != "BIGINT":
                        raise DBError("unsupported_sql", "primary key must be BIGINT")
                    pk_idx = index
                    break
            else:
                raise DBError("column_not_found", f"unknown column {table_pk}")
        if pk_idx < 0:
            raise DBError("unsupported_sql", "a BIGINT primary key is required")
        table = Table(name, columns, pk_idx, vec_idx)
        self.tables[name] = table
        op = {"t": "create_table", "name": name,
              "columns": [c.to_json() for c in columns], "pk": pk_idx, "vec": vec_idx}
        self._record([op])
        return {}

    def drop_table(self, stmt: dict[str, Any]) -> dict[str, Any]:
        self._require_no_txn_for_ddl()
        table = None
        for candidate in list(self.tables.values()):
            if candidate.name.lower() == stmt["name"].lower():
                table = candidate
                break
        if table is None:
            if stmt.get("if_exists"):
                return {}
            raise DBError("table_not_found", f"unknown table {stmt['name']}")
        del self.tables[table.name]
        self._record([{"t": "drop_table", "name": table.name}])
        return {}

    def truncate(self, stmt: dict[str, Any]) -> dict[str, Any]:
        self._require_no_txn_for_ddl()
        table = self._table(stmt["name"])
        table.rows.clear()
        table.touch()
        self._record([{"t": "delete_all", "table": table.name}])
        return {}

    def create_index(self, stmt: dict[str, Any]) -> dict[str, Any]:
        self._require_no_txn_for_ddl()
        table = self._table(stmt["table"])
        if table.vec_idx < 0:
            raise DBError("unsupported_sql", "table has no VECTOR column")
        column_idx = table.column_index(stmt["column"])
        if column_idx != table.vec_idx:
            raise DBError("unsupported_sql", "vector index must target the VECTOR column")
        if stmt["name"] in table.indexes:
            raise DBError("index_exists", f"index {stmt['name']} already exists")
        table.indexes[stmt["name"]] = table.columns[column_idx].name
        self._record([{"t": "create_index", "name": stmt["name"],
                       "table": table.name, "column": table.columns[column_idx].name}])
        return {}

    # -- DML ----------------------------------------------------------
    def _table(self, name: str) -> Table:
        table = self.tables.get(name)
        if table is None:
            lowered = name.lower()
            for candidate in self.tables.values():
                if candidate.name.lower() == lowered:
                    return candidate
            raise DBError("table_not_found", f"unknown table {name}")
        return table

    def insert(self, stmt: dict[str, Any]) -> dict[str, Any]:
        table = self._table(stmt["table"])
        columns = stmt["columns"]
        if columns is None:
            order = list(range(len(table.columns)))
        else:
            order = [table.column_index(name) for name in columns]
            if len(set(order)) != len(order) or len(order) != len(table.columns):
                raise DBError("unsupported_sql", "INSERT column list must cover all columns once")
        prepared: list[list[Any]] = []
        seen: set[int] = set()
        for raw_values in stmt["rows"]:
            if len(raw_values) != len(order):
                raise DBError("column_count_mismatch", "value count does not match column count")
            row: list[Any] = [None] * len(table.columns)
            for slot, expr in zip(order, raw_values):
                row[slot] = self._coerce(table.columns[slot], expr)
            pk = row[table.pk_idx]
            if pk is None:
                raise DBError("primary_key_required", "primary key must not be NULL")
            if pk in seen or pk in table.rows:
                raise DBError("duplicate_key", f"duplicate primary key {pk}")
            seen.add(pk)
            prepared.append(row)
        for row in prepared:
            table.rows[row[table.pk_idx]] = row
            if self.in_txn:
                self.undo.append((table, row[table.pk_idx], None))
        table.touch()
        self._record([{"t": "insert", "table": table.name, "rows": prepared}])
        return {"affected_rows": len(prepared)}

    def update(self, stmt: dict[str, Any]) -> dict[str, Any]:
        table = self._table(stmt["table"])
        slots = []
        for column_name, expr in stmt["sets"]:
            idx = table.column_index(column_name)
            slots.append((idx, expr))
        matches = [pk for pk in sorted(table.rows) if self._match(table, table.rows[pk], stmt["where"])]
        updated: list[list[Any]] = []
        undo_entries: list[tuple[Table, int, list[Any]]] = []
        for pk in matches:
            old = table.rows[pk]
            new_row = list(old)
            for idx, expr in slots:
                new_row[idx] = self._coerce(table.columns[idx], expr, current=new_row)
            pk_value = new_row[table.pk_idx]
            if pk_value != pk:
                raise DBError("unsupported_sql", "primary key updates are not supported")
            updated.append(new_row)
            undo_entries.append((table, pk, old))
        for (table_ref, pk, _old), new_row in zip(undo_entries, updated):
            table_ref.rows[pk] = new_row
            if self.in_txn:
                self.undo.append((table_ref, pk, _old))
        if updated:
            table.touch()
            self._record([{"t": "update", "table": table.name, "rows": updated}])
        return {"affected_rows": len(updated)}

    def delete(self, stmt: dict[str, Any]) -> dict[str, Any]:
        table = self._table(stmt["table"])
        pks = [pk for pk in sorted(table.rows) if self._match(table, table.rows[pk], stmt["where"])]
        old_rows = [(pk, table.rows[pk]) for pk in pks]
        for pk, _row in old_rows:
            if self.in_txn:
                self.undo.append((table, pk, table.rows[pk]))
            del table.rows[pk]
        if pks:
            table.touch()
            self._record([{"t": "delete", "table": table.name, "pks": pks}])
        return {"affected_rows": len(pks)}

    # -- SELECT -------------------------------------------------------
    def select(self, stmt: dict[str, Any], search_mode: str) -> dict[str, Any]:
        if stmt["table"] is None:
            return self._select_constants(stmt)
        table = self._table(stmt["table"])
        items = stmt["items"]
        if len(items) == 1 and items[0]["expr"][0] == "star":
            projections = [("column", column.name) for column in table.columns]
            aliases = [column.name for column in table.columns]
        else:
            projections = []
            aliases = []
            for item in items:
                expr = item["expr"]
                projections.append(expr)
                if item["alias"] is not None:
                    aliases.append(item["alias"])
                elif expr[0] == "column":
                    aliases.append(expr[1])
                else:
                    aliases.append(self._expr_label(expr))
        alias_map = {alias: expr for alias, expr in zip(aliases, projections)}
        order_specs: list[tuple[Any, bool]] = []
        for order in stmt["order"]:
            expr = order["expr"]
            if expr[0] == "column" and expr[1] in alias_map:
                expr = alias_map[expr[1]]
            order_specs.append((expr, order["desc"]))

        if stmt["limit"] is not None and len(order_specs) == 1 \
                and order_specs[0][0][0] == "l2":
            distance_expr = order_specs[0][0]
            hits = self._fast_topk(table, distance_expr, stmt["where"],
                                   int(stmt["limit"]), order_specs[0][1])
            if hits is not None:
                result_rows = []
                for pk, distance in hits:
                    values = self._evaluate_row(table, table.rows[pk], projections,
                                                (distance_expr, distance))
                    result_rows.append([self._json_value(v) for v in values])
                return {"columns": aliases, "rows": result_rows}

        rows = [pk for pk in sorted(table.rows)
                if self._match(table, table.rows[pk], stmt["where"])]
        expressions = list(projections) + [spec[0] for spec in order_specs]
        cache = {pk: self._evaluate_row(table, table.rows[pk], expressions) for pk in rows}
        projection_count = len(projections)

        # Least-significant key first: primary key ascending breaks ties.
        rows.sort(key=lambda pk: table.rows[pk][table.pk_idx])
        for position in range(len(order_specs) - 1, -1, -1):
            _, desc = order_specs[position]
            slot = projection_count + position
            rows.sort(key=lambda pk, slot=slot: cache[pk][slot], reverse=desc)

        if stmt["limit"] is not None:
            rows = rows[: max(0, int(stmt["limit"]))]
        # ANN mode currently uses the same exact scan (recall 1.0); the flag is
        # accepted so the protocol and index maintenance tests behave alike.
        del search_mode
        result_rows = [[self._json_value(v) for v in cache[pk][:projection_count]] for pk in rows]
        return {"columns": aliases, "rows": result_rows}

    def _select_constants(self, stmt: dict[str, Any]) -> dict[str, Any]:
        columns: list[str] = []
        values: list[Any] = []
        for item in stmt["items"]:
            expr = item["expr"]
            if expr[0] != "literal":
                raise DBError("unsupported_sql", "field references need a FROM clause")
            columns.append(item["alias"] or "expr")
            values.append(expr[1])
        return {"columns": columns, "rows": [values]}

    def _expr_label(self, expr) -> str:
        if expr[0] == "l2":
            return f"L2_DISTANCE({expr[1]})"
        return "expr"

    def _evaluate_row(self, table: Table, row: list[Any], exprs: list[Any],
                      known_distance=None) -> list[Any]:
        out = []
        for expr in exprs:
            kind = expr[0]
            if kind == "column":
                out.append(row[table.column_index(expr[1])])
            elif kind == "literal":
                out.append(expr[1])
            elif kind == "alias":
                idx = expr[1]
                out.append(out[idx] if idx < len(out) else None)
            elif kind == "l2":
                if known_distance is not None and expr == known_distance[0]:
                    out.append(known_distance[1])
                    continue
                column_idx = table.column_index(expr[1])
                vector = row[column_idx]
                target = parse_vector_literal(expr[2], len(vector) // 4)
                out.append(self._distance(vector, target))
            else:
                raise DBError("unsupported_sql", "unsupported expression")
        return out

    @staticmethod
    def _distance(vector: bytes, target: bytes) -> float:
        return math.dist(memoryview(vector).cast("f"), memoryview(target).cast("f"))

    def _fast_topk(self, table: Table, expr, condition, limit: int, desc: bool):
        """Scan only qualifying rows and retain the best k by (distance, primary key)."""
        if table.vec_idx < 0 or limit <= 0:
            return None
        if table.column_index(expr[1]) != table.vec_idx:
            return None
        pks, scan = table.vector_scan()
        target = parse_vector_literal(expr[2], table.columns[table.vec_idx].size)
        if not pks:
            return []
        candidates = None if condition is None else self._candidate_slots(table, pks, condition)
        limit = min(limit, len(pks))
        if scan is not None:
            hits = _mvdb_native.topk(scan, target, candidates, limit, desc)
            return [(pks[slot], distance) for slot, distance in hits]

        slots = range(len(pks)) if candidates is None else candidates
        distances = ((pks[slot], self._distance(table.rows[pks[slot]][table.vec_idx], target))
                     for slot in slots)
        return heapq.nsmallest(limit, distances,
                              key=lambda hit: (-hit[1] if desc else hit[1], hit[0]))

    def _candidate_slots(self, table: Table, pks, condition):
        if condition[:2] == ("cmp", "="):
            left, right = condition[2:]
            if left[0] == "literal":
                left, right = right, left
            if left[0] == "column" and right[0] == "literal":
                index = table.column_index(left[1])
                if index != table.vec_idx:
                    return table.equal_slots(pks, index, right[1])
        return array.array("I", (slot for slot, pk in enumerate(pks)
                                if self._match(table, table.rows[pk], condition)))

    # -- type coercion ------------------------------------------------
    def _coerce(self, column: Column, expr, current: list[Any] | None = None):
        if expr[0] == "literal":
            value = expr[1]
        elif expr[0] == "column":
            raise DBError("unsupported_sql", "column references in VALUES are not supported")
        else:
            raise DBError("unsupported_sql", "unsupported value expression")
        if value is None:
            raise DBError("null_not_supported", "NULL values are not supported")
        if column.type == "BIGINT":
            if isinstance(value, bool) or not isinstance(value, int):
                raise DBError("type_mismatch", "BIGINT column requires an integer")
            if value < -(2 ** 63) or value > 2 ** 63 - 1:
                raise DBError("out_of_range", "BIGINT out of range")
            return value
        if column.type == "VARCHAR":
            if not isinstance(value, str):
                raise DBError("type_mismatch", "VARCHAR column requires a string")
            if column.size is not None and len(value) > column.size:
                raise DBError("value_too_long", "string exceeds VARCHAR length")
            return value
        if column.type == "VECTOR":
            if not isinstance(value, str):
                raise DBError("type_mismatch", "VECTOR column requires a string literal")
            return parse_vector_literal(value, int(column.size))
        raise DBError("unsupported_sql", f"unsupported column type {column.type}")

    # -- WHERE evaluation ---------------------------------------------
    def _match(self, table: Table, row: list[Any], condition) -> bool:
        if condition is None:
            return True
        kind = condition[0]
        if kind == "and":
            return self._match(table, row, condition[1]) and self._match(table, row, condition[2])
        if kind == "or":
            return self._match(table, row, condition[1]) or self._match(table, row, condition[2])
        _tag, op, left, right = condition
        lv = self._value(table, row, left)
        rv = self._value(table, row, right)
        if lv is None or rv is None:
            return False
        if op == "=":
            return lv == rv
        if op == "!=":
            return lv != rv
        if op == "<":
            return lv < rv
        if op == "<=":
            return lv <= rv
        if op == ">":
            return lv > rv
        if op == ">=":
            return lv >= rv
        raise DBError("unsupported_sql", f"unsupported operator {op}")

    def _value(self, table: Table, row: list[Any], expr):
        if expr[0] == "literal":
            return expr[1]
        if expr[0] == "column":
            value = row[table.column_index(expr[1])]
            return array.array("f", value) if isinstance(value, bytes) else value
        if expr[0] == "l2":
            column_idx = table.column_index(expr[1])
            vector = row[column_idx]
            target = parse_vector_literal(expr[2], len(vector) // 4)
            return self._distance(vector, target)
        raise DBError("unsupported_sql", "unsupported value expression")

    # -- JSON conversion ----------------------------------------------
    @staticmethod
    def _json_value(value):
        if isinstance(value, bytes):
            return memoryview(value).cast("f").tolist()
        if isinstance(value, float):
            if not math.isfinite(value):
                raise DBError("internal_error", "non-finite value")
            return value
        return value


# --------------------------------------------------------------------------
# protocol loop
# --------------------------------------------------------------------------
def serve(data_dir: str) -> int:
    try:
        sys.stdin.reconfigure(encoding="utf-8", errors="strict")
        sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    except (AttributeError, ValueError):  # pragma: no cover - non-standard streams
        pass
    database = Database(data_dir)
    database.open()
    stdout = sys.stdout
    stdout.write('{"type":"ready"}\n')
    stdout.flush()
    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            sys.stderr.write("invalid request line\n")
            continue
        request_id = request.get("id")
        op = request.get("op")
        if op == "shutdown":
            try:
                database.close()
                response = {"id": request_id, "ok": True}
            except Exception as exc:  # pragma: no cover - best effort
                sys.stderr.write(f"shutdown failed: {exc}\n")
                response = {"id": request_id, "ok": False,
                            "error": {"code": "internal_error", "message": str(exc)}}
            stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
            stdout.flush()
            return 0
        if op != "execute":
            stdout.write(json.dumps({"id": request_id, "ok": False,
                                     "error": {"code": "unsupported_op",
                                               "message": f"unsupported op {op}"}}) + "\n")
            stdout.flush()
            continue
        sql = request.get("sql") or ""
        mode = request.get("search_mode") or "exact"
        try:
            result = database.execute(sql, mode)
            response = {"id": request_id, "ok": True}
            response.update(result)
        except StorageError as exc:
            sys.stderr.write(f"storage error: {exc}: {exc.__cause__}\n")
            return 1
        except DBError as exc:
            response = {"id": request_id, "ok": False,
                        "error": {"code": ERROR_CODE_MAP.get(exc.code, exc.code),
                                  "message": exc.message}}
        except Exception as exc:  # pragma: no cover - defensive
            sys.stderr.write(f"internal error: {exc!r}\n")
            response = {"id": request_id, "ok": False,
                        "error": {"code": "internal_error", "message": str(exc)[:200]}}
        stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
        stdout.flush()
    database.close()
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="MiniVectorDB (stdin/stdout JSONL)")
    parser.add_argument("--data-dir", default=os.environ.get("VECTORDB_DATA_DIR", "/data"))
    args = parser.parse_args(argv)
    return serve(args.data_dir)


if __name__ == "__main__":
    raise SystemExit(main())
