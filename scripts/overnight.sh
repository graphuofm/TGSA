#!/bin/bash
# Overnight sweep: Barry on full-tier + BYO baselines on full-tier.
# Logs to /home/jding/Barry/logs/overnight_*.log
# Run order: cheap experiments first (so partial completion still gives data)

set -u
ROOT=/home/jding/Barry
LOGS=$ROOT/logs/overnight_$(date +%Y%m%d_%H%M%S)
mkdir -p "$LOGS"
echo "logs in $LOGS"

# Mid-tier datasets we expect to finish overnight (16M-69M edges).
MID_TIER=(cit-Patents wiki-talk-temporal sx-stackoverflow soc-LiveJournal1)

# Smaller-tier we re-run with the new algos (sssp/cc/kcore).
SMALL_TIER=(CollegeMsg email-Eu-core-temporal sx-mathoverflow act-mooc \
            sx-askubuntu sx-superuser)

# ----------------------------------------------------------------- Phase 1
# E5 memory footprint on everything we have, including reddit-1B.
# Cheap (one run per dataset), and gives the paper's Table 2.
echo "=== Phase 1: E5 memory on full-tier + reddit ==="
python3 $ROOT/scripts/run_all.py e5 \
    --datasets "${SMALL_TIER[@]}" "${MID_TIER[@]}" temporal-reddit-reply \
    --threads 16 --timeout 1800 \
    > "$LOGS/01_e5_full.log" 2>&1
echo "phase 1 exit: $?"

# ----------------------------------------------------------------- Phase 2
# Re-run E3 on small-tier with the expanded algo set (now 6 algos).
# Reset CSV first so old data does not collide with new schema.
echo "=== Phase 2: E3 (bfs+pr+tc+sssp+cc+kcore) on small-tier ==="
mv $ROOT/results/e3_compute/e3.csv $ROOT/results/e3_compute/e3_v2_old.csv 2>/dev/null
python3 $ROOT/scripts/run_all.py e3 \
    --datasets "${SMALL_TIER[@]}" \
    --threads 16 --trials 3 --timeout 1800 \
    > "$LOGS/02_e3_small_full_algos.log" 2>&1
echo "phase 2 exit: $?"

# ----------------------------------------------------------------- Phase 3
# Mid-tier ingestion (E1) + scalability (E4) on sx-stackoverflow.
echo "=== Phase 3: E1 ingestion on mid-tier ==="
python3 $ROOT/scripts/run_all.py e1 \
    --datasets "${MID_TIER[@]}" \
    --threads 16 --trials 3 --timeout 1800 \
    > "$LOGS/03_e1_midtier.log" 2>&1
echo "phase 3 exit: $?"

# ----------------------------------------------------------------- Phase 4
# Mid-tier temporal queries (Q1..Q6).
echo "=== Phase 4: E2 queries on mid-tier ==="
python3 $ROOT/scripts/run_all.py e2 \
    --datasets cit-Patents wiki-talk-temporal sx-stackoverflow \
    --threads 16 --trials 2 --timeout 3600 \
    > "$LOGS/04_e2_midtier.log" 2>&1
echo "phase 4 exit: $?"

# ----------------------------------------------------------------- Phase 5
# Mid-tier compute (Barry vs naive CSR) — slowest, but most informative.
echo "=== Phase 5: E3 on mid-tier ==="
python3 $ROOT/scripts/run_all.py e3 \
    --datasets "${MID_TIER[@]}" \
    --threads 16 --trials 2 --timeout 3600 \
    > "$LOGS/05_e3_midtier.log" 2>&1
echo "phase 5 exit: $?"

# ----------------------------------------------------------------- Phase 6
# BYO baselines (DCSR / PPCSR / BYO_CSR) on mid-tier datasets.
echo "=== Phase 6: BYO baselines on mid-tier ==="
RES=$ROOT/results/e3_compute
{ echo "system,dataset,trial,threads,algo,run_seconds";
  for sys in csr dcsr ppcsr; do
    BIN=$ROOT/baselines/BYO/bazel-bin/benchmarks/run_structures/run_$sys
    for ds in "${MID_TIER[@]}"; do
      gbbs=/home/jding/dataset/bin/$ds.gbbs
      if [ ! -f "$gbbs" ]; then
        # Convert if missing.
        $ROOT/build/barry_bin_to_gbbs /home/jding/dataset/bin/$ds.bin $gbbs \
          >> "$LOGS/06_gbbs_convert.log" 2>&1
      fi
      out=$($BIN -src 0 -s -rounds 3 $gbbs 2>&1 | grep -E "^# # # (BFS|PageRank|TriangleCount|KCore|SimpleUnionCC),")
      while IFS= read -r line; do
        algo=$(echo "$line" | awk -F',' '{print $1}' | sed 's/# # # //')
        secs=$(echo "$line" | awk -F',' '{print $2}' | tr -d ' ')
        case "$algo" in BFS) algo=bfs;; PageRank) algo=pr;; TriangleCount) algo=tc;;
                        KCore) algo=kcore;; SimpleUnionCC) algo=cc;; *) continue;; esac
        sysname=$sys
        [ "$sys" = "csr" ] && sysname=byo_csr
        echo "$sysname,$ds,0,16,$algo,$secs"
      done <<< "$out"
    done
  done
} > $RES/byo_baselines_midtier.csv 2> "$LOGS/06_byo_midtier.err"
echo "phase 6 done; rows in byo_baselines_midtier: $(wc -l < $RES/byo_baselines_midtier.csv)"

# ----------------------------------------------------------------- summary
echo "=== overnight done at $(date) ==="
ls -la $ROOT/results/*/*.csv 2>&1 | tail
echo "logs: $LOGS"
