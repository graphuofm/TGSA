# E11: Sustained triage throughput -- what the speedup makes affordable

_2026-08-21. Backward causality trace over an ENTIRE alert set, no sampling._

## Why this experiment exists

A 200x speedup on one query is not a pain point. The pain point is what the
speedup makes affordable. E9 showed Barry traces 150-700x faster than a columnar
engine; this experiment turns that into the operational claim: tracing every alert
stops being a budget decision.

The framing matters more in an agent deployment than a human one. A human analyst
triages tens of alerts a day and can wait 2 seconds. An agent issues probes
continuously, most of them speculative, and cannot.

## Results (5000 alerts each, window_frac = 0.1, single core)

| graph | edges | sweep time | queries/s | median | p95 | max | avg ancestors |
|---|---|---|---|---|---|---|---|
| sx-askubuntu | 964K | 2.74 s | 1 824 | 0.7 us | 5.4 ms | 12.1 ms | 511 |
| wiki-talk-temporal | 7.8M | 42.8 s | 117 | 0.7 us | 89.1 ms | 160.5 ms | 2 283 |
| cit-Patents | 16.5M | 0.006 s | 779 292 | 1.0 us | 3.1 us | 57 us | 1.6 |

cit-Patents is a citation DAG: edges point to older work, so time-respecting paths
barely exist and it traces nothing. Keep it for scale, not for provenance realism.

## The affordability arithmetic

Taking wiki-talk-temporal as the pessimistic case (7.8M edges, avg 2283 ancestors
per trace) and a SOC receiving 10 000 alerts a day:

| | per alert | 10 000 alerts |
|---|---|---|
| DuckDB traversal (E9, \|R\| > 1000 bucket) | 2.43 s | **6.7 hours** |
| Barry (this sweep, measured end to end) | 8.6 ms avg | **86 seconds** |

86 seconds of one core per day is not a budget decision. 6.7 hours is. That is the
difference between "trace the alerts an analyst picked" and "trace everything",
and it is the claim the paper should make -- not that analysts wait less.

## The number that must not be quoted alone

Median latency is 0.7 us and p95 is 89 ms: **five orders of magnitude apart**.
Sweep time is set almost entirely by a handful of heavy traces, so "1824 queries/s"
without the distribution beside it is misleading. Throughput here is a property of
the dependency-explosion tail, not of the engine's typical cost.

Same conclusion as E7 and E9: on this workload, report distributions.

## Reproduce

```
./build/bench_e11_sweep --graph data/bin/<ds> --num_alerts 5000 --window_frac 0.1 \
    --threads 8 --trial 0 --output e11.csv --detail e11_detail.csv
# with real ground truth once LANL lands:
./build/bench_e11_sweep --graph data/bin/lanl-auth --alerts data/bin/lanl-auth.redteam.tsv ...
```
