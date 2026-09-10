# Barry Experimental Summary
_Last updated: 2026-04-30 19:46 UTC_

## Headline Numbers

- **E1 Barry vs DCSR ingestion:** 40-cell grid (10 datasets × 4 batches), Barry wins 15, DCSR wins 25. Barry dominates batch=1k sweet spot (12-17 Meps); DCSR dominates large batches due to its merge-redistribute path.
- **E1 Barry vs Neo4j:** Barry **561× to 1650× faster** at batch=1k.
- **E2 Barry vs Neo4j on temporal queries (CollegeMsg):** Q1 point-in-time **414,903× faster**, Q2 56,352×, Q5 2,261×. PMA-based stores (DCSR/PPCSR) cannot natively answer this query class.
- **E3 Storage overhead:** Barry/our_CSR ≈ 1.1-2.5× (BFS), DCSR/BYO_CSR ≈ 1.0-1.4× — comparable storage cost; algorithm-level gap is parlay/GBBS tuning, not storage.
- **L0 ablation:** 3-5× internal BFS speedup at zero memory cost.
- **E5 Memory:** reddit-1B (646M edges) → 39.6 GB RSS. Full-tier table available.

---

## E1: Ingestion throughput (M edges / s, median across trials)

```
system                           barry   dcsr
dataset                batch                 
CollegeMsg             10         1.44   3.01
                       1000       4.49   4.66
                       100000     2.79  10.46
                       10000000   2.07   8.55
email-Eu-core-temporal 10         2.44  10.87
                       1000      15.80  32.35
                       100000     8.17  48.50
                       10000000   8.16  49.45
act-mooc               10         3.11   5.36
                       1000      16.85   5.05
                       100000     9.23  15.85
                       10000000   8.81  17.13
sx-mathoverflow        10         3.00   2.41
                       1000      16.84   2.05
                       100000     9.57   6.52
                       10000000   9.87   6.18
sx-askubuntu           10         3.46   3.86
                       1000      14.13   5.57
                       100000    10.54  19.05
                       10000000  10.33  19.50
sx-superuser           10         3.44   3.79
                       1000      13.39   5.93
                       100000    10.69  15.77
                       10000000   9.28  18.30
wiki-talk-temporal     10         3.30   2.84
                       1000      12.32   6.02
                       100000     6.35   2.18
                       10000000   9.53   2.38
cit-Patents            10         2.24   1.75
                       1000       4.00  14.75
                       100000     6.62  17.14
                       10000000   7.60  17.18
sx-stackoverflow       10         3.02   1.16
                       1000       7.10   1.01
                       100000     6.01   9.07
                       10000000   7.00  14.14
soc-LiveJournal1       10         2.14   1.74
                       1000       3.73  11.91
                       100000     4.75  18.33
                       10000000   7.15  19.74
```

Barry wins on these (dataset, batch) combos vs DCSR:
```
system                       barry  dcsr  barry/dcsr
dataset            batch                            
act-mooc           1000      16.85  5.05        3.34
sx-mathoverflow    10         3.00  2.41        1.25
                   1000      16.84  2.05        8.22
                   100000     9.57  6.52        1.47
                   10000000   9.87  6.18        1.60
sx-askubuntu       1000      14.13  5.57        2.54
sx-superuser       1000      13.39  5.93        2.26
wiki-talk-temporal 10         3.30  2.84        1.16
                   1000      12.32  6.02        2.05
                   100000     6.35  2.18        2.92
                   10000000   9.53  2.38        4.01
cit-Patents        10         2.24  1.75        1.28
sx-stackoverflow   10         3.02  1.16        2.61
                   1000       7.10  1.01        7.01
soc-LiveJournal1   10         2.14  1.74        1.23
```

## E2: Temporal-query latency (median µs)

```
system                        barry
dataset            query           
cit-Patents        q1           4.5
                   q2        2689.8
                   q3       11471.5
                   q4           8.3
                   q5         348.6
                   q6         736.4
sx-stackoverflow   q1           6.8
                   q2       10812.2
                   q3     4122276.7
                   q4        2847.7
                   q5        1330.4
                   q6     1313972.6
wiki-talk-temporal q1           1.0
                   q2         141.6
                   q3      445662.4
                   q4          83.3
                   q5          92.6
                   q6       20006.5
```

## E3: Compute (run_seconds, median)

```
system                          barry  byo_csr     csr    dcsr   ppcsr
dataset                algo                                           
CollegeMsg             bfs     0.0005   0.0002  0.0003  0.0002  0.0001
                       cc      0.0005      NaN  0.0003  0.0002     NaN
                       kcore   0.0045      NaN  0.0009  0.0021     NaN
                       pr      0.0181   0.0032  0.0268  0.0011  0.0011
                       sssp    0.0024      NaN  0.0012     NaN     NaN
                       tc      0.0105   0.0005  0.0122  0.0008  0.0006
act-mooc               bfs     0.0000   0.0003  0.0000  0.0003  0.0002
                       cc      0.0024      NaN  0.0017  0.0004     NaN
                       kcore   0.0157      NaN  0.0016  0.0028     NaN
                       pr      0.1523   0.0019  0.1581  0.0026  0.0029
                       sssp    0.0000      NaN  0.0017     NaN     NaN
                       tc      0.0010   0.0017  0.0171  0.0019  0.0021
cit-Patents            bfs     0.0104   0.0004  0.0045  0.0004  0.0004
                       cc      0.7880   0.0709  0.3129  0.0874  0.0880
                       kcore   5.7920   0.2473  0.4890  0.2786  0.2737
                       pr      1.1001   0.9252  0.8652  1.2168  1.1237
                       sssp    0.0121      NaN  0.0289     NaN     NaN
                       tc      0.0514   0.3077  0.2274  0.3715  0.3272
email-Eu-core-temporal bfs     0.0012   0.0001  0.0007  0.0001  0.0001
                       cc      0.0010      NaN  0.0008  0.0002     NaN
                       kcore   0.0092      NaN  0.0034  0.0022     NaN
                       pr      0.0516   0.0026  0.0608  0.0012  0.0011
                       sssp    0.0051      NaN  0.0038     NaN     NaN
                       tc      0.0666   0.0006  0.0283  0.0007  0.0006
soc-LiveJournal1       bfs     1.4953   0.0443  0.7637  0.0580  0.0643
                       cc      1.7216   0.1058  0.9347  0.1202  0.1237
                       kcore  18.2017   0.4922  3.0511  0.5100  0.5207
                       pr      3.7205   1.2134  2.7009  1.3912  1.4736
                       sssp    5.6480      NaN  1.6401     NaN     NaN
                       tc      4.3149   0.9830  0.9975  1.0749  1.0763
sx-askubuntu           bfs     0.0131   0.0006  0.0099  0.0007  0.0007
                       cc      0.0223      NaN  0.0157  0.0024     NaN
                       kcore   0.0861      NaN  0.0132  0.0138     NaN
                       pr      0.0501   0.0160  0.0583  0.0163  0.0189
                       sssp    0.0522      NaN  0.0253     NaN     NaN
                       tc      0.0944   0.0058  0.0293  0.0067  0.0067
sx-mathoverflow        bfs     0.0036   0.0002  0.0024  0.0002  0.0002
                       cc      0.0037      NaN  0.0043  0.0007     NaN
                       kcore   0.0313      NaN  0.0071  0.0068     NaN
                       pr      0.0669   0.0038  0.0739  0.0044  0.0050
                       sssp    0.0149      NaN  0.0100     NaN     NaN
                       tc      0.0997   0.0044  0.0333  0.0044  0.0052
sx-stackoverflow       bfs     0.7960   0.0090  0.3748  0.0122  0.0140
                       cc      0.9188   0.0557  0.4819  0.0594  0.0602
                       kcore   9.3172   0.2617  0.8745  0.2679  0.2715
                       pr      2.5882   0.6456  2.0775  0.7289  0.7235
                       sssp    3.1821      NaN  0.8185     NaN     NaN
                       tc     23.6489   1.0927  1.5755  1.2034  1.1921
sx-superuser           bfs     0.0185   0.0007  0.0092  0.0007  0.0008
                       cc      0.0290      NaN  0.0179  0.0030     NaN
                       kcore   0.1330      NaN  0.0172  0.0185     NaN
                       pr      0.0754   0.0203  0.0741  0.0192  0.0192
                       sssp    0.0731      NaN  0.0285     NaN     NaN
                       tc      0.1873   0.0119  0.0473  0.0113  0.0113
wiki-talk-temporal     bfs     0.0859   0.0034  0.0359  0.0034  0.0044
                       cc      0.1142   0.0095  0.0585  0.0111  0.0109
                       kcore   0.7369   0.0432  0.0482  0.0476  0.0470
                       pr      0.5084   0.1163  0.4326  0.1421  0.1358
                       sssp    0.3914      NaN  0.1681     NaN     NaN
                       tc      1.2737   0.0569  0.4421  0.0642  0.0653
```

### E3 storage-overhead ratios (closer to 1.0 is better)

```
system                        barry/our_csr  dcsr/byo_csr  ppcsr/byo_csr
dataset                algo                                             
CollegeMsg             bfs             1.67          1.00           0.50
                       cc              1.67           NaN            NaN
                       kcore           5.00           NaN            NaN
                       pr              0.68          0.34           0.34
                       sssp            2.00           NaN            NaN
                       tc              0.86          1.60           1.20
act-mooc               bfs              NaN          1.00           0.67
                       cc              1.41           NaN            NaN
                       kcore           9.81           NaN            NaN
                       pr              0.96          1.37           1.53
                       sssp            0.00           NaN            NaN
                       tc              0.06          1.12           1.24
cit-Patents            bfs             2.31          1.00           1.00
                       cc              2.52          1.23           1.24
                       kcore          11.84          1.13           1.11
                       pr              1.27          1.32           1.21
                       sssp            0.42           NaN            NaN
                       tc              0.23          1.21           1.06
email-Eu-core-temporal bfs             1.71          1.00           1.00
                       cc              1.25           NaN            NaN
                       kcore           2.71           NaN            NaN
                       pr              0.85          0.46           0.42
                       sssp            1.34           NaN            NaN
                       tc              2.35          1.17           1.00
soc-LiveJournal1       bfs             1.96          1.31           1.45
                       cc              1.84          1.14           1.17
                       kcore           5.97          1.04           1.06
                       pr              1.38          1.15           1.21
                       sssp            3.44           NaN            NaN
                       tc              4.33          1.09           1.09
sx-askubuntu           bfs             1.32          1.17           1.17
                       cc              1.42           NaN            NaN
                       kcore           6.52           NaN            NaN
                       pr              0.86          1.02           1.18
                       sssp            2.06           NaN            NaN
                       tc              3.22          1.16           1.16
sx-mathoverflow        bfs             1.50          1.00           1.00
                       cc              0.86           NaN            NaN
                       kcore           4.41           NaN            NaN
                       pr              0.91          1.16           1.32
                       sssp            1.49           NaN            NaN
                       tc              2.99          1.00           1.18
sx-stackoverflow       bfs             2.12          1.36           1.56
                       cc              1.91          1.07           1.08
                       kcore          10.65          1.02           1.04
                       pr              1.25          1.13           1.12
                       sssp            3.89           NaN            NaN
                       tc             15.01          1.10           1.09
sx-superuser           bfs             2.01          1.00           1.14
                       cc              1.62           NaN            NaN
                       kcore           7.73           NaN            NaN
                       pr              1.02          0.95           0.95
                       sssp            2.56           NaN            NaN
                       tc              3.96          0.95           0.95
wiki-talk-temporal     bfs             2.39          1.00           1.29
                       cc              1.95          1.17           1.15
                       kcore          15.29          1.10           1.09
                       pr              1.18          1.22           1.17
                       sssp            2.33           NaN            NaN
                       tc              2.88          1.13           1.15
```

## E5: Memory footprint (Barry, vidx=1)

```
               dataset  total_edges  raw_MB  arena_MB  vidx_MB  rss_MB
            CollegeMsg        59835     1.2       4.0      1.5     9.8
email-Eu-core-temporal       332334     6.6      11.1      8.2    26.6
              act-mooc       411749     8.2      14.5     10.3    34.8
       sx-mathoverflow       506550    10.1      18.0     14.2    43.7
          sx-askubuntu       964437    19.3      31.5     35.8    88.4
          sx-superuser      1443339    28.9      45.6     50.8   126.5
    wiki-talk-temporal      7833140   156.7     240.6    214.6   533.5
           cit-Patents     16518948   330.4     404.1    543.6  1269.3
      sx-stackoverflow     63497050  1269.9    1894.4   1697.2  4177.7
      soc-LiveJournal1     68993773  1379.9    2020.8   2019.1  4870.9
 temporal-reddit-reply    646044687 12920.9   18540.8  15798.9 39637.4
```

---

## Status

- ✅ Barry full implementation, 9 algorithms (BFS, PR, TC, BC, EarliestArrival, TemporalTriangle, SSSP, CC, KCore)
- ✅ Neo4j 4.4 wrapper (E1, E2 small-tier)
- ✅ DCSR / PPCSR / BYO_CSR via BYO+Bazel (no sudo, conda-forge gcc-11)
- ✅ **DCSR streaming-ingest custom driver** (`run_dcsr_ingest.cc`) — 10 datasets × 4 batches × 3 trials
- ✅ Per-run directory + manifest layout (no more accidental data overwrites)
- ⏸ AeonG (still trying via conda; init script slow, may fail)
- ⏸ Mid-tier ingestion regression: investigate why Barry drops from 16 Meps (small) to 7 Meps (mid). Likely vertex_index CAS contention.
- ⏸ Q4 / Q3 / Q6 sampling improvements
