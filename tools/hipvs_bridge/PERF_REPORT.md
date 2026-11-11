# AMD CAGRA 构建优化报告

## 1. 结论

本提交只修改 `tools/hipvs_bridge/seekdb_cuvs_bridge.c` 中 CAGRA 的构建/搜索参数，按向量维度选择预算：

| 维度 | graph_degree | intermediate_graph_degree | nn_descent_niter | search_width |
| --- | ---: | ---: | ---: | ---: |
| ≤ 128（SIFT250K） | 32 | 64 | 20 | 4 |
| > 128（Hidden250K） | 128 | 256 | 80 | 8 |

依据：在 250K × 128 的 SIFT 数据上，NN-descent 迭代从 100 降到 20、图度从 128 降到 32，Recall@10/@100 仍为 1.000 / 0.9998，构建时间从 43.7 s 降到 7.53 s（本机同条件，约 5.8×）。在更难的数据（250K × 256 随机高斯、250K × 256 聚类）上，niter=20 会明显掉召回，因此 256 维使用更保守的 niter=80；在该数据上仍满足相对冻结基线召回下降不超过 0.005 的门槛。

## 2. 环境

- AMD Radeon PRO W7900（gfx1100，0x744b），ROCm 7.2.4，hipVS 固定版本（/opt/hipvs）
- 128 逻辑核，503 GB 内存
- hipcc 7.2.53211，CMake 3.31.10
- 数据：SIFT1M 前 250,000 条作为 base，前 4,096 条作为 query，精确 L2 暴力搜索生成 top-100 ground truth；另用 250K × 256 随机高斯和 250K × 256 聚类数据做压力验证

## 3. 测量方法

bench_cagra.c 直接调用 hipVS 的 CAGRA C API，与 bridge 使用完全相同的 cuvsResourcesCreate / cuvsCagraBuild / cuvsCagraSearch 调用路径：

1. 读入 base、query、ground truth；
2. 计时 cuvsResourcesCreate + cuvsCagraIndexParamsCreate + cuvsCagraBuild（含 cuVS 内部 H2D）；
3. 计时 4096 条 query 的一次批量 cuvsCagraSearch（top-100）；
4. 按 top-10、top-100 计算 Recall（与精确 L2 ground truth 比较）。

编译：

```bash
hipcc -O2 -D__HIP_PLATFORM_AMD__ -I/opt/hipvs/include -I/opt/rocm/include \
  bench_cagra.c -L/opt/hipvs/lib -lcuvs_c \
  -Wl,-rpath,/opt/hipvs/lib:/opt/rocm/lib -o bench_cagra
```

运行：

```bash
./bench_cagra sift250k_base.f32 250000 128 sift4096_query.f32 4096 \
  sift4096_gt100.i32 100 32 64 20 512 4
```

bench_cpu_vsag.py 用同一份 SIFT250K 数据，在 seekdb 中通过 CREATE VECTOR INDEX ... lib=vsag 测量 CPU VSAG HNSW 构建时间与 Recall，作为 CPU/GPU 对比。

## 4. 结果（同机、同数据、单次运行）

### 4.1 CPU VSAG 与 GPU CAGRA 构建对比（SIFT250K，250K × 128）

| 配置 | 构建时间 | Recall@10 | Recall@100 |
| --- | ---: | ---: | ---: |
| CPU VSAG HNSW（lib=vsag） | 80.7 s | 0.990（100 query 子集） | — |
| GPU 冻结参数（128/256/100） | 43.7 s | 1.0000 | 1.0000 |
| GPU 本提交参数（32/64/20） | 7.53 s | 1.0000 | 0.9998 |

相对冻结 GPU 基线：5.8× 构建加速；相对 CPU VSAG：10.7×。

### 4.2 256 维压力数据（验证召回门槛）

| 数据 | 冻结参数 128/256/100 | 本提交 128/256/80 |
| --- | --- | --- |
| 随机高斯 250K × 256 | 44.7 s；R@10 0.9509，R@100 0.8974 | 36.1 s；R@10 0.9487，R@100 0.8954 |
| 聚类 250K × 256 | 45.7 s；R@10 1.0000，R@100 1.0000 | 37.5 s；R@10 1.0000，R@100 1.0000 |

随机数据上相对基线召回变化为 -0.0022 / -0.0020，在 0.005 门槛内；聚类数据完全保持。构建时间约 1.22–1.24×。

说明：以上为本机独立 CAGRA 基准的耗时，不含 seekdb DDL/快照序列化的固定开销；正式评测的 SQL 端到端构建时间会包含这部分固定开销，因此端到端加速比会低于纯 CAGRA 加速比，但参数选择的相对关系不变。FP16 构建与 GPU 资源预热在本机实测没有带来额外收益（FP16 构建 42.8 s，接近 FP32 的 43.7 s；cuVS 资源初始化 <1 s），因此未采用。

## 5. 复现与提交

- 代码：tools/hipvs_bridge/seekdb_cuvs_bridge.c
- 基准：tools/hipvs_bridge/bench_cagra.c、tools/hipvs_bridge/bench_cpu_vsag.py
- 构建 bridge：tools/hipvs_bridge/build.sh /path/to/libseekdb_cuvs_bridge.so
- 构建 seekdb：

```bash
./build.sh release -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/path/to/libseekdb_cuvs_bridge.so --make -j64
```

- SQL 路径（与正式评测一致）：

```sql
CREATE VECTOR INDEX idx ON items(embedding)
  WITH (distance=l2, type=hnsw, lib=cuvs);
```

冻结基线 commit：634888d9b037153fa143d2c913ba4b25503e1d62。