# 提测信息

| 字段 | 值 |
| --- | --- |
| 仓库 HTTPS URL | `https://github.com/asdjqw/seekdb` |
| 分支 | `2026-competition-round2` |
| 完整 commit SHA | 提交前执行 `git rev-parse HEAD`（本文件不写死，避免过期） |
| 交付目录 | 仓库根目录（`minivectordb/`、`Dockerfile`、`agent/conversation.json`、`README.md`） |

## 提交前自检

```bash
git rev-parse HEAD            # 记录 40 位 SHA
git status --short            # 必须干净
git branch --show-current     # 2026-competition-round2

# 本地功能验证（Linux 或 Windows 均可；Windows 用 check 目录里的等价驱动）
python check/local_judge.py           # 期望 20/20 cases passed
python check/extra_tests.py           # 期望 all extra checks passed
python check/bench.py                 # 10k x 128 维性能探针

# 官方协议自测（Linux + Docker）
docker build -t minivectordb:latest .
python3 protocol_check/run.py --image minivectordb:latest --handshake-only --output protocol-results/handshake
python3 protocol_check/run.py --image minivectordb:latest --output protocol-results/public
```

## 材料对应

| 材料 | 路径 |
| --- | --- |
| 源码 | `minivectordb/minivectordb.py` |
| Dockerfile | `Dockerfile` |
| Agent prompt 材料 | `agent/conversation.json`（≤128 KiB） |
| 运行说明 | `README.md` |
| 公开用例 | `mysqltest/`、`protocol_check/`（官方题包原样保留） |
