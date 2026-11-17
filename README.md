# MiniVectorDB

单机、单进程、单连接的轻量向量数据库实现，用于 `2026-competition-round2`。
程序以 stdin/stdout JSON Lines 协议与评测驱动通信，数据持久化在 `/data`。

## 1. 目录

```text
minivectordb/minivectordb.py   # 数据库实现（SQL 解析、存储、事务、恢复、向量检索）
minivectordb/native.c          # 向量字面量解析和精确 TopK 的 C 扩展
Dockerfile                     # Linux x86_64 多阶段构建，运行时为 CPython 3.12
mysqltest/                     # 官方公开用例（未改动）
protocol_check/                # 官方协议自测工具（未改动）
STDIO_PROTOCOL.md              # 官方协议说明（未改动）
agent/conversation.json        # Agent prompt 学习材料
verify/                        # 容器性能对照及回归检查
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

本地可以用 CPython 3.12 直接跑。未安装 C 扩展时，向量检索使用 Python 实现。

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

`/data/wal.v3` 保存追加式 redo 日志。每笔自动提交或显式事务对应一帧，帧头记录长度和 CRC32，帧体记录提交序号及操作。结构化数据使用 CPython marshal v4 编码，向量直接保存为 float32 字节串。数据库复用 WAL 写入句柄，每次追加在 fdatasync（或平台的 fsync）完成后才确认提交。已同步且完整的 WAL 页可以释放文件缓存，未对齐的尾页仍保留。

`/data/snapshot.v3` 保存检查点。WAL 至少增长到 64 MiB，或达到更大的现有快照大小，才触发检查点。数据库分批写出表定义和行数据，同步临时文件并原子替换快照，同步目录后再截断 WAL。正常关闭时回滚未提交事务，已同步的 WAL 可以直接用于下次恢复。

启动时先恢复快照，再重放序号更新的完整 WAL 帧。恢复遇到残缺的尾帧时截断尾部，后续提交可以继续追加。启动可导入旧版 `snapshot.jsonl` 和 `wal.log`，导入成功后以 v3 文件为准。

事务内修改作用于内存，并记录旧行和 redo 操作。`COMMIT` 同步整帧后清空事务状态，`ROLLBACK` 逆序恢复旧行。写入失败导致提交状态不确定时，进程退出，由下次启动恢复完整提交。

## 5. 向量检索

- 精确模式先处理 `WHERE`，可复用普通列等值分组，再对候选向量计算距离。C 扩展引用行内已有的 float32 字节，在支持 AVX2 的 CPU 上用 SIMD 执行 float64 距离运算，有界堆按 `(距离, 主键)` 选出 TopK。
- 支持 AVX2 和 FMA 的 CPU 可用 float32 快速估算距离。误差界能排除一定进不了 TopK 的候选，其他候选计算 float64 距离。极端值及舍入敏感的排序保留高精度计算。
- 当舍入误差可能改变入选或排序时，使用扩展精度和补偿求和复算距离。
- 事务内的写入、回滚及其他行变动会使候选缓存失效。ANN 模式复用精确扫描，`CREATE VECTOR INDEX` 的元数据随事务日志和检查点保存。
- 简单的批量 `INSERT ... VALUES` 直接读取有类型的字面量，复用通用的类型检查和事务提交。重复的距离查询可复用语法模板，每次请求绑定自己的向量参数。

## 6. 附加验证

在 Linux 上运行以下命令。`mvdb-baseline` 是上一版提交 `0ee61ab` 的镜像，`minivectordb:latest` 是本版镜像。测试旧 JSON 格式迁移时使用 `0b1db0d` 的镜像。

```bash
python3 verify/regression.py minivectordb:latest --legacy mvdb-baseline
python3 verify/benchmark.py mvdb-baseline minivectordb:latest --rounds 3 --protocol-probe 100
python3 verify/benchmark.py minivectordb:latest --rounds 1 --seed 91 --shuffle-keys --exponential --oracle-queries 1000
```

基准生成 10,000 条 128 维向量，每批写入 100 条，执行 1,000 次 TopK 查询，其中一半带等值过滤。脚本检查所有返回行的数量、排序、唯一性、过滤条件及距离，默认对 40 个查询独立全量计算真值，并检查重启后的向量值。输出包括有过滤和无过滤两组延迟、端到端用时、容器 CPU 时间、峰值内存、设备 I/O 和数据文件大小。

本地 WSL2 Linux 容器通过官方握手和 20/20 个公开用例。`0ee61ab` 和本版交错测量的三轮中位数为写入 0.400 s → 0.345 s、查询 0.831 s → 0.684 s、查询阶段峰值内存 24.6 MiB → 21.1 MiB。另一组乱序主键和科学计数法输入通过了 1,000 个查询的独立全量真值校验。赛题的正式 E 以平台测量为准。

## 7. 已知限制

1. 行数据驻留内存，最大数据规模受容器内存限制。
2. ANN 模式当前复用精确扫描，召回率没有问题，但大表查询延迟不会优于精确扫描。
3. 不支持 NULL、JOIN、聚合、子查询、ALTER TABLE、多向量列、主键更新与嵌套事务。
4. DDL 不能在显式事务内执行（返回错误），避免部分 DDL 与事务日志交错。
