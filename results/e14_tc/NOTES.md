# E14: Cross-corpus validation on DARPA TC E3 CADETS

_2026-09-10. Whole-system provenance: 44.4M CDM records -> 22,536,062
information-flow edges over 2,855,135 nodes (225K processes, 2.3M files, 155K
netflows), 10.98 days, 179 out-of-order. Ground truth: 4 nation-state attacks
from the official TC report (sections 3.1/3.8/3.13/3.14), 24 entity seeds
resolved to 45 node ids; expected-absent entities documented in
data/tc_cadets_groundtruth.tsv._

## Case study -- the attack chain, recovered end to end

Forward taint from the attack-1 exploit source (81.49.200.166) on the full
graph returns 265 entities (0.0093% of the graph) and contains the entire
documented chain in causal order: nginx worker -> shellcode server ->
second-round exploit -> /tmp/vUgefal (the implant) -> elevated process ->
drakon fetch -> netrecon C2 -> /var/log/devc. Every report entity reachable by
information flow appears; total latency 133 us.

## The investigation primitive is bidirectional

Entities the implant READ (/etc/passwd, /etc/group) are upstream of the
process, so forward taint alone recovers 47% of chain entities -- correctly.
Forward-from-intrusion UNION backward-from-each-C2 recovers 68-100% per attack,
with union sizes 0.29-1.6% of the graph. Chain misses that remain are nginx
binary/config file nodes never touched in the window and failed connections
that produced no NetFlow (expected-absent).

## The cross-corpus finding

| | LANL (host-level) | CADETS 1-3 (syscall-level) | CADETS 4 (infra reuse) |
|---|---|---|---|
| CPR reduction achievable | 131x | 5.6x (ceiling) | 5.6x |
| evidence lost at that ratio | 44% of attack edges | **0%** | **2/9 chain entities (22%)** |
| random 90% drop | recall 58%, edges 8.6% survive | chains break (47-80%) | 55.6% |
| taint scope (usable?) | 15-29% of network (no) | 0.3-1.6% (yes) | 1.6% (yes) |

Three consequences, each a paper claim:

1. **Granularity determines actionability.** Fine-grained flow graphs give
   surgical impact sets (0.3-1.6%); coarse host graphs explode (15-29%).
   Aggressive reduction moves a log from the first regime toward the second.
2. **The reduction ceiling is a danger signal.** CPR finds 131x of "redundancy"
   in a coarse log and destroys 44% of evidence there; on a fine-grained log it
   tops out at 5.6x and (first-contact attacks) destroys nothing. Apparent
   reducibility IS causal detail miscounted as redundancy.
3. **CPR selectively erases infrastructure reuse.** Attack 4 -- the attacker
   returning over C2 channels used in attacks 1-3 -- loses a third of its chain
   at CPR's own ceiling, because repeat contact is exactly what CPR
   deduplicates. Reuse is how analysts link intrusions into campaigns; CPR
   deletes that link preferentially.
4. **Fine granularity trades robustness for precision.** Random 90% loss leaves
   coarse LANL still over-connected (58% recall) but shatters fine chains
   (47-80%): each hop rides on few edges. Lossless retention matters MORE, not
   less, as collection gets better.

## Reproduce

```
./build/bench_e14_tc --graph <prefix> --seeds <prefix>.seeds.tsv --attack N \
    --policy <full|cpr|random> --param <ns_window|drop_pct> --threads 16 \
    --trial 0 --output e14.csv
```
Converter: tools/tc_cdm_to_bin.cpp; ground truth: data/tc_cadets_groundtruth.tsv.

## Full sweep (2026-09-10): CPR windows 1s-24h, random x3 seeds

```
attack policy  param       reduction  chain_recall union_frac
1      cpr     1e+09       5.01       0.68         0.0026
1      cpr     6e+10       5.29       0.68         0.0025
1      cpr     6e+11       5.53       0.68         0.0023
1      cpr     3.6e+12     5.59       0.68         0.0023
1      cpr     8.64e+13    5.60       0.68         0.0022
1      random  90          10.00      0.47         0.0000
1      random  90          10.01      0.47         0.0000
1      random  90          10.01      0.47         0.0000
2      cpr     1e+09       5.01       0.92         0.0145
2      cpr     6e+10       5.29       0.92         0.0130
2      cpr     6e+11       5.53       0.92         0.0127
2      cpr     3.6e+12     5.59       0.92         0.0125
2      cpr     8.64e+13    5.60       0.92         0.0123
2      random  90          10.00      0.92         0.0000
2      random  90          10.01      0.92         0.0000
2      random  90          10.01      0.92         0.0000
3      cpr     1e+09       5.01       1.00         0.0144
3      cpr     6e+10       5.29       1.00         0.0129
3      cpr     6e+11       5.53       1.00         0.0126
3      cpr     3.6e+12     5.59       1.00         0.0124
3      cpr     8.64e+13    5.60       1.00         0.0122
3      random  90          10.00      0.80         0.0000
3      random  90          10.01      0.80         0.0000
3      random  90          10.01      0.80         0.0000
4      cpr     1e+09       5.01       0.89         0.0144
4      cpr     6e+10       5.29       0.89         0.0129
4      cpr     6e+11       5.53       0.89         0.0126
4      cpr     3.6e+12     5.59       0.67         0.0124
4      cpr     8.64e+13    5.60       0.67         0.0122
4      random  90          10.00      0.56         0.0000
4      random  90          10.01      0.78         0.0000
4      random  90          10.01      0.56         0.0000
```

## Reduction families beyond CPR (2026-10-20)

Policies now live in `benchmark/reduction_policies.hpp`, shared with E8; `full`,
`cpr` and `random` reproduce the earlier rows exactly (re-verified: attack 1
full/cpr 1 h/random 50/random 90 and attack 4 cpr 1 h give identical
edges_kept, recall and union). Each new policy is a pure function of the
time-ordered (src, dst, t, class) stream:

- `loggc P` (LogGC, CCS'13; the rule as briefed): an entity is temporary iff it
  has an in-edge, its lifetime is < P, and no edge leaves it for a non-temporary
  entity at or after its last in-edge (solved to a fixed point = the dead-end
  subgraph). All edges incident to a temporary entity are removed.
- `loggc_reach P` (LogGC's garbage definition proper): entity dead iff lifetime
  < P. Edge u->v at t is removed iff v is dead and no time-respecting path from
  v at >= t reaches a live entity. A dead entity's out-flows into live state are
  kept. One exact reverse-time sweep.
- `nodemerge P` (NodeMerge, CCS'18): a source with zero in-degree over the
  whole stream is read-only; its edges to a reader within the first P of the
  reader's lifetime collapse to the first event per (src, reader).
- `dedup`: first event per (src, dst) over the whole stream (STINGER /
  Sortledton one-edge-per-pair; CPR with an infinite window).
- `sample_pa:D P`: drop P% uniformly, except edges incident to an entity with
  more than D distinct neighbours. D = 100000 protects the 6 largest hubs
  (12.5% of edges); D = 10000 (60 hubs, 63% of edges) caps reduction at 1.59x,
  so only a handful of hubs can be protected inside CPR's range.

Random controls are matched to the sample_pa reductions. Results in
`e14_policies.csv` (same schema as `e14.csv`); reference rows from `e14.csv` /
`e14_sweep.csv`. Recall = chain entities reached by forward u backward taint.

| policy | param | reduction | A1 | A2 | A3 | A4 | union A1 / A2 / A3 / A4 | latency A1 (us) |
|---|---|---|---|---|---|---|---|---|
| full (ref) | - | 1.00x | 13/19 | 11/12 | 5/5 | 8/9 | 8238 / 46597 / 46268 / 46261 | 74813 |
| cpr (ref) | 10 min | 5.53x | 13/19 | 11/12 | 5/5 | 8/9 | 6533 / 36212 / 35878 / 35876 | 17857 |
| cpr (ref) | 1 h | 5.59x | 13/19 | 11/12 | 5/5 | 6/9 | 6451 / 35708 / 35367 / 35365 | 16436 |
| cpr (ref) | 1 d | 5.60x | 13/19 | 11/12 | 5/5 | 6/9 | 6289 / 35033 / 34694 / 34692 | 15990 |
| random (ref) | 50% | 2.00x | 10/19 | 11/12 | 5/5 | 8/9 | 65 / 220 / 102 / 101 | 74 |
| random (ref) | 90% | 10.00x | 9/19 | 11/12 | 4/5 | 5/9 | 21 / 57 / 15 / 12 | 25 |
| loggc | 1 h | 1.37x | 9/19 | 11/12 | 4/5 | 5/9 | 105 / 299 / 15 / 54 | 112 |
| loggc | 6 d | 1.99x | 7/19 | 11/12 | 4/5 | 5/9 | 7 / 103 / 4 / 47 | 12 |
| loggc | 8 d | 4.36x | 7/19 | 11/12 | 4/5 | 5/9 | 7 / 53 / 4 / 47 | 12 |
| loggc | unbounded | 4.96x | 7/19 | 11/12 | 4/5 | 5/9 | 7 / 11 / 4 / 5 | 8 |
| loggc_reach | 1 h | 1.40x | 12/19 | 11/12 | 5/5 | 6/9 | 7992 / 46528 / 46247 / 46248 | 44189 |
| loggc_reach | 6 d | 2.12x | 12/19 | 11/12 | 5/5 | 6/9 | 7988 / 46528 / 46247 / 46248 | 44508 |
| loggc_reach | 8 d | 5.05x | 12/19 | 11/12 | 5/5 | 6/9 | 7988 / 46524 / 46247 / 46248 | 43519 |
| loggc_reach | 10 d | 5.43x | 12/19 | 11/12 | 4/5 | 6/9 | 7988 / 46524 / 46168 / 46169 | 41652 |
| nodemerge | 1 s | 2.13x | 13/19 | 11/12 | 5/5 | 8/9 | 8238 / 46597 / 46268 / 46261 | 72977 |
| nodemerge | 1 h | 2.44x | 13/19 | 11/12 | 5/5 | 8/9 | 8238 / 46597 / 46268 / 46261 | 81460 |
| nodemerge | unbounded | 2.56x | 13/19 | 11/12 | 5/5 | 8/9 | 8238 / 46597 / 46268 / 46261 | 73225 |
| dedup | - | 5.60x | 13/19 | 11/12 | 5/5 | 6/9 | 6289 / 35004 / 34665 / 34663 | 15790 |
| sample_pa:100000 | 60% | 2.10x | 11/19 | 11/12 | 5/5 | 6/9 | 2955 / 185 / 41 / 36 | 26586 |
| random (control) | 52.48% | 2.10x | 10/19 | 11/12 | 4/5 | 8/9 | 61 / 209 / 50 / 47 | 59 |
| sample_pa:100000 | 90% | 4.70x | 8/19 | 11/12 | 5/5 | 5/9 | 28 / 55 / 18 / 19 | 37 |
| random (control) | 78.72% | 4.70x | 10/19 | 11/12 | 4/5 | 6/9 | 38 / 105 / 19 / 16 | 31 |
| sample_pa:100000 | 99% | 7.46x | 7/19 | 11/12 | 4/5 | 5/9 | 7 / 11 / 8 / 9 | 9 |
| random (control) | 86.6% | 7.46x | 9/19 | 11/12 | 4/5 | 6/9 | 23 / 70 / 18 / 16 | 33 |

Lifetime parameters are in ns in the CSV (1 h = 3.6e12, 6 d = 5.184e14, 8 d =
6.912e14, 10 d = 8.64e14, unbounded = 1e15 > the 10.98 d span).

What each family deletes (from per-entity hit/miss against `full`):

- **NodeMerge is lossless and stops at 2.56x.** Union sizes equal `full` to
  the entity on all four attacks. This is structural: a collapsed source has no
  in-edges, so its first read preserves every forward and backward path through
  it. It removes repetition, never a path, and there is not enough of that kind
  of repetition to go past 2.56x.
- **GC deletes the attacker's dead ends.** `loggc_reach` keeps the
  investigation scope intact (union within 3% of `full`), and loses exactly
  the artifacts the attacker wrote and nobody read: `/var/log/devc` (attack 1)
  and the dropped implants `eWq10bVcx` and `memhelp.so` (attack 4), from 1.40x
  at a 1 h lifetime. By LogGC's own definition these have no effect on live
  state, so they are garbage, and they are evidence. The entity-level rule
  (`loggc`) is far more destructive: at 1.37x it also removes the implant
  `/tmp/vUgefal` and both `/etc/group` reads, and the union collapses from 8 238
  to 105 entities, below random at 2x. It removes a temporary entity's earlier
  out-flows into live processes, which the reachability rule keeps.
- **Dedup is CPR's ceiling, and it costs what CPR's long windows cost.** 5.60x,
  identical recall to CPR 1 h / 1 d, losing the same two attack-4 implants.
  On fine-grained provenance CPR at >= 1 h *is* whole-stream deduplication.
- **Keeping the hubs does not protect evidence.** Summed over the 45 chain
  entities of the four attacks, sample_pa vs uniform random at matched
  reduction is 33 vs 33 at 2.10x (better on A1/A3, worse on A4), 29 vs 31 at
  4.70x and 27 vs 30 at 7.46x. Single-seed random varies by 1-2 entities
  (random 90 x3 seeds: A4 5-7/9), so at best a tie. The protected hubs do keep
  broad connectivity (A1 union 2 955 vs 61 at 2.10x), but the chains are not on
  them: 41 of the 45 chain entities have at most 5 distinct neighbours (dropped
  files 1-3 neighbours and 1-6 events, C2 flows exactly one neighbour; only
  /etc/passwd and /etc/group are wider), and those low-degree edges absorb the whole drop
  budget once hubs are exempt.

**Does any policy reach CPR's reduction without CPR's loss? No.** CPR itself is
lossless up to a 10 min window (5.53x) and loses 2/9 attack-4 entities at its
5.60x ceiling. No new family does as well. NodeMerge is lossless but stops at
2.56x. `loggc_reach` reaches 5.05-5.43x but has lost three chain entities
(A1 1, A4 2) from 1.40x on, plus one in attack 3 at 5.43x. `loggc`,
`sample_pa` and random lose more at every ratio. Every family that removes
paths, not just repetition, deletes chain entities below 2x on this corpus.
