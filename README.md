# MiniVectorDB

单机、单进程、单连接的轻量向量数据库实现，用于 `2026-competition-round2`。
程序以 stdin/stdout JSON Lines 协议与评测驱动通信，数据持久化在 `/data`。

## 1. 目录

```text
minivectordb/minivectordb.py   # 数据库实现（SQL 解析、存储、事务、恢复、向量检索）
Dockerfile                     # 评测镜像（python:3.12-slim + numpy）
mysqltest/                     # 官方公开用例（未改动）
protocol_check/                # 官方协议自测工具（未改动）
STDIO_PROTOCOL.md              # 官方协议说明（未改动）
agent/conversation.json        # Agent prompt 学习材料
```

## 2. 构建与运行

```bash
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest
```

容器启动后先完成 /data 恢复，输出 `{"type":"ready"}`，随后逐行读取请求：

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, embedding VECTOR(2));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1, '[3,4]');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding,'[0,0]') AS d FROM docs ORDER BY d LIMIT 1;","search_mode":"exact"}
{"id":4,"op":"shutdown"}
```

对应响应：

```json
{"type":"ready"}
{"id":1,"ok":true}
{"id":2,"ok":true,"affected_rows":1}
{"id":3,"ok":true,"columns":["id","d"],"rows":[[1,5.0]]}
{"id":4,"ok":true}
```

本地不构建镜像也可以直接跑：

```bash
python minivectordb/minivectordb.py --data-dir ./data
```

公开自测（Linux，需 Docker）：

```bash
python3 protocol_check/run.py --image minivectordb:latest --handshake-only --output protocol-results/handshake
python3 protocol_check/run.py --image minivectordb:latest --output protocol-results/public
```

## 3. SQL 支持范围

| 类别 | 支持 |
| --- | --- |
| 表结构 | `CREATE TABLE [IF NOT EXISTS] t (id BIGINT PRIMARY KEY, name VARCHAR(n), v VECTOR(d))`；内联或表级 `PRIMARY KEY (id)`；`NOT NULL`；单 BIGINT 主键；每表至多一个 VECTOR 列 |
| 写入 | `INSERT INTO t VALUES (...), (...)`（`VALUE` 亦可，可带列清单）；自动提交 |
| 查询 | `SELECT *` / 列投影 / `AS` 别名；`SELECT 1` 常量查询；`WHERE` 的 `= < <= > >= !=`、`AND`、`OR` 与括号；`ORDER BY expr [ASC|DESC]`；`LIMIT` |
| 向量 | `L2_DISTANCE(col, '[..]')`；精确 TopK；过滤后 TopK；距离并列按主键升序 |
| 索引 | `CREATE VECTOR INDEX name ON t(v)`（记录索引元数据，ANN 模式下仍保证正确性） |
| 修改 | `UPDATE t SET c = v WHERE ...`；`DELETE FROM t WHERE ...` |
| 其它 | `DROP TABLE [IF EXISTS]`、`TRUNCATE [TABLE]`、`USE`/`SET`（空操作）、`--`/`/* */` 注释 |
| 事务 | `BEGIN [WORK|TRANSACTION]` / `COMMIT` / `ROLLBACK`，事务内读到自己的修改 |
| 不支持 | `ALTER TABLE`、`JOIN`、聚合、子查询、NULL、主键更新、嵌套事务 → 返回 `unsupported_sql` 等错误，不静默执行 |

数值语义：BIGINT 为 64 位整数；VARCHAR 长度按 Unicode 码点计；字符串比较按 Unicode 码点；
向量先按 float32 存储再计算距离，拒绝 NaN/Infinity/维度不符/float32 溢出；距离为欧氏距离。

错误响应示例：

```json
{"id":3,"ok":false,"error":{"code":"1062","message":"duplicate primary key 1"}}
```

`error.code` 使用 MySQL 风格数字码：`1062` 主键冲突、`1064` 语法错误、`1050` 表已存在、
`1054` 未知列、`1146` 表不存在、`1235` 不支持、`1366` 类型/维度不符、`1406` 超长、
`1105` 内部错误。语义名（如 `duplicate_key`）保留在内部，便于和题包的 `error_codes` 映射对齐。

## 4. 存储、事务与恢复

`/data` 下两个文件：

- `snapshot.jsonl`：快照，首行 `{"magic":"mvdb-snapshot-v2","lsn":N}`，随后每表一行元数据、每行一条记录；
- `wal.log`：追加式 redo 日志，每行一个已提交事务：`{"lsn":N,"crc":32位校验,"ops":[...]}`。

写路径：自动提交语句或 `COMMIT` 先把 redo 记录写入 WAL 并 `fsync`，成功后才返回响应；
随后按需（WAL 超过 2000 条操作）写快照并截断 WAL。快照通过临时文件 + `os.replace` 原子切换。

读路径：启动时加载快照，再从 WAL 逐行重放 `lsn` 更大的记录；
末尾不完整或校验失败的行会被丢弃（进程被强杀时可能留下半行）。
重放是幂等的：恢复途中再次被强杀，下次启动仍从同一快照重放。

事务：`BEGIN` 后的修改直接作用于内存并记录 undo；`COMMIT` 落 WAL 后清空 undo；
`ROLLBACK` 逆序恢复；正常 `shutdown` 会先回滚未提交事务再写快照。

## 5. 向量检索

- 精确模式（默认）：过滤后的候选集合内全量计算距离，按 `(距离, 主键)` 排序取 TopK；
  有 numpy 时使用向量化扫描（缓存 float32 矩阵与列数组），无 numpy 时退化为纯 Python。
- ANN 模式：当前同样返回精确结果（Recall@10 = 1.0）。索引元数据会被持久化，
  `CREATE VECTOR INDEX` 后新增/更新/删除记录立即对检索可见。后续可在同一入口替换为 IVF/HNSW。

## 6. 已验证结果（本地，2026-11-17）

- 官方公开用例 **20/20 通过**（`mysqltest/manifest.json`，Windows 本地管道复现，与官方断言模块同一套比较逻辑）。
- 额外验证：强杀后已提交数据保留、未提交事务丢弃；NaN/Infinity/维度不符/float32 溢出被拒绝；
  VARCHAR 码点长度；距离并列按主键升序；过滤后 TopK；ANN 过滤结果正确。
- 小规模基准（本机单进程，10,000 条 × 128 维，1,000 次 TopK=10，一半带等值过滤）：
  写入 2.5 s、查询 3.1 s（约 3.1 ms/查询）、重启并 ready 0.5 s。

## 7. 已知限制

1. 数据全部驻留内存，单机数据规模受内存限制；超大表需要改为磁盘行存 + 分页扫描。
2. ANN 模式当前复用精确扫描，召回率没有问题，但大表查询延迟不会优于精确扫描。
3. 不支持 NULL、JOIN、聚合、子查询、ALTER TABLE、多向量列、主键更新与嵌套事务。
4. DDL 不能在显式事务内执行（返回错误），避免部分 DDL 与事务日志交错。
