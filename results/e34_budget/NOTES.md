# E34: lossless-short vs reduced-long under a fixed storage budget

bench_e34_budget. Both policies are given the same budget B (a fraction of the
lossless full-history size) at investigation time T_inv = last ground-truth
event + delay. LOSSLESS-SHORT keeps every event inside the largest horizon that
fits B. REDUCED-LONG applies CPR with the smallest window that fits B over the
whole history; when even CPR at its ceiling does not fit, it adds a horizon
(note `cpr-ceiling+horizon`). Budget 1, delay 0 reproduces E8 (699/699) and
E14 (CADETS chain recall per attack) and is marked `verify` in e34.csv.

LANL, delay 0 (attack-edge survival / host recall):

| budget | lossless-short horizon | lossless-short | reduced-long CPR window | reduced-long |
|---|---|---|---|---|
| 1/2 | 29.6 d | 100% / 97.7% | none needed | 100% / 97.7% |
| 1/4 | 15.2 d | 14.0% / 55.1% | 2 s | 99.9% / 97.7% |
| 1/8 | 7.3 d | 6.6% / 33.8% | 91 s | 90.6% / 97.7% |
| 1/16 | 3.2 d | 6.4% / 33.8% | 24 min | 74.0% / 97.7% |
| 1/32 | 1.5 d | 1.4% / 16.1% | 1.6 h | 68.4% / 97.7% |
| 1/64 | 0.8 d | 1.1% / 4.6% | 4.6 h | 63.4% / 97.7% |

CADETS (four attacks, delays 0/1/7 days for attack 1): CPR's ceiling (5.6x) is
reached at budget 1/4, after which reduced-long must also shorten its horizon and
keeps only 2-9% of attack events; lossless-short keeps 100% down to 1/8 at delay
0 but loses the attack entirely once the delay exceeds its horizon (attack 1,
delay 7 d: 0% at every budget for lossless-short; reduced-long keeps 100% at 1/2).

Finding, stated plainly because it cuts against the simple version of the
paper's thesis: on a coarse host-level log (LANL), when storage is the binding
constraint, a reduced log kept for the whole history preserves far more attack
evidence than a lossless log kept for a shorter horizon (90.6% vs 6.6% at 1/8).
On the fine-grained CADETS log the order reverses at small budgets because CPR
hits its ceiling. The argument for lossless retention therefore has to be made
with the storage cost removed (TGSA's bytes per event and retention by prefix
release), not as "lossless beats reduced at equal storage".
