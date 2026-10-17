# Final revision checklist (user requirements, 2026-10-17)

Mode: advise-only. Claude proposes text; the user pastes into Overleaf.
Source of truth for the paper is the Overleaf copy, not `paper_SIGMOD/main.tex`.
The user is editing from a printed copy.

## Requirements, verbatim (Chinese) with working interpretation

1. **Captions too long.** "所有的fig，table，你的caption都太长了，你应该把要说的全部在文章里面说，caption要尽量缩短。"
   Everything explanatory moves into the body; captions become a title phrase.
   Style: "figure是什么加粗，说明文字不用加粗" -> the "Figure N:" label is
   bold, the caption text is not. acmart already renders label bold / text
   normal (checked in main.pdf); the fix is length, not fonts.
2. **Every term defined at first use.** "所有名词出现的时候必须要有解释。"
3. **Intro paragraph 2 needs citations.** "introduction的第二段里面，有的地方要有references的你真实一点都没有。"
4. **Figure 1 must connect to the rest of the paper.** "fig1得和后面的文章有关系啊。"
5. **CADETS must be explained.** "文章里CADETS是什么东西呢？"
6. **Motivation and selling point must be explicit.** "为什么我们要做这个东西，现在的东西问题在哪里，你也没有说啊，就是我们的卖点是什么。"
7. **Body must reflect the new title.** "现在文章叫 TGSR: A Time-Keyed Storage Engine for Temporal Event Analytics，我们得反映 temporal events analytics。"

## Measurements against the repo copy (main.tex @ faf5cb4)

### 1. Caption lengths (words, LaTeX stripped). TVA's captions are 4 to 9 words.

| words | label           | file             |
|------:|-----------------|------------------|
|  89   | fig:ingest      | main.tex         |
|  79   | fig:deploy      | main.tex         |
|  78   | fig:arch        | fig_arch.tex     |
|  76   | tab:breadth     | table_breadth.tex|
|  76   | fig:ablation    | main.tex         |
|  67   | fig:structural  | main.tex         |
|  62   | fig:temporal    | main.tex         |
|  60   | fig:baselines   | main.tex         |
|  59   | fig:motivation  | main.tex         |
|  51   | fig:runclaim    | fig_runclaim.tex |
|  51   | fig:recovery    | main.tex         |
|  42   | tab:complexity  | main.tex         |
|  42   | alg:publish     | main.tex         |
|  41   | tab:storage     | main.tex         |
|  38   | fig:overview    | fig_overview.tex |
|  38   | fig:format      | fig_format.tex   |
|  34   | alg:asof        | main.tex         |
|  27   | alg:shift       | main.tex         |
|  25   | fig:pipeline    | main.tex         |

19 captions, 1035 words total, mean 54. Target: one title phrase each
(roughly 5 to 12 words), panel letters allowed as "(a) X; (b) Y" without
numbers or claims. The displaced sentences go into the paragraph that
first cites the figure.

### 2. Terms used before they are defined (first occurrence, main.tex line)

| term | line | status |
|------|-----:|--------|
| CADETS | 85 (caption), 95 (Example 1), 333 (Example 2) | named "DARPA Transparent Computing CADETS corpus" but never explained as a whole-system provenance trace with a documented red-team intrusion; no citation |
| THEIA | 794 | no explanation |
| LANL | 790 | "LANL enterprise authentication" only; expand Los Alamos National Laboratory, cite lanl2015data |
| CSR | 118 | never expanded (compressed sparse row) |
| LSM | 113 | never expanded (log-structured merge tree) |
| TVA | 205 | used as a name before §7; one clause saying what it is |
| mmap / madvise | 215, 218 | OK in §2.2 context; confirm one-clause gloss |
| arena / timeline / tmap | 315, 323, 141 | tmap defined at 141; "arena" and "timeline" are used in §3 before their §4.1 definitions; add one-clause gloss at first use in §3.2 |
| run claiming | 144 ("run-claiming publication protocol") | named in Intro, defined only in §4.2; add a half-sentence gloss in the Intro |
| compaction | 154 | used in Intro contributions; gloss "the optional pass that sorts a window in place" |
| Meps / events/s | 519 | define "million events per second (Meps)" at first numeric use |
| GraphOne, LiveGraph, Stinger, Sortledton, RocksDB, DuckDB | 784, 813, 652 | each gets a category phrase at first mention (streaming graph store, key-value store, analytical column store) |
| CAS, fetch_add | 442, 443 | gloss "compare-and-swap" |
| RSS | 898 | gloss "resident set size" |
| SIMD / AVX2 | 624, 625 | gloss once |
| snapshot isolation, MVCC | 757 | gloss once |

### 3. Intro paragraph 2 ("Existing storage engines approach this shifted workload...")

Zero `\cite` in the paragraph. Each of the four claims needs anchors:
- dynamic graph stores keyed on source vertex, timestamp as payload: GraphOne, LiveGraph, Aspen, Sortledton, Teseo, Stinger
- columnar and LSM engines order by time but join per hop: RocksDB/LevelDB (O'Neil LSM), DuckDB/MonetDB, and a time-series store
- versioned temporal graph databases (version chains, delta encoding): TVA, AeonG, Chronos, ImmortalGraph, Kineograph
- time-sorted CSR with no ingest path: a static temporal-graph library (e.g. a CSR-based temporal analytics reference) plus the "rebuild during which answers are wrong" argument needs a cite or a concrete sentence
Intro paragraph 1 was already proposed with per-claim citations (lakhina2004anomalies to be added to references.bib).

### 4. Figure 1 is referenced nowhere

`\ref{fig:motivation}` appears 0 times in main.tex. Example 1 refers to the
figure only through the caption's "Example 1" back-pointer. Planned hooks:
Intro ¶1 or Example 1 (first cite), §5.3 Time-Respecting Traversal (the
chain in Fig. 1 is the traversal being defined), §7.2 Time-Respecting
Traversal Latency (the 133 µs result is that chain), §4.4 Retention
(the horizon sweeping from the left in Fig. 1).

### 5. CADETS

Needs, at first mention: the DARPA Transparent Computing program, Engagement 3,
the CADETS team's FreeBSD whole-system provenance trace (22.5M information-flow
events over 11 days) with a documented red-team intrusion, and a citation
(program report or the OmegaLog/UNICORN-style papers that use it; a
`darpa-tc` bib entry to add). THEIA gets the same treatment in one clause.

### 6. Motivation, problem, selling point

The Intro states the workload shift and the three challenges but never says
in one place: (a) what breaks today (a time-window query on a vertex-keyed
store is a scan plus filter; retention is a scatter delete; time-sorted CSR
cannot ingest), (b) what we do differently (the timestamp is the address;
no index is built, maintained, or recovered), (c) what the reader gets
(throughput numbers, retention as a pointer move, recovery as a replay).
Proposal: a "problem" paragraph between ¶2 and the challenges, and a
one-sentence selling-point statement that the abstract, Intro and
Conclusion all carry in the same words.

### 7. "Temporal event analytics" appears 0 times in the body

Title in Overleaf (agreed): "TGSR: A Time-Keyed Storage Engine for Temporal
Event Analytics". The body must use the phrase: Intro (the workload is
temporal event analytics; define it as window, recency, causal-order
queries over timestamped events), §2.1 model section, §7 opening sentence,
§9 Conclusion. "Temporal event graph" stays as the data model term;
"temporal event analytics" is the workload term.

## Attack order (proposed)

1. Intro rewrite as one block: ¶1 (already drafted), ¶2 with citations,
   new problem/selling-point paragraph, Example 1 (already drafted),
   Fig. 1 caption cut to one line, definitions of CADETS and the key terms.
2. Caption sweep, front to back: for each of the 19 captions, a short
   caption plus the paragraph that absorbs the displaced sentences.
3. Term-definition sweep by section (§2 to §7), using the table above.
4. Figure 1 cross-references in §4.4, §5.3, §7.2.
5. "Temporal event analytics" thread: Intro, §2.1, §7 opener, Conclusion.
6. Final scan on the Overleaf copy (user pastes the tex back, or exports
   the PDF) for em-dashes, "AI", undefined terms, caption lengths.

## Already proposed, awaiting the user to paste into Overleaf

- Abstract merged version (original voice plus four fixes).
- Title change (drop "High-Throughput").
- Intro ¶1 with per-claim citations and the lakhina2004anomalies entry.
- Example 1 as pure motivation (133 µs moves to §7.2).
- Shorter Figure 1 caption.

---

# Second batch (same day, 2026-10-17), verbatim with measurements

## A. Readability of the text

8. **Show what a query actually looks like.** "我们在文章当中说query的时候，我们是不是要考虑在实际状态下query长什么样？方便读者理解？"
   The paper names queries loosely: "window count" (3×), "windowed neighbor" (2×), "entity expansion" (2×), "window scan", "window query", "as-of", "time-respecting traversal/path", and Q1/Q2 only in Example 2. No query is ever written out.
   Fix: one numbered query set, stated once in §2.1 with a concrete form (input, predicate, output), and then used by that name everywhere, including §7 and every figure legend. Working set:
   - Q1 Window scan: all events with t in [t1, t2).
   - Q2 Windowed neighbors: out-events of vertex v with t in [t1, t2).
   - Q3 Time-respecting traversal: k hops from v inside [t1, t2), each hop later than the previous.
   - Q4 As-of neighbors: the latest event to each distinct neighbor of v at time t.
   Each gets the Figure 1 instance as its running example (the analyst's backward and forward questions are Q3 in two directions).

9. **T1 to T5 are indented.** "T1.T2.T3.T4.T5你为啥要缩进呢？"
   They sit in a `description` environment (main.tex line 257), which indents every clause. Fix: five run-in paragraphs with the clause name in italics, no list environment, no indentation.

10. **Three terms the user did not understand after reading the paper.** "run claiming 这个是啥？" "retention and durability 是什么？" "vertex index 我也没有明白。"
    Each is a mechanism name that the paper uses before it explains, and the explanation, when it comes, is written for someone who already knows the design. Each needs a one-sentence plain definition at first mention and a two-sentence "what problem, what mechanism" opener at its own subsection:
    - Run claiming (§4.2): consecutive events usually fall into the same time window, so instead of one atomic counter increment per event, a writer claims a whole run of consecutive slots with one increment and fills them without further synchronization.
    - Retention and durability (§4.4): retention is forgetting, i.e. deleting everything older than a horizon, done by releasing a prefix of the address space; durability is surviving a crash, done by a write-ahead log whose replay is ordinary re-ingestion.
    - Vertex index (§4.3): the per-vertex list of references into the arena that answers "what did v do", built beside the time-keyed store, not inside it, with the timestamp copied inline so the index can be scanned without touching the arena.

11. **§5 is confusing.** "我们query处理是少的对不对…第五章temporal query engine这个东西也很迷惑，query processing on the timeline我明白…as of snapshot retrieval其实我也不明白是啥。"
    The user understands 5.1 only. Our query processing is deliberately thin (the storage layout does the work), so §5 must say so in its first paragraph and then present each operator as "what is asked, where it reads, what it costs", tied to Q1 to Q4 above. 5.4 As-of retrieval must open with the question it answers in plain words (the graph as it stood at time t: for each neighbor, the most recent event up to t), explain why an event store needs an operator for it (an event stream has many events per pair; a snapshot wants one), and only then give Algorithm 3. Consider merging 5.2 and 5.4 under one heading "Query Processing on the Vertex Index" with Q2 and Q4 as run-ins, so §5 has three subsections mapping to Timeline, Vertex Index, Traversal.

## B. Section structure

12. **Why Implementation is its own section.** "implementation单独一章的必要性是什么？教授说implementation单列实现是因为我们的实现需要和hardware相关啊，这一点很重要。"
    Measured: §6 is 38 lines (main.tex 744 to 782) with three run-ins (Consistency Model, Fault Tolerance and Persistence, Configuration Switches). Hardware vocabulary in §6: SIMD 1, mmap 1, madvise 1, huge page 1, atomic 1; cache, page fault, TLB, memory ordering, NUMA, prefetch: 0.
    Fix: §6 becomes the hardware section. Candidate run-ins, each with the mechanism and the hardware fact it exploits:
    - Virtual Memory as the Allocator: anonymous mmap reservation, demand paging, page faults as the only allocation, madvise(MADV_DONTNEED) as the only free, THP negative result moved here from §7.5.
    - Cache-Line Layout: the 20 B packed edge, the 24 B EdgeRef, slots per 64 B line, why the inline timestamp saves a cache miss per reference.
    - Atomics and Memory Ordering: fetch_add for claiming, CAS for overflow blocks, release store of the timestamp as the publication point, what the x86 memory model gives for free.
    - SIMD Boundary Filter: AVX2 gather path, why it only helps on boundary slots (moved from §5.1).
    - Storage Device Interaction: WAL framing, fsync policy, why the device bounds durable ingest.
    Consistency Model and Configuration Switches move: consistency to §4.2 (it is a property of the publication protocol), switches to §7.1 Setup.

## C. The evaluation (the largest item)

13. **Comparisons are not aligned.** "在实验比较的部分…有不同的query对吧…我们比的真的歪七鸟八的…和别人比的时候是不是要对齐啊？这很重要啊。"
    Measured, system × dataset × query as the figures stand:
    | Figure | Systems | Datasets | Query / metric |
    |---|---|---|---|
    | Fig 6a ingest | TGSR, GraphOne, LiveGraph, RocksDB, TVA (DNF) | baselines on wiki-talk only; TGSR also CADETS, THEIA, LANL | events/s |
    | Fig 7a,b | TGSR vs GraphOne | cadets, wiki-talk, askubuntu, cit-Patents | windowed neighbor delivery, ns per delivered edge, scanned/delivered |
    | Fig 7c | TGSR vs RocksDB | same four | entity expansion in window |
    | Fig 8a | TGSR vs DuckDB, tcsr, ucsr | mathoverflow, wiki-talk | time-respecting traversal latency vs window width |
    | Fig 8b | TGSR vs tcsr, ucsr | CollegeMsg, email-Eu, askubuntu, ... | windowed BFS cost vs out-degree |
    | Fig 8c | TGSR only | lanl | sparse reachability |
    | Table 3 | TGSR, LiveGraph, GraphOne, RocksDB | askubuntu, wiki-talk, cit-Patents, CADETS (already the four, small to large) | resident MB |
    | Fig 9 | TGSR, DuckDB, LiveGraph | wiki-talk | pipeline time |
    | Fig 10 | TGSR vs topo CSR, weight CSR | eleven corpora | adjacency delivery for BFS/PR/LP/SSSP |
    | Fig 12b | TGSR, GraphOne, LiveGraph(sharded) | four | concurrent ingest + windowed neighbors |
    Every panel has a different competitor set, a different dataset set and a different query. Figures 7 and 8 are the worst: "一会儿和这个比，一会儿和那个比".

14. **Too many datasets in Table 2, too few shown.** "table 2这里面这么多数据集，我们展示的实验结果却很少，我们是不是应该只用几个数据集，从大到小，然后和别人比较的时候对齐。"
    Measured: Table 2 has 15 corpora. The datasets that EVERY baseline run already covers (GraphOne E20, LiveGraph E19, RocksDB E24, DuckDB E22, Stinger E31, Sortledton E32, concurrent E26) are exactly four:
    | Corpus | Events | V | role |
    |---|---|---|---|
    | TC-CADETS | 22.5M | 2.9M | provenance, the Figure 1 corpus |
    | cit-Patents | 16.5M | 6.0M | structural-era reference |
    | wiki-talk | 7.8M | 1.1M | interaction stream, the TVA corpus |
    | askubuntu | 0.96M | 159K | small interaction stream |
    Proposal: the main comparison uses these four, large to small, in every panel; LANL 1.05B and LDBC-SNB SF10 appear once each as scale points (Stinger and Sortledton already have LDBC rows); the other nine corpora leave Table 2 and survive only as the breadth row set behind Fig 10 or move to a one-line "also run on" sentence. Table 2 then has 6 rows.

15. **"Ingest" is undefined and we compare ourselves with ourselves.** "ingest到底是什么意思呢？就是我们现在就是自己和自己比。"
    Fix: define ingestion at first use (appending the arriving events to the store so that queries can see them; measured in events per second). Fig 6a redraws as a grouped bar chart: four datasets × seven systems (TGSR, GraphOne, LiveGraph, Stinger, Sortledton, RocksDB, DuckDB load), one scale, all on eight threads. The TGSR-only corpora leave this panel.

16. **Figures 7 and 8: not aligned, and the log plots are ugly.** "figure7和figure8，不仅没有对齐，而且对数表很丑。"
    Fix: one figure for queries, one grid: rows Q1 to Q4, columns the four datasets, every system that can answer the query as a bar in every cell, "cannot express" marked, linear scale where the range allows and a broken axis or a single shared log scale otherwise, never a mix. Supporting diagnostics (scanned/delivered, cost vs out-degree, cost vs window width) move to the ablation section or are dropped.

17. **The workload section is sloppy; the message must be unmistakable.** "负载也写的很潦草，就是至少要让人知道我们就是吞吐快，具体算是算法的问题。"
    Fix: §7 opens with the claims in order: (1) ingestion throughput, where we win against every system on every dataset; (2) temporal queries Q1 to Q4, where we win on cost per delivered edge; (3) structural algorithms, where we lose to a static CSR by a stated factor and say that this is the algorithm's data access pattern, not the storage engine's job. The workload definition (datasets, query set, threads, verification) is one tight subsection that the figures then obey.

## Updated attack order

1. Fix the query set (item 8) and the dataset set (item 14) first, because every later edit depends on them.
2. Redraw Fig 6, Fig 7, Fig 8 and Table 3 on the aligned grid (items 13, 15, 16), regenerate from existing CSVs; no new runs needed for the four corpora. Fonts: no `\tiny` in any TikZ figure (Fig 2: 8 tiny labels, Fig 3: 21, Fig 4: 3, Fig 5: 8); minimum `\scriptsize` for cell text and `\footnotesize` for names; Fig 3 becomes a `figure*` if needed.
3. Intro block (items 1 to 7 of the first batch), using the fixed query names and Figure 1.
4. §4 openers for run claiming, vertex index, retention and durability (item 10); T1 to T5 as run-ins (item 9).
5. §5 restructure (item 11), §6 hardware rewrite (item 12).
6. Caption sweep and term sweep (items 1, 2), then the "temporal event analytics" thread (item 7).


---

# Third batch (same day), verbatim with measurements

18. **Do T1 to T5 have an order or a logic?** "T1,2,3,4,5到底有没有顺序关系逻辑关系。"
    Measured: the five clauses are stated as a flat list. The logic exists but is never written: T1 is the axiom (a timestamp is an address); T2 and T3 are what that axiom buys from the virtual memory system (space follows occupancy; deletion is a prefix release); T4 and T5 are the write discipline that keeps the axiom true under a stream (append-only content, so replay is reconstruction; per-slot publication, so readers never see a torn entry). They also map one-to-one onto the Intro's challenges (T1 to Challenge 1, T5 to Challenge 2, T2 to T4 to Challenge 3) and onto the four theorems (T1 Thm addr, T2 Thm space, T4 Thm recover, T5 Thm conc). Fix: one sentence before the clauses stating this structure, and the clauses typeset as run-ins in that grouping (item 9).

19. **Figures 10, 11, 12 are also unaligned; align them, run more experiments if needed.** "fig10，11，12都有这个问题，就是比较没有对齐，一定要对齐，我们宁可继续做实验。"
    Measured:
    | Figure | Systems | Datasets |
    |---|---|---|
    | Fig 10 structural | TGSR vs topology CSR, weight CSR | eleven corpora (CollegeMsg to LANL) |
    | Fig 11 recovery | TGSR only | wiki-talk, stackoverflow, cit-Patents, LANL |
    | Fig 12a modules | TGSR only | cit-Patents, wiki-talk, cadets |
    | Fig 12b concurrent | TGSR, GraphOne, LiveGraph sharded | askubuntu, wiki-talk, cit-Patents, cadets |
    | Fig 12c shards | TGSR only | cit-Patents |
    Fix: every panel runs on the same aligned dataset set (item 14, revised in item 22) in the same order; TGSR-only panels are allowed only in the design-analysis section and must say so in the heading; Fig 10 on the aligned set plus LANL, with the structural baseline that competitors can also run (GraphOne and LiveGraph expose BFS; run them on the same four so the structural loss is against real systems, not only against our own CSR). New runs needed: GraphOne/LiveGraph BFS on the aligned set; every baseline on whichever large corpus item 22 adds.

20. **Colors are used arbitrarily; one scheme for every figure.** "全文就是用颜色也是随便乱用…全文配色必须一致，所有的figure都要检查配色方案全体一致。"
    Measured, the same hex color means different things in different figures:
    | color | meaning in Python figures | meaning in TikZ figures |
    |---|---|---|
    | #0072B2 blue | sorted CSR (Fig 8, 10); recovery curve (Fig 11); a module (Fig 12); a switch (Fig 13); Fig 1 timeline | qwin = window query (Fig 2, 3) |
    | #E69F00 orange | arrival CSR AND LiveGraph (figstyle.py maps both) | not used |
    | #D55E00 vermillion | DuckDB AND RocksDB (both) ; Fig 1 attack chain | wins = insert path (Fig 2, 3, 4, 5) |
    | #009E73 green | topology CSR | qnbr = neighbor query (Fig 3, 5) |
    | black | TGSR everywhere | borders |
    TikZ figures additionally use raw green!11, blue!12, yellow!22, orange!6 fills that are not in the palette.
    Fix: one legend for the paper, defined once in figstyle.py and once in the preamble with identical hex values: TGSR black; each competitor a fixed color it keeps in every figure (GraphOne, LiveGraph, RocksDB, DuckDB, Stinger, Sortledton, CSR); the three paths (insert, window query, neighbor query) three other fixed colors used only in the architecture drawings; grey for references and annotations. Then every figure is regenerated and checked against the legend.

21. **Full consistency audit of referents, numbers, terms, symbols.** "全文指代，实验数据，名词，符号一致性你检查过没有？"
    Not done before today. First pass found:
    - Numbers: wiki-talk ingestion is 66.9 M events/s in Table 2 but 73.2 in §4.2 and §7.5 (different runs, E16 vs E23); CADETS is 80.1 in Table 2 but 85.4 in §7.5. One number per corpus, from one run, everywhere.
    - Units: "M events/s" 16 times, "events/s" 20, "events per second" 2, and Table 2's header says "Meps", which the text never defines. Pick one.
    - Name: `\sys` 53 times, bare "TGSR" 3 times in main.tex, plus "TGSR" in figure legends; use the macro everywhere, and fix the legends if the name changes.
    - Capitalization: Timeline 4 / timeline 7, Arena 3 / arena 11, Vertex Index 2 / vertex index 6; run claiming / run-claiming / Run Claiming all appear.
    - Vocabulary: "bucket" 11 times and "window" 81 times for what is often the same object (s well (\cref{alg:publish}): an append clears the bucket's \emph{sorted} flag \emph{before; t's \emph{sorted} flag \emph{before} it bumps the bucket counter.  \begin{algorithm}[t] \s; \textsc{Expand}$(v, t_{lo}, t_{hi})$}{   \ForEach{bucket $b$ of $v$}{     $n \leftarrow b.; s $v$'s reference chain in the vertex index. Each bucket carries the sorted flag of \cref{); "slot" 40 times. Define window (a time range), slot (its address), bucket (the block the slot points to), and use each only in its sense.
    - Figures referenced nowhere in the text: fig:motivation, fig:baselines, fig:temporal, fig:pipeline, fig:structural, fig:recovery, fig:ablation, fig:arch.
    - Symbols: $S$ shift, $P$ period, $T_0$ origin, $W$ windows in range, $W_occ$ occupied windows, $E$, $V$, $d(v)$, $k$, $R$ result; check each is introduced once at the mapping equation or Table 1 and never reused with a second meaning ($R$ is used both for result set and in §7 for readers; $k$ for hops and for results per expansion).
    - Repeated factors: $5.5\times$ appears three times, $1.52\times$, $2.1\times$, $3.8\times$, $5.8\times$ twice each; confirm each repetition is the same claim.
    Deliverable: a consistency table in this file after the figures are redrawn, then a final pass on the Overleaf PDF.

22. **Fill the matrix even with fewer datasets, and show scale.** "你是不是要补全，宁可数据集少一点，就是展现大量处理数据的能力。"
    Revised dataset proposal: three or four corpora, ordered large to small, with every competitor on every one, and the largest big enough to show volume: LANL (1.05B events) or THEIA (97.3M) at the top, then CADETS (22.5M), wiki-talk (7.8M), optionally askubuntu (0.96M). Competitors that cannot finish the largest corpus are reported as did-not-finish with the time budget, which is itself the throughput result. New runs: GraphOne, LiveGraph, RocksDB, DuckDB, Stinger, Sortledton on THEIA and on LANL (or on whichever of the two is chosen), 90-minute budget each, eight threads, plus the Q1 to Q4 query suite on each. cit-Patents stays only as the structural reference corpus in Fig 10 if GraphOne/LiveGraph BFS runs there.

23. **Figure 1 and Example 1 must state the intent explicitly.** "fig1和example1也要把你的意图阐述的很明确。"
    Intent, in one sentence each, to appear in the Example and in the caption's first line: the attack is a chain of events ordered in time; both of the analyst's questions are traversals along the time axis (Q3 backward and forward); retention is deleting the oldest end of the same axis; therefore the store should address events by time. The already-drafted Example 1 rewrite is revised to end with that sentence.

24. **Figures 3, 4, 5 text too small** (first batch said 2, 3, 4; both numberings recorded). Measured `\tiny` counts: Fig 2 overview 8, Fig 3 arch 21, Fig 4 format 3, Fig 5 run claiming 8. Rule: no `\tiny` anywhere; names `\footnotesize`, cell contents `\scriptsize` at minimum; widen to `figure*` where needed.

25. **The strength is throughput; say it plainly, and no ML vocabulary.** "我们这个东西吞吐+query处理，其实长处就是吞吐，我们一定要把长处说清楚，不要说ablation study用数据库术语。"
    Measured: "ablation" appears 9 times, including the §7.5 title "Ablation Study on TGSR" and Fig 13's caption. Fix: §7.5 becomes "Performance Analysis of TGSR" or "Effect of Design Choices" (TVA uses "Performance Analysis on TVA"), "ablation" is removed everywhere, "switch on/off" becomes "with and without". The throughput claim is stated in the abstract, the first paragraph of §7, and the conclusion in the same words.

26. **Captions change; do not be verbose.** "caption肯定要改，还有就是不要啰嗦。" Item 1 covers captions. Verbosity rule for the whole text: one claim per sentence, no restatement of a figure in prose, no sentence that only comments on another sentence.

## Attack order, revised

1. Decide the dataset set (item 22) and the query set (item 8); fix the color legend (item 20). These three decisions gate everything else.
2. Run the missing experiments: all competitors on the new largest corpus; GraphOne and LiveGraph BFS on the aligned set; Q1 to Q4 on every (system, corpus) cell. Record in results/ with README per the hygiene rule.
3. Redraw every evaluation figure on the aligned grid with the shared legend (items 13, 15, 16, 19); fix TikZ font sizes (item 24); regenerate drawio sources.
4. Text: Intro block (items 1 to 7, 23), T1 to T5 logic and run-ins (9, 18), §4 openers (10), §5 restructure (11), §6 hardware (12), §7 opener and renaming (17, 25).
5. Caption sweep (1, 26), term sweep (2), consistency audit (21), "temporal event analytics" thread (7).

---

# Execution log (started 2026-10-17 after the user's "开始吧，修改")

Mode for this pass: executed in the repo (experiments, figures, main.tex), with a
replacement main.tex and figure PDFs delivered for Overleaf at the end. The
title and abstract the user edited in Overleaf must be re-merged by hand.

Decisions taken (items 8, 14, 20, 22):
- Corpora, large to small: LANL 1.05B, THEIA 97.3M, CADETS 22.5M, wiki-talk 7.8M.
  Every competitor on every one; DNF recorded with the sustained rate.
- Query set Q1..Q4 defined in docs/E33_SPEC.md and Table 1 of the paper.
- Color legend in figures/figstyle.py and the preamble (systems vs paths).
- "Ablation Study" renamed "Performance Analysis of TGSR"; the word is gone.

Done so far:
- Figures 1 to 5 re-laid out, no \tiny anywhere, one color scheme, Figure 1
  states its intent on the drawing.
- Sections 1 to 6 rewritten: cited gap paragraph, explicit problem/selling
  point, CADETS defined, Q1..Q4 table, T1..T5 as grouped run-ins with their
  logic stated, plain openers for run claiming / vertex index / retention and
  durability, as-of merged into the vertex-index subsection, Implementation
  as the hardware section, every caption cut to a title phrase.
- Section 7 rewritten on the aligned grid with \textbf{XX} placeholders for
  numbers that the queue below will produce; headline numbers are macros in
  the preamble (\nIngestGapLo etc.) so they cannot disagree across sections.
- New harness benchmark/bench_e33_matrix.cpp (TGSR, LiveGraph, RocksDB,
  DuckDB) and GraphOne jobs 28/34/35/36 implement the spec; e22 got --systems.
- scripts/run_revision.sh is the sequential experiment queue.

Still to do once the queue finishes: fill the XX placeholders from the CSVs,
regenerate the seven evaluation figures, re-check overfull boxes and page
count, run the consistency scan, flatten to overleaf-upload/main.tex, commit.

## Open measurement note (2026-10-17, during the queue)

TGSA ingestion in the aligned matrix (64K-event batches, the same arrival
protocol every system gets) is lower than the earlier whole-corpus single
call of E16/E23: wiki-talk 54.2 vs 73.2, CADETS 60.3 vs 85.4, THEIA 30.5 vs
94.6 M events/s. Batching explains ~0.7x; THEIA's 0.32x may also be page-cache
pressure while the queue was running. After the queue: re-measure TGSA on
the four corpora, quiet machine, both protocols, three trials; report the
aligned (batched) number in Fig 5 and quote the single-call ceiling in the
performance-analysis text. Never mix the two in one figure.

## Progress log, later on 2026-10-17
- System renamed TGSA; title "TGSA: A Time-Keyed Storage Engine for Temporal Event Analytics".
- Palette moved to a blue base (TGSA blue; writes vermillion, reads green; blue tints in drawings).
- Figures merged 12 -> 9: formats into the architecture figure; concurrent panel into the
  ingestion figure; pipeline + BFS into one "analytics" figure; analysis figure 6 -> 4 panels.
- Introduction 1300 -> 800 words; §5 rewritten around the design priority (throughput first,
  the four operators downstream asks for, the rest left to the layers that own it); §4 opener.
- Aside sweep: no lines-of-code, no process anecdotes, no self-assurance refrains.
- Queue restarted once (my pkill matched my own shell); resumed at DuckDB/CADETS at 08:50.
- Pending after the queue: fill XX from scripts/fill_numbers.py, TGSA ingest re-measurement
  (scripts/run_tgsa_ingest_trials.sh), regenerate figures, consistency scan, flatten, zip, commit.

## Defect found by the aligned matrix (2026-10-17 09:45)
On THEIA (6.9M vertices) the engine's vertex-index bucket arena, reserved at
n x 24 B x 4 by the harness (and by bench_e16_breadth), ran out
("[VertexIndex] bucket arena exhausted ... further refs will be refused"), so
TGSA silently dropped 4.5% of Q2 results; LiveGraph and RocksDB agreed with
the independent ground truth (tools/e33_ref) and TGSA did not. Cause: each of
the 6.9M vertices pays a first bucket of fixed minimum capacity, which the
n-proportional reserve does not cover. Fix: MAP_NORESERVE on the engine's
anonymous reservations, a reserve sized from n and V, and a refused-ref
counter the harness checks after ingest (abort, never a silent result).
The THEIA .ref was replaced by the independent ground truth; TGSA's THEIA
rows must be rerun after the queue (scripts/run_e33.py --corpora theia
--systems tgsr --force) and its ingest rate of 30.5 M events/s discarded.
Every earlier THEIA number from E16 is suspect for the same reason.

## Final assembly (2026-10-17 evening)
- Matrix complete: 7 systems x 4 corpora, every verifiable cell verified.
- TGSA ingestion trials (quiet machine, 3 trials): batched 54.0/59.0/33.4/72.4 (wiki/CADETS/THEIA/LANL),
  one-batch 59.9/85.8/78.8/77.0 M events/s. THEIA's batched rate is a protocol effect.
- Honest losses recorded in the text: THEIA Q2 vs GraphOne 1.5x, Q4 vs GraphOne on THEIA 2.8x and
  LANL 4.7x, THEIA ingest tie; claims in abstract/§7/conclusion rewritten to "59 of 64 cells".
- Hardware sentence corrected from the machine itself (10-core Xeon W-2255, SATA HDD, not NVMe).
- Durability rows for CADETS/THEIA/LANL rerun (first run had no log directory and measured nothing).
- Figures 12 -> 8 (run-claiming figure cut for length; durability figure single panel).
- Body ends on page 12; references from page 13.
