# SIGMOD 长文方案：以 TVA (PVLDB 19(10), 2536-2548) 为模板逐节仿写

工作文档，中文。论文本体为英文。基于 2026-09-16 对 p2536-li.pdf 的逐页精读。

---

## 一、TVA 的解剖：它每一节在干什么、占多少版面、用了什么写作手法

13 页正文，11 张图 + 1 张表，约 40 个实验面板。

| 节 | 版面 | 内容 | 值得偷的手法 |
|---|---|---|---|
| Abstract | ~200词 | 问题 → 三个点名的机制 → 头条数字 (9.9x, 2.2x) | 机制在摘要里就有名字 |
| §1 Intro | 1.25页 | 图1金融欺诈实例 + Example 1 叙事 → 逐个数落现有系统 → **三个编号挑战 ❶❷❸** → 三个机制各自回应一个挑战 → 五条贡献列表 | 挑战编号、机制-挑战一一对应 |
| §2 Background | 1页 | §2.1 形式化模型 + 操作语义 + 查询公式 Eq(1)；§2.2 hopscotch 入门 + 为什么直接套用不行 | 借来的技术先科普再说不够用 |
| §3 Overview | 0.75页 | 大架构图 (图3, 查询流程用颜色叠在图上) + Example 2 拿两条具体查询走一遍图 | **Example 和图用颜色互相锁定** |
| §4 Storage | 2.5页 | 每小节以 **"Key Idea."** 段开头；结构全部起名并用打字机字体 (Temporal Table, NextOffset...)；格式图 (图4/5)；CRUD 伪代码 (Algo 1/2)；**Discussion 段明确划清与原始 hopscotch 的差异** | Key Idea 段式；结构命名即贡献 |
| §5 Query Engine | 1.5页 | 查询流程 + **每种操作一段 I/O Analysis** + **Table 1 复杂度总表** + 定理1/2（证明放 github）+ SIMD 带真实 intrinsics 的 Algo 3 | 复杂度表；证明放仓库 |
| §6 Impl | 0.5页 | 7000行C++；**Snapshot Isolation**（细粒度锁在 Header 上）；WAL 帧格式（magic+ts+len+CRC32）；PersistManager 两种模式 | 一致性模型明说 |
| §7 Eval | 3.5页 | §7.1 **基线分两类**（时序类5个 + 动态图类3个）+ 公平性工程明写（全部驻内存、统一保留完整版本历史）；§7.2 时序主战场；§7.3 **它的弱轴（当前图）也打满一节，输的地方带机理解释**；§7.4 持久化；§7.5 **每个机制一小节的 ablation** | 两类基线；弱轴不躲；ablation 按机制切分 |
| §8 Related | 0.5页 | 四组，每组末尾一句划界 | |
| §9 Conclusion | 5行 | | |

**它的输也写得漂亮**（我们照学）：
- "GraphOne marginally outperforms TVA on YouTube, mainly because most edges connect to a few high-degree vertices, causing more hash collisions"
- "Sortledton outperforms others in BFS because its skiplist structure makes it convenient to access all neighbors"
- "An exception occurs on IMDB and Epinions... We consider this overhead acceptable because the reserved space is a key design choice that enables high insertion throughput"

每一句输 = 数字 + 机理 + （可选）为什么这个代价是设计选择。没有一句是干巴巴的认怂。

---

## 二、我们的抽象（可被引用之物）：Temporal Address Space，原语叫 `tmap`

TVA 留下的可引用物是"版本元数据与值分离 + 有界邻域"这条设计原则。GraphOne 留下 GraphView。我们留下什么？

**提案：把 mmap 对文件做的事，对时间做一遍。**

`tmap(T0, S, P)` 返回一个区域，其中 **地址 = A_base + ((T − T0) >> S) × P**。契约（即卖点）：

1. **定位是算术**：任意时刻的宿位 O(1) 最坏情况、无数据依赖访存（定理1）
2. **常驻正比于被占用的时间，而非跨越的时间**：稀疏由 MMU 吸收（定理2）
3. **时间前缀 = 地址前缀**：保留期就是一次 madvise（保留定理）
4. **区域内容是追加多重集的纯函数**：恢复 = 重放（定理6）
5. **逐槽发布协议**：并发写入期间读永不见半写（定理4）

**为什么这是抽象而不是公式**：别人可以往每个时间窗里放*他们自己的东西*——邻接块（我们的图存储只是第一个客户）、每窗 sketch/计数器/Bloom、时间键 KV——并自动继承全部五条契约。论文 §3 定义 tmap 契约 + 定理，§4 把图存储作为客户建在其上。这样"层"清晰了，可引用之物也有了。

审稿人一句话总结应当是："they tmap the stream" —— 就像 "they mmap the file"。

## 三、系统名字

**建议：保留 Gnomon，给抽象命名（tmap / Temporal Address Space），不给系统换名。**

理由：
1. GraphOne 从 FAST'19 到 TOS'20 没换名，TVA 也是一名到底。短文→长文换名会切断引用连续性，还显得在掩饰扩展关系（扩展版反而需要明示前身）。
2. FlashGraph 已被 FAST'15 占用（Zheng et al., SSD 图处理），撞在同一个领域同一个会。
3. 已核实撞名的候选：Chronos（EuroSys'14，TVA 引文[11]）、Sundial（VLDB'18）、ChronoGraph、Arrow。

如果坚持换，候选（**用前必须过 DBLP**）：Obelisk（方尖碑本身就是巨型 gnomon）、Solstice、Heliograph（含 graph，暂未见撞名）。我不推荐冒这个险。

---

## 四、逐节对应：TVA 怎么写 → 我们怎么写 → 现状

| TVA | 我们 | 现状 |
|---|---|---|
| 图1 金融欺诈 + Example 1 | 图1 **CADETS 攻击链真实故事**：t 时刻告警 → 反向因果追踪 133µs → 保留期视界正在逼近证据。我们有真数据（749 红队告警、0.0093% 影响集） | 要画 |
| 挑战 ❶❷❸ | ❶ 不维护索引却按时间定位 ❷ 摄取中并发读的正确性且永不排序 ❸ 无界流上的有界资源（保留+持久+恢复为推论） | 要写 |
| §2.1 模型+Eq(1) | 事件图模型；time-respecting 路径/窗口查询形式化；操作=append/evict/replay | THEORY.md 有底子 |
| §2.2 hopscotch 入门 | demand paging / 匿名 mmap 入门 + 为什么 CIDR'22 的 mmap 批评不适用 | 短文有雏形，扩写 |
| §3 架构图+Example 2 | **§3 = tmap 抽象**（契约五条+定理1/2）+ 架构图 + Example 2 用颜色走两条查询（一条窗口查询、一条反向追踪） | 全新，核心增量 |
| §4 Key Idea + 格式图 + Algo | timeline/arena/vertex-index 三个格式图；Algo: 移位选择、发布协议、run-claim 批量认领；**EdgeRef 16→24 字节的决策连同测量写进去**（TVA 式 Discussion 段） | 材料全有 |
| §5 + Table 1 + I/O Analysis | 复杂度总表直接从 THEORY.md §1 搬；每操作 I/O Analysis；定理3/4/5/6；SIMD 边界扫描带 intrinsics | 材料全有 |
| §6 SI + WAL 格式 | **一致性模型要诚实写**：逐边原子可见（发布协议），无多边事务——这是和 TVA(SI) 的真实差异，明说而不是回避 | 要写清 |
| §7.1 两类基线 | 时序类：TVA、DuckDB(+ts索引)、(考虑 AeonG)；流式类：GraphOne、LiveGraph、(考虑 Sortledton)；+ 三个CSR。公平性工程照抄它的写法（in-process、checksum 校验、给足索引） | 大部分已跑 |
| §7.2 主战场 | 窗口查询、时序遍历、E22 端到端流水线（窗口扫出 70x→1.0x 交叉全曲线） | 已有 |
| §7.3 弱轴打满 | E13 结构轴 1.3–6.1x 中位 2.4x + 全图 as-of 快照 218s 让给 TVA | 已有 |
| §7.4 持久化 | E18 全套：**崩溃注入 50/50、截断/位翻转 60/60、重放曲线到 10.24亿边 29.6s**——TVA 只测了开销没做故障注入，这里我们反超它的模板 | 已有 |
| §7.5 ablation 按机制 | run-claim 开/关（14.5→52.8）；EdgeRef 内联开/关（含结构轴代价 2.1→2.4）；THP 开/关（负结果）；小桶阈值；τ 扫描；乱序窗口扫描；线程扩展；读写混合比 | **数据全在这个 session 里，缺开关化整理** |
| §8 四组 related | 版本化时序（TVA/AeonG/Aion）；结构动态（+RapidStore/RadixGraph 划界）；LSM/时序存储谱系；provenance | 调研已备好 |

## 五、相对短文必须新增的工作（按依赖排序）

1. **抽象层落地**：把 tmap 从叙述变成代码里真实的层（现在 Timeline+Arena 事实上就是，需要提接口 + 一个非图客户端做证明，比如 per-window 计数器/sketch，几百行）
2. **ablation 开关化**：run-claim、inline-ts 目前是提交序列里的前后版本，要做成编译/运行开关才能出干净的 ablation 图
3. **规模**：Kron 生成器到 ~40 亿边（GraphOne 的量级）
4. **并发头对头**：GraphOne stream view / LiveGraph MVCC 读者在写入下的对比（我们只测过自己）
5. **图1 + 架构图 + 两张格式图**（TVA 图3/4/5 的质量标准）
6. **LDBC 考虑**：TVA 用 T-mgBench/T-LDBC/T-gMark；我们真语料更强，但可加一个 LDBC-SNB 使可比性无懈可击
7. 时间线：FAST 通知 12-08；SIGMOD 轮次在其后（不可同时在审）

## 六、版面预算（对齐 TVA 的 13 页）

Intro 1.25 / 背景+模型 1 / §3 抽象 1 / §4 存储 2.5 / §5 查询 1.5 / §6 实现 0.5 / §7 评测 4 / §8 相关 0.75 / 结论 0.25 ≈ 12.75 页正文。

---

## 七、TVA 实验逐图对照契约（TVA 做了 1，TGSR 也做 1）

定案（2026-09-17）：系统名 **TGSR**（temporal graph storage and read，已核查图/存储/DB 领域无撞名）。
不做 40 亿边（TVA 最大 5270 万边，我们 LANL 10.5 亿已是它的 20 倍）。
身份：**面向 temporal/dynamic graph 的时间敏感大吞吐引擎**；结构主导负载天生会输，照实报，
卖点是给下游应用省时间（E22 端到端就是这个论证）。

| TVA 图/节 | 内容 | TGSR 对应 | 状态 |
|---|---|---|---|
| Fig 6(a)(c) | 存储消耗 vs 操作量；历史/当前分解 | B/edge 跨 13 语料 + vs LiveGraph/GraphOne RSS；历史=arena、索引分解 | 有数据，缺分解图 |
| Fig 6(b)(d) | 操作延迟 vs 操作量 | E21 steady：速率 vs 累计边数（平坦性） | 已有 |
| Fig 7(a)(b) | 时序查询 Q1–Q4、随操作量 | 窗口查询/遍历/范围过滤（E19/E7/E9） | 已有 |
| Fig 7(c) | hot/warm/cold 频率 | E22 窗口新近度扫描 0.01%→100% | 已有 |
| Fig 7(d)(e)(f) | 大图压测、竞品 OOM | wiki-talk 上 TVA 90min DNF @41.6GB；LANL 10.5亿 | 已有 |
| Fig 7(g)(h) | vs 通用系统 PostgreSQL/RocksDB | vs DuckDB(+ts索引)；RocksDB v9.7.4 时间编码键 E24（0 校验不符；邻居差 784–59,400x） | 已有 |
| Fig 7(i) | 非时序负载开销检查 | E13 纯拓扑轴（我们的弱轴打满） | 已有 |
| Fig 7(j) | 属性密集负载 | 不适用（我们无属性，§6 诚实声明数据模型边界） | 声明代替 |
| Fig 8(a)(b) | 当前图存储/插入 vs GraphOne/Stinger/Sortledton | E19/E20 vs GraphOne/LiveGraph（checksum 校验） | 已有 |
| Fig 9 | BFS/PR/SSSP/WCC 归一化 | E22 compute 三系统逐位一致 + E13 分解 | 已有 |
| Fig 10(a) | TemporalChain 长版本链有效性 | 压缩 vs 未压缩查询代价随度数（26→1500 vs 54→15ns 曲线） | 已有 |
| Fig 10(b)(c) | 持久化两模式开销 + WAL 开销 | E18 cost 六档阶梯（none→fsync/batch）；**外加 TVA 没有的：崩溃/截断注入、重放曲线** | 已有+超出 |
| Fig 11(a) | Zipf 偏斜下 hop 距离（结构参数在真实偏斜下守界） | E23 tau 扫描：μ∈[τ/2,4τ) 18/18 全过，查询代价对 τ 平坦 | 已有 |
| Fig 11(b) | vs 版本链基线加速 | sorted-flag 二分 vs 线性扫描（同一引擎两态） | 已有（E19 两态） |
| Fig 11(c) | SIMD 有效性 | AVX2 边界扫描开/关：20 格全负，默认关（诚实负结果=TVA 对齐的反向版） | 已有 |
| Fig 11(d) | 迁移阈值选择 | E23 threshold：1–256 平台 + 关闭悬崖（wiki 2.8x、CADETS 26x） | 已有 |
| Fig 11(e) | 线程扩展 | runclaim A/B 线程扩展曲线（开：21→73；关：12→6 反转）fig_ablation(a) | 已有 |
| Fig 11(f) | 读写混合比 | rw_ratio 8:1→1:8 已测，settled 校验全 0，fig_ablation(d) | 已有 |
| （无对应） | — | **TGSR 独有 ablation**：run-claim 开/关（73.2 vs 6.1 @8T，已记录 runclaim_*.csv）、ts 缓存 16/24B 双构建（时序 2.1–5.5x，结构轴噪声内）、THP（负结果关闭） | 已有 |

### 需要新做的引擎开关
1. `run_claim`（运行时，GraphConfig）：关闭→逐边 insert 回退
2. `simd`（运行时）：关闭→标量边界扫描
3. `binary_search_min`（运行时，替换硬编码 32）
4. `TGSR_NO_TS_CACHE`（编译期，EdgeRef 回 16 字节）→ CMake 选项出第二组 bench 二进制

### 需要新做的实验模式
5. E23 阈值扫描（复用 E19 窗口测量）
6. E21 加 `--rw_ratio` 扫描
7. E11 τ 扫描在当前引擎重跑
8. RocksDB 基线（时间编码键 `ts:src:dst`，与 TVA Full Mode 键设计同族）——工作量中等，排最后

## 进度（2026-09-17，自动更新）

paper_SIGMOD/main.tex：全文初稿 11 页编译通过。已完成：摘要、§1（Fig1 时间线图 +
CADETS 例）、§2 背景（模型 + demand paging）、§3 tmap 五条款 + 反例客户端、
§4 存储设计（Thm 1–4 + 移植 FAST 证明 + shift/发布协议两个算法 + run-claim +
EdgeRef Discussion + 保留/持久化 + Thm 恢复）、§5 查询引擎（每操作 I/O Analysis）、
§6 实现（一致性模型直说：无多边事务）、§7 评测五小节（全部对齐已记录 CSV）、
§8 相关工作四组、§9 结论。图 9 张全部由 scripts/make_sigmod_figures.py 从
results/ 生成：motivation、架构（TikZ, 色锁 Example 2）、ingest、baselines、
temporal、structural、recovery、ablation、pipeline；corpora 表 table_breadth.tex。

尚未做：GraphOne stream-view / LiveGraph MVCC 并发对打（现仅自测并发）；LDBC-SNB
（未定）；Timeline 换 Tmap 实例化重构（刻意推迟）；投稿格式（匿名化/PVLDB vs
SIGMOD 模板确认）；参考文献补 lsm/oneil1996 等新引 key 的核对。
