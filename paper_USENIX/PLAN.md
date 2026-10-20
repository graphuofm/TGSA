# USENIX Security draft: evidence map (created 2026-10-20)

Thesis: log reduction, validated only by detection accuracy, destroys the
evidence an investigation needs; the storage pressure that forces it is a
layout problem that TGSA removes. Second paper of the pair planned on
2026-08-21 (project memory "provenance pivot"): the SIGMOD paper is the engine,
this one is the security claim built on it.

## Evidence already on disk
| claim | experiment | result files | status |
|---|---|---|---|
| CPR deletes 44% of red-team edges at 131x while node recall stays 97.7% | E8 (LANL) | results/e8_attack/e8_lanl*.csv, NOTES.md | done |
| no investigation horizon is both accurate and actionable (LANL) | E8 | results/e8_attack/e8_lanl_win.csv | done |
| fine-grained logs: 5.6x ceiling, 0% loss first-contact, 22% loss on infra reuse | E14 (CADETS) | results/e14_tc/e14*.csv, NOTES.md | done |
| forward/backward taint reconstructs the documented chain in 133 us | E14, E7 | results/e14_tc, results/e7_provenance | done |
| dependency explosion is heavy-tailed; rollback would discard 17-57% of state | E7 | results/e7_provenance/e7_window_sweep.csv | done (SNAP proxies) |
| ingestion at collection rate, retention as prefix release, recovery by replay | SIGMOD E33/E18 | results/e33_matrix, results/e18_recovery | done |
| per-host event rate under routine load (~80K/s) | docs/log_reduction_findings.pdf | measurement not in results/ | redo and record |

## Gaps to close before a draft
1. THEIA ground truth: resolve the TC report's THEIA attacks to node ids as done for
   CADETS (data/tc_cadets_groundtruth.tsv), rerun E14 on THEIA.
2. Reduction schemes beyond CPR: LogGC-style garbage collection, NodeMerge, sampling
   with provenance awareness, lossy aggregation; same survival metric.
3. A published detector or investigation tool run on reduced vs unreduced input
   (candidates: UNICORN, HOLMES-style tag propagation) so the cost shows up in a
   detector's own output, not only in our metric.
4. Collection-rate measurement: an auditd/eBPF collector on one host, events/s under
   routine load, against TGSA's single-host ingestion rate.
5. Retention experiment framed for security: evidence available at investigation time
   under 7/30/90-day policies, reduced vs lossless.
6. Threat model section and the reviewer-facing "why not just store everything in
   Kafka/Elastic" comparison (one baseline that security papers recognize).

## Venue notes
- USENIX Security: 13 pages body excluding references/appendix; double-blind; artifact
  evaluation badges. Replace usenix2019_v3.sty with the year's official style file.
- Never cite the SIGMOD submission by name while it is under review; cite as
  "[anonymous, under submission]" and put the engine summary in §4.

## Work in flight (2026-10-20)
| gap | owner | output |
|---|---|---|
| 1 THEIA ground truth + E14 | agent | data/tc_theia_groundtruth.tsv, results/e14_tc/e14_theia.csv, NOTES_THEIA.md |
| 2 reduction policies (loggc, nodemerge, dedup, sample_pa) | agent | benchmark/reduction_policies.hpp, results/e8_attack/e8_lanl_policies.csv, results/e14_tc/e14_policies.csv |
| 3 UNICORN on reduced streams | agent | baselines/unicorn (ignored), results/unicorn/README.md |
| 4 collector event rate (no root: strace proxy) | scripts/collector_rate.sh | results/e36_collector/rate.csv |
| 5 retention under a storage budget | agent | benchmark/bench_e34_budget.cpp, results/e34_budget/ |
| 6 Elasticsearch / Kafka baseline | agent | benchmark/bench_e35_siem.py, results/e35_siem/README.md |

## Cleanup list (run when the above finish)
- /home/jding/dataset/es_data, /home/jding/dataset/kafka_data, /home/jding/dataset/graphone, e33_rocksdb_*, e18_wal
- baselines/unicorn parsed graphs and sketches (keep the clone, drop outputs)
- build_rate/, any *.tgz under build dirs, results/revision_logs/*.time
- results/e33_matrix/smoke if recreated; scratch dumps under /tmp are session-local
