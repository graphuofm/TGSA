# TVA 对照审计（实验 / 算法 / 基线 / 功能 全清单）

_2026-09-21。逐项对照 TVA（PVLDB 19(10), p2536）§5–§7 与我们的全部实验。
每行给出 TVA 的项、我们的对应物、证据文件、状态。状态含义：
**已覆盖**=有同等或更强的已记录测量；**等价替换**=测的是同一问题但轴不同，
文中需一句话说明；**部分**=有数据未成完整对照；**缺口**=需要补。_

## 一、实验对照（按 TVA §7 逐图）

| TVA 图/节 | TVA 测什么 | 我们的对应 | 证据 | 状态 |
|---|---|---|---|---|
| 7.1 Setup | Xeon 5220/128GB，默认 5 线程；两类基线；6 数据集 + 3 基准 | 20 核 Xeon/188GB，8 线程；两类基线；14 语料（最大 1.05B，为其 52.7M 的 20 倍） | §7.1, tab:breadth | 已覆盖 |
| Fig 6(a) | 存储消耗 vs 操作量（vs AeonG/Clock-G/T-GQL，省 2.2–4.7×） | bytes/edge 跨 14 语料（65–175B）；vs LiveGraph/GraphOne RSS 列已在 CSV | results/e28_storage | 已覆盖（E28 已补，诚实读法：两胜两平，非 TVA 式压缩冠军） |
| Fig 6(b)(d) | 操作延迟 vs 操作量增长 | E21 steady：速率对累计边数平坦（LANL 到 1.05B） | e21 steady_lanl_*.csv | 已覆盖 |
| Fig 6(c) | T-LDBC 历史/当前数据分解 | arena/索引/侧车分解在 E25 RSS 阶梯 | e25.csv | 等价替换 |
| Fig 7(a)(b) | 时序查询 Q1–Q4 延迟、随操作量（206.9×） | 窗口/范围/遍历延迟随窗口宽度与语料（E9/E19/E7） | fig_temporal, e19.csv | 已覆盖 |
| Fig 7(c) | hot/warm/cold 访问频率 | E22 窗口新近度 0.1%→100% 扫描（近期窗口=hot） | e22.csv | 等价替换 |
| Fig 7(d)(e) | T-LDBC IS1–IS7，4.4–207× | 我们无 LDBC 交互查询集；对应物是校验过的窗口邻域套件 + 因果遍历 | e19/e20/e26 | 等价替换（LDBC-SNB 未定，见缺口 5） |
| Fig 7(f) | 大图压测，竞品 OOM（9.28M V/52.7M E） | LANL 1.05B 边；TVA 自身在 wiki-talk 7.8M 上 90min DNF/41.6GB | e16_lanl.csv, e15 | 已覆盖（更强） |
| Fig 7(g)(h) | vs PostgreSQL（RDBMS 代表，126.4×）与 RocksDB（KV 代表） | vs DuckDB（列存代表，E9/E22）与 RocksDB 时间编码键（E24，邻居 784–59,400×） | e24.csv, e22.csv | 已覆盖（RDBMS 槽位用列存填，需一句话说明） |
| Fig 7(i) | 非时序负载开销检查（静态图上仍快 21.7×） | E13 纯拓扑轴照实打满：中位 2.1× 于 topology-CSR（我们此轴认输） | e13*.csv, fig_structural | 已覆盖（方向相反，诚实报） |
| Fig 7(j) | 属性密集负载（T-gMark，5.1–132.9×） | E25 属性侧车：写路径近零成本 + 正确性验证；**无属性查询负载** | e25.csv, test_props | **部分（缺口 3）** |
| Fig 8(a) | 当前图存储 vs GraphOne/Stinger/Sortledton（1.4–5.1×） | E28 存储对照表（四系统同表） | results/e28_storage | 已覆盖 |
| Fig 8(b) | 插入吞吐 vs 三者（6.1×/2×/4.5×；YouTube 上 GraphOne 小胜，照实报） | E19/E20 摄入（LiveGraph 2.7–15.7×、GraphOne 0.92–2.76× 照实报）+ E26 并发版 | e19/e20/e26 | 已覆盖（更强：并发下对打） |
| Fig 9 | 4 算法归一化运行时 vs 三者（BFS/PR/SSSP/WCC，6.5×/18.4×/5.6×） | E13：bfs/pr/cc(≈WCC)+bfsw(加权扫掠=SSSP 槽位) 对四种布局；E22 三系统 compute 逐位一致 | e13*.csv + bfsw 已入 fig_structural（vs 内联权重 CSR，中位 3.8×） | 已覆盖 |
| Fig 10(a) | TemporalChain 长版本链 6.2× | 对应机制=sorted-bucket 二分：E23 threshold（关掉 2.8×/26× 悬崖） | threshold.csv | 已覆盖 |
| Fig 10(b)(c) | 持久化双模式（Pointer/Full=RocksDB 后端）开销 + WAL 开销 | E18 六档耐久阶梯 + E25 +wal 档；**外加它没有的**：50 崩溃注入、60 撕裂日志、重放曲线 29.6s/1.02B | e18/*, e25.csv | 已覆盖（超出） |
| Fig 11(a) | Zipf 偏斜下 hop 距离守界 | 结构参数守界的对应物：τ 界 [τ/2,4τ) 在 18/18 真实语料实测通过 | tau.csv | 等价替换 |
| Fig 11(b) | Hopscotch vs 版本链 2.9× | 二分 vs 线性扫描同引擎两态（E19 两态 + threshold） | e19.csv | 已覆盖 |
| Fig 11(c) | SIMD 有效性（1.4× 增益） | AVX2 边界扫描 20 格全负，默认关（诚实负结果，同一问题反向答案） | simd.csv | 已覆盖 |
| Fig 11(d) | 迁移阈值选择曲线 | 二分阈值 1–256 平台+悬崖 | threshold.csv | 已覆盖 |
| Fig 11(e) | 线程扩展近线性 | run-claim 线程曲线（开 21→73；关反转）+ E27 分片扩展 | runclaim_*.csv, e27.csv | 已覆盖（更强） |
| Fig 11(f) | 读写混合比下吞吐 | E21 rw_ratio 8:1→1:8，settled 校验全零 + E26 跨系统并发混合 | rw_ratio.csv, e26.csv | 已覆盖（更强） |
| （TVA 无） | — | 我们独有：E15 与 TVA 本尊对打（DNF 界定）；E18 故障注入；E21 disorder/visibility/retention；E22 端到端管线交叉点；E24 KV 邻居悬崖；E25 底座阶梯；E26 三系统并发零错对打；E27 分片扩展；全部跨系统比较逐条校验和 | results/ | 超出部分 |

## 二、算法对照

| 算法 | TVA（Fig 9） | 我们 | 状态 |
|---|---|---|---|
| BFS | 有 | E13 跨四布局 + E22 三系统逐位一致 | 已覆盖 |
| PageRank | 有 | 同上 | 已覆盖 |
| WCC | 有 | cc=标签传播至定点（逐位校验） | 已覆盖 |
| SSSP | 有 | bfsw 加权扫掠（SSSP 序、每步取 (dst,w)）已入 fig_structural；真 Dijkstra 因权重全 1 语义空洞，文中一句话说明 | 已覆盖 |
| 时间关联算法 | 无（快照面无法表达时间尊重路径） | forward_taint / backward_trace / earliest_arrival / temporal_triangle / 窗口 BFS，133µs CADETS 攻击链 | 我们独有 |
| 其他结构算法 | 无 | triangle / betweenness / k-core（引擎内，论文未主打） | 超出，不必进论文 |

## 三、基线对照

| 槽位 | TVA 用谁 | 我们用谁 | 状态 |
|---|---|---|---|
| 版本/时序图存储 | Clock-G, T-GQL, AeonG | TVA 本尊（作者工件，修复脚本存档） | 已覆盖（打的就是它） |
| RDBMS 代表 | PostgreSQL | DuckDB（列存代表，带 ts 索引） | 等价替换，文中说明 |
| KV 代表 | RocksDB（其 Full Mode 后端同款） | RocksDB v9.7.4 时间编码键（其键设计同族） | 已覆盖 |
| 动态/流式图存储 | GraphOne, Stinger, Sortledton | GraphOne, LiveGraph（均逐条校验；并发 E26） | 已覆盖（Stinger/Sortledton 未测，见缺口 4） |
| 静态参照 | 无 | tcsr / ucsr / topology-CSR 三个 CSR 参照系 | 超出 |

## 四、功能对照

| 功能 | TVA | 我们 | 状态 |
|---|---|---|---|
| 数据模型 | 属性图版本（少量属性丰富版本/实体） | 不可变事件多重集（时间即地址） | 定位互补，§2 已声明 |
| CRUD | 全支持，delete=特殊 update | append + `remove()` 软墓碑（weight=NaN）+ 保留策略；无就地 update（模型不需要） | 已声明边界 |
| 属性 | 列存属性 + Temporal Buffer 历史值 | 属性侧车（dock 模块，写路径近零成本，T5 覆盖） | 已覆盖（查询负载见缺口 3） |
| 事务/一致性 | 细粒度锁的 Snapshot Isolation | 逐边原子可见，无多边事务（§6 直说） | 已声明，模型不需 SI |
| as-of 快照 | 核心强项（NextOffset 版本跳跃） | 枚举归约，CADETS 217.9s，照实认输 | 已声明（可选补：逐邻居最新算子） |
| 时间范围查询 | FOR TT FROM t1 TO t2 | 窗口/范围/聚合，O(W+Wµ+\|R\|) | 已覆盖 |
| 时间尊重路径 | 公开接口不可表达 | 原生（taint/trace/earliest-arrival） | 我们独有 |
| 持久化 | WAL(CRC32) + Pointer/Full 双模式 | 帧校验 WAL + 六档耐久 + 恢复=重放定理 + 故障注入实证 | 已覆盖（超出） |
| 保留/退场 | 无原生（版本永留） | evict_before=页释放，E10/E21 retention 稳态 | 我们独有 |
| SIMD | 有效（+1.4×） | 测了，负收益关掉入册 | 已覆盖（反向诚实） |
| 并发 | SI 锁 + 线程扩展 | 无锁多写者 + 发布协议定理 + settled 校验 + 跨系统并发对打 | 已覆盖（超出） |
| 横向扩展 | 无 | E27 分片可分解性（live 实体轴 / replay 双轴） | 我们独有 |
| 可复用抽象 | 无独立抽象 | tmap 五条款 + 非图客户端证明 | 我们独有 |

## 五、缺口清单（按优先级，做完勾销）

1. ~~跨系统存储消耗对照~~ 已补：results/e28_storage（2026-09-21）。
2. ~~SSSP~~ 已补：E13 早有 bfsw（加权扫掠，SSSP 的诚实替身，真 SSSP 在
   这些语料上因权重全 1 而语义空洞），171 行入册数据画进 fig_structural
   第四条线，图注和 7.3 各加一句（2026-09-21）。
3. ~~属性查询负载~~ 已补：E29（2026-09-22）。共存 ≤3%，侧车谓词不慢于
   记录内字段，全部选择率校验通过。results/e29_props。
4. ~~Stinger / Sortledton~~ 已补：E31/E32（2026-09-22）。两者按 (src,dst)
   键控、重复对=就地更新，语义边界照 TVA 只比当前图操作；发现 Stinger 在
   高重复对流上三个数量级退化（cadets 0.02 Meps），正中论点。
   results/e31_streaming_baselines。
5. ~~LDBC-SNB~~ 已补（2026-09-22）：BI SF10 四张动态关系表转 65.7M 事件
   三年流（tools/ldbc_to_bin.cpp），进语料表 Interaction streams 族，
   TGSR 58.1 Meps / 74 B/边，E31/E32 也在其上跑。
6. ~~逐邻居最新版本算子~~ 已补：E30（2026-09-22）。CADETS 全图 as-of
   219.0s -> 0.27s（803x），校验和与地面真值一致；论文 as-of 段已改写，
   剩余让步只在渐近复杂度。results/e30_asof。

## 六、图表完整性复核（2026-09-29，Opus 5）

按 TVA 的 float 清单逐一对照（它：11 图 / 1 表 / 3 算法 / 2 定理 / 2 例）：

| TVA float | 我们的对应 | 状态 |
|---|---|---|
| Fig 1 金融时序图动机例（配 Example 1） | fig:motivation + ex:cadets | 有 |
| Fig 2 hopscotch 机制前后对照小图 | **fig:runclaim（本轮新增，run-claiming 前后对照）** | 已补 |
| Fig 3 总体架构（Example 2 色锁） | fig:arch + ex:queries | 有 |
| Fig 4 顶点属性存储格式 | fig:format（与 Fig5 合并成一张） | 有 |
| Fig 5 动态拓扑存储格式 | 同上 | 有 |
| Fig 6(a) 存储消耗 | **tab:storage（本轮新增，E28）** | 已补 |
| Fig 6(b)(d) 操作延迟随操作量 | E21 steady（正文述，无图） | 文字覆盖 |
| Fig 7(a–j) 时序查询延迟 10 面板 | fig:temporal(3)+fig:baselines(3)+fig:pipeline(2) | 有 |
| Fig 8(a) 当前图存储 | tab:storage | 已补 |
| Fig 8(b) 插入吞吐 | fig:ingest(2) + 7.2 Stinger/Sortledton 段 | 有 |
| Fig 9 四算法归一化 | fig:structural（四算法一图） | 有 |
| Fig 10 持久化 3 面板 | fig:recovery(2) | 有 |
| Fig 11 性能/消融 6 面板 | fig:ablation(4) | 有 |
| Table 1 复杂度 | tab:complexity | 有 |
| Alg 1 写路径 CRUD | alg:publish | 有 |
| Alg 2 结构维护（hop update） | alg:shift | 有 |
| Alg 3 读路径取边（含 SIMD） | **alg:asof（本轮新增）** | 已补 |
| Thm 1–2 | 五条定理 | 超出 |
| Ex 1–2 | ex:cadets, ex:queries | 有 |
| （TVA 无） | tab:breadth 语料表 | 超出 |

本轮修复的三件事：
1. **E31/E32 段落实际从未落盘**（上一轮 str.replace 目标串已被更早的改动
   替换，静默跳过，被误报为完成）。已补写并逐条核验入 PDF。
2. 新增 Algorithm 3（as-of 算子读路径伪码），填上 TVA Alg 3 的槽位；
   7.3 的 as-of 段改为引用它。
3. 新增 Table 3（四系统常驻内存，E28），填上 TVA Fig 6a/8a 槽位；
   7.2 增"Storage"小段。附带补 E29 属性侧车读路径结果进 7.5，
   并修掉 §5.5 里陈旧的"2.4×/九组配对"（正解 2.1×/29 组）。

仍未进正文（有数据、有 README，等用户通读后共同决定）：
E25 底座阶梯、E26 三系统并发对打、E27 分片扩展。这三项是用户明确
"先存脚本和结果，文章等我看完再定怎么编进去"的部分，不擅自加。
