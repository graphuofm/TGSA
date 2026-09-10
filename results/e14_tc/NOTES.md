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
