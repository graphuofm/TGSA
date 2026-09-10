# E8: Attack-edge survival and the investigation-window curve

_LANL "Comprehensive, Multi-Source Cyber-Security Events" (Kent 2015):
1 051 430 459 authentication events, 17 666 hosts, 58.0 days, 0 out-of-order.
749 red team events, all 749 resolved to node ids, 706 matched an auth edge._

Graph: `computer -> computer` per authentication, successful auth only (class 1),
1 038 590 151 edges. Load 255 s, 96 GB RSS.

## Finding 1 -- lossy reduction deletes attack evidence, and the standard metric cannot see it

| policy | reduction | **attack-edge survival** | node-level recall | fwd latency |
|---|---|---|---|---|
| none | 1.00x | **699/699 (100.0%)** | 97.7% | 17.1 s |
| CPR 60 s | 3.27x | 646/699 (92.4%) | 97.7% | 4.4 s |
| CPR 1 h | 14.08x | 490/699 (70.1%) | 97.7% | 0.66 s |
| CPR 24 h | **131.51x** | **391/699 (55.9%)** | **97.7%** | 74 ms |
| random 50% | 2.00x | 344/699 (49.2%) | 85.9% | 8.8 s |
| random 90% | 10.00x | 60/699 (8.6%) | 58.4% | 1.5 s |

Across the whole CPR range -- 1x to 131x reduction -- node-level recall does not
move: 97.7%, every time. Over the same range, attack-edge survival falls from 100%
to 55.9%. **Forty-four percent of the red team's evidence is deleted while the
metric the field validates reduction with reports no change at all.**

This is why the metric was switched mid-experiment. A pilot on the DNS channel
using node recall showed 8.4x CPR reduction costing nothing (92.8% -> 92.8%) and
would have concluded that reduction is free. It is not; node recall is simply blind
to it, because taint over-approximates enough to reach the hosts anyway via other
paths. Whether the specific edge an investigator must follow still exists is a
different question, and it is the one that matters.

Systems reporting three-orders-of-magnitude reduction at "99.99% accuracy" are
reporting *detection* accuracy. This experiment does not contradict those numbers;
it says they do not measure what an investigation needs.

## Finding 2 -- there is no usable investigation window

Forward taint from the first red team compromise, sweeping the horizon
(single load, all windows measured in-process):

| window | real duration | tainted hosts | % of network | recall of compromised hosts | latency |
|---|---|---|---|---|---|
| 0.0001 | 8.4 min | 12 | 0.1% | 3.6% | 667 us |
| 0.001 | 1.4 h | 45 | 0.3% | 7.9% | 6.5 ms |
| 0.01 | 14 h | 95 | 0.5% | 10.2% | 72 ms |
| 0.05 | 2.9 d | 808 | 4.6% | 22.6% | 435 ms |
| 0.2 | 11.6 d | 2 774 | 15.7% | 88.9% | 2.55 s |
| 1.0 | 58 d | 5 121 | 29.0% | 97.7% | 17.1 s |

At a horizon an analyst would actually accept (1.4 h) the trace finds 7.9% of the
compromised hosts -- it misses the attack. Reaching 88.9% recall requires an 11.6
day horizon, which implicates 2 774 machines, 15.7% of the network. **There is no
setting that is both accurate and actionable.**

Real APT dwell time is measured in weeks, so the horizon the workload *requires* is
exactly the horizon at which the answer stops being usable. That is the shape of
the problem this domain has, quantified here on real events against real ground
truth.

Two consequences:

- **Report performance in the usable band.** Barry answers the queries that would
  actually be issued (8 min to 3 day horizons) in 667 us to 435 ms. The 17.1 s
  figure is the cost of a query nobody should run, and belongs in the paper as a
  boundary, not as a headline.
- **This is the empirical case for version-granularity recovery.** Whole-node
  rollback at 88.9% recall means discarding 2 774 production machines. E7 measured
  17-57% taint on social proxies; this is the same phenomenon with the security
  consequence attached.

## Method note

Latency tracks |T| almost linearly (12 hosts -> 667 us, 5 121 hosts -> 17.1 s,
~3 ms per tainted host). At an average degree of 59 517, expanding one host means
touching tens of thousands of in-range edges, and no layout avoids that: the work
is the answer size. Binary-search expansion (sorted buckets) bought only 10% here
(18.9 s -> 17.1 s) for exactly this reason -- when the horizon is unbounded, almost
every edge is in range and there is nothing to skip. It is worth 2.5x on graphs
where the horizon actually prunes.

## Reproduce

```
./build/bench_e8_attack --graph /home/jding/dataset/bin/lanl-auth \
    --policy <full|cpr|random> --param P --classes 1 --threads 16 \
    --windows 0.0001,0.001,0.01,0.05,0.2,1.0 --trial 0 --output e8.csv
```
