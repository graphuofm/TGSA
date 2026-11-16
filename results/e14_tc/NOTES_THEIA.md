# E14 on DARPA TC E3 THEIA: chain recall under reduction

_2026-10-20. 110.6M CDM records -> 97,289,833 information-flow edges over
6,874,886 nodes (262K processes, 1.01M files, 185K netflows), 10.29 days, 39
out-of-order. Ground truth: the four THEIA attacks of the TC E3 report,
data/tc_theia_groundtruth.tsv; resolved seeds data/tc-theia-e3.seeds.tsv (=
/home/jding/dataset/bin/tc-theia-e3.seeds.tsv). Raw rows: e14_theia.csv._

| attack | report section | outcome per report |
|---|---|---|
| 1 | 3.3 20180410 Firefox backdoor, drakon in memory | succeeded on the 4th try (allstate exploit crashed Firefox 3x; gatech exploit worked) |
| 2 | 3.11 20180412 browser extension, drakon dropper | partial: all sshd injections failed; micro apt ran and portscanned |
| 3 | 4.6 20180410 phishing e-mail with link | succeeded (credentials submitted) |
| 4 | 4.8 20180413 phishing e-mail, executable attachment | failed: tcexec "failed missing library" |

## Ground truth and resolution

scripts/resolve_tc_groundtruth.py reproduces tc-cadets-e3.seeds.tsv byte for
byte. The rule: a node matches a row when its label contains (`substr`) or
equals (`exact`) the match string; the kind column is not a filter; matches
are taken in ascending node id, capped at 6 per row; unmatched rows emit
nothing.

Three THEIA facts shaped the ground truth:
- No FileObject carries a path (0 of 1,011,567), so no file can be a seed.
  Executed implants appear as process nodes whose cmdLine is their path.
- One process is many Subject nodes (one per thread): 19 for clean, 39 for
  profile, 63 for the phishing Firefox. The cap of 6 therefore decides which
  Subjects count, so every result below is also reported uncapped.
- The exploit and shellcode connections are charged to Subjects labelled
  `fluxbox` (1,383 of them), not to a Firefox Subject. The browser itself
  cannot be seeded by label.

| attack | rows resolved | node ids (cap 6) | node ids (uncapped) |
|---|---|---|---|
| 1 | 7/7 | 23 | 69 |
| 2 | 7/7 | 24 | 62 |
| 3 | 3/3 | 13 | 70 |
| 4 | none: no entity of the attack is in the trace | 0 | 0 |

## Chain recall

Recall is over the attack's resolved ids. The union is forward taint from the
earliest attack NetFlow plus backward taint from every attack NetFlow
(bench_e14_tc, unchanged semantics).

| attack | full | cpr 1s | cpr 1min | cpr 10min | cpr 1h | cpr 24h | random 50 | random 90 (trials 0/1/2) |
|---|---|---|---|---|---|---|---|---|
| 1 | 21/23 = 91.3% | 91.3% | 91.3% | 91.3% | 91.3% | 91.3% | 82.6% | 65.2 / 69.6 / 56.5% |
| 2 | 24/24 = 100% | 100% | 100% | 100% | 100% | 100% | 70.8% | 70.8 / 66.7 / 70.8% |
| 3 | 11/13 = 84.6% | 84.6% | 84.6% | **61.5%** | **61.5%** | **61.5%** | 76.9% | 53.8 / 53.8 / 69.2% |
| reduction | 1.00x | 6.22x | 7.68x | 8.65x | 8.84x | 8.92x | 2.00x | 10.00x |

Uncapped control (full graph): 66/69 = 95.7%, 62/62 = 100%, 15/70 = 21.4%.
Attack 4 cannot be run: the harness needs a NetFlow seed ("no seeds for
attack 4", exit 1).

## Union size and latency

| attack | | full | cpr 1h | random 50 | random 90 (trials 0/1/2) |
|---|---|---|---|---|---|
| 1 | union, % of V | 22.15% | 21.08% | 6.19% | 1.09 / 1.10 / 0.0007% |
| | latency | 13.8 s | 6.4 s | 3.5 s | 98 ms / 97 ms / 43 us |
| 2 | union, % of V | 58.53% | 57.54% | 32.89% | 1.10 / 1.11 / 0.0094% |
| | latency | 17.9 s | 8.0 s | 3.6 s | 67 ms / 66 ms / 186 us |
| 3 | union, % of V | 22.06% | 21.31% | 9.88% | 0.0001 / 0.0002 / 1.04% |
| | latency | 7.6 s | 4.2 s | 1.8 s | 13 us / 18 us / 79 ms |

Latency is the full query: forward taint plus every backward trace. Run cost:
4.7-34.9 s wall per run (load, reduce, build, query), full-graph peak RSS
12.7 GB. The 33 runs took 8m44s in total; none came near the 30-minute cap.

## Chain entities not recovered, and why

**Attack 1, every policy: 2827227 /home/admin/clean, 2857897
/home/admin/profile (uncapped: also 2828808 clean).** These are the implants'
reader threads. They take 39 and 174 in-edges from the C2 flow 161.116.88.72
but never write to it, so a backward trace from the C2 cannot reach them.
Forward taint would, but the harness starts it at the earliest attack NetFlow.
On THEIA that is the crashed allstate attempt (61.130.69.232 -> fluxbox,
18:25:32 UTC), whose forward closure is 2 entities. Verified with
barry_provenance_query: forward taint from the successful gatech shellcode
flow (141.43.176.203, 18:31:22 UTC) reaches all four implant Subjects. The miss
comes from the choice of seed, not from reduction.

**Attack 3, every policy: 2805685, 2805829 (uncapped: 55 of the 63
phishing-Firefox Subjects).** All share the cmdLine
`.../firefox http://www.nasa.ng/` and were spawned by Thunderbird 2805483 at
17:42:53.9 UTC. Of the 55, 22 have no edge after the first phishing contact
(17:42:54.68). The other 33 have no time-respecting flow path to or from the
nasa.ng or foo1.com flows. The Subject that did make those connections
(2805847) is the 7th id, so the cap of 6 drops it; the uncapped run hits it.
The ground truth over-counts here: label matching cannot tell the connecting
thread from its siblings.

**Attack 3, CPR at 10 min, 1 h and 24 h: also 2805752, 2805753, 2805826.**
These are reached on the full graph and under 1 s and 1 min windows. They
write into the address-less NetFlow hub 147 (`NA:0`). CPR keeps the first
occurrence of a (src,dst) pair per window; once the window passes 10 minutes,
the occurrence that carried the flow after these Subjects acted is dropped.
This is the mechanism behind the CADETS attack-4 loss. The path was not traced
edge by edge.

**Random drop.** The lost entities are low-degree Subjects (1-13 edges) whose
few edges fell to the coin. Attack 2 loses pass_mgr, five gtcache Subjects
and 2 of 3 /var/log/mail Subjects in every random run, already at 50%.

**Absent from the trace (no label contains the string; reasons in the TSV
header):**
- 145.199.103.57, the allstate webserver. The exploit crashed.
- 104.228.117.212, the gatech webserver. The page fetch was not recorded.
- 5.214.163.155, libdrakon. Shipped over the implant channel; marked failed.
- /var/log/xdev, /var/log/wdev, /tmp/memtrace.so. Files carry no path, and
  all injections failed.
- Scan targets 128.55.12.166, .67, .141, .1, .55. No NetFlow was recorded.
- 62.83.155.175. The phishing sender talked to the CADETS postfix, not to
  THEIA.
- tcexec. Attack 4 failed and left no trace.

## Comparison with CADETS (NOTES.md)

| | CADETS 1-4 | THEIA 1-3 |
|---|---|---|
| graph | 22.5M edges / 2.86M nodes | 97.3M edges / 6.87M nodes |
| CPR ceiling (24 h window) | 5.60x | 8.92x |
| chain recall, full | 68 / 92 / 100 / 89% | 91 / 100 / 85% |
| evidence lost at CPR 1 h | none (1-3); 2/9 (4) | none (1, 2); 3/13 (3) |
| random 50% | 53 / 92 / 100 / 89% | 83 / 71 / 77% |
| random 90% | 47 / 92 / 80 / 56% (trial 0) | 57-70 / 67-71 / 54-69% |
| union, full | 0.29-1.6% of V | 22-59% of V |
| latency, full | 75-151 ms | 7.6-17.9 s |

Two CADETS findings carry over to THEIA and one does not.

CPR again erases evidence inside its own operating range. THEIA's ceiling is
higher (8.9x against 5.6x). At that ceiling the two drakon chains lose
nothing, while the phishing chain loses 3 of 13 entities once the window
reaches 10 minutes; CADETS attack 4 lost 2 of 9 at 1 h. Fine chains also break
under random loss. A 90% drop leaves 54-71% of each THEIA chain, and THEIA
already breaks at 50% (71-83%), where three of the four CADETS chains held.

Taint scope does not carry over. CADETS kept the union to 0.3-1.6% of the
graph. THEIA's union is 21-59%, which is the LANL regime (15-29%), even though
both TC logs are syscall-level. The cause is the Subject each event is charged
to, not how fine the records are:
- THEIA charges the exploit and shellcode connections to fluxbox Subjects.
  The busiest has 9.3M edge endpoints.
- Much socket traffic passes through 28,240 address-less `NA:0` NetFlow
  nodes. The busiest has 14.5M endpoints, 7.5% of all.

One backward step from a C2 flow into either hub pulls in a fifth of the host.
For the cross-corpus claim, "granularity determines actionability" needs
qualifying: attribution granularity is what matters. A syscall-level log that
charges its network events to a hub Subject behaves like a host-level log.

The full-graph forward closure of the gatech shellcode flow lists both THEIA
intrusions in causal order: loader 146.153.68.151 -> clean -> C2
161.116.88.72 -> profile -> netrecon 7.149.198.40 (Apr 11) -> pass_mgr ->
gtcache -> /var/log/mail -> micro C2 149.52.198.23 -> scan of 128.55.12.110
(Apr 12). The closure is 41% of the graph, though, so containing the chain is
not by itself evidence of a causal link.

## Reproduce

```
python3 scripts/resolve_tc_groundtruth.py --gt data/tc_theia_groundtruth.tsv \
    --nodes <prefix>.nodes.tsv --out <prefix>.seeds.tsv     # --max-per-row 1000000 = uncapped
./build/bench_e14_tc --graph <prefix> --seeds <prefix>.seeds.tsv --attack N \
    --policy <full|cpr|random> --param <ns_window|drop_pct> --threads 16 \
    --trial i --output e14_theia.csv [--report-ids hits.tsv]
```
CPR windows 1e9, 6e10, 6e11, 3.6e12, 8.64e13 ns. Random: 50 (trial 0) and 90
(trials 0-2). `--report-ids` is an optional diagnostic: it appends one hit/miss
line per ground-truth id and leaves the CSV unchanged.
