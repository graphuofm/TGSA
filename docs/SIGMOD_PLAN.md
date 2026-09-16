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
