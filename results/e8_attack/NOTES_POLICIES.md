# E8b: more reduction policies on LANL (attack-edge survival)

Same graph, ground truth and metric as NOTES.md (699 red-team auth edges,
host-level forward taint from the first red-team event). Policies are defined in
benchmark/reduction_policies.hpp and shared with bench_e14_tc. Raw rows:
e8_lanl_policies.csv (baseline rows for full/cpr/random are in e8_lanl.csv).

| policy | param | reduction | attack-edge survival | node recall | taint |
|---|---|---|---|---|---|
| full | - | 1.00x | 699/699 (100%) | 97.7% | 29.0% |
| cpr | 60 s / 1 h / 24 h | 3.27 / 14.1 / 131.5x | 92.4 / 70.1 / 55.9% | 97.7% | 29.0% |
| dedup (first event per pair) | - | 2529x | 289/699 (41.3%) | 97.0% | 19.8% |
| sample_pa:14300 (keep top-2 hubs) | 60 / 90 / 99% | 1.96 / 3.78 / 5.24x | 40.9 / 9.3 / 1.0% | 84.6 / 58.0 / 20.0% | |
| random, matched to sample_pa | 49 / 74 / 81% | 1.96 / 3.78 / 5.24x | 50.4 / 27.5 / 20.0% | 86.2 / 75.1 / 70.5% | |
| loggc | 1 d / 30 d / 58 d | 1.00 / 1.00 / 1.07x | 100 / 100 / 95.6% | 97.7 / 97.7 / 94.1% | |
| loggc_reach | 5e6 s | 1.00x | 100% | 97.7% | |
| nodemerge | 1 h / 1 d / 58 d | 1.00 / 1.00 / 1.01x | 100 / 99.7 / 45.4% | 97.7% | |

Findings
1. Full deduplication is the limit of CPR: 2529x, 41% of attack edges survive,
   node recall still 97.0%. The metric the field uses stays blind at every ratio.
2. Keeping hubs makes sampling worse than plain random at the same reduction
   (9.3% vs 27.5% survival at 3.78x): the red-team edges are not on the hubs.
3. LogGC and NodeMerge do not reduce a host-level log: every host lives the whole
   trace and both sends and receives, so neither finds anything to remove. Their
   one non-trivial setting (nodemerge over the whole trace) removes 1% of edges and
   55% of the attack edges, because the attack's first contacts are exactly the
   "read-only initialization" events it collapses.
