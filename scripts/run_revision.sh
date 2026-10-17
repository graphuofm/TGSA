#!/bin/bash
# Final-revision experiment queue: everything the aligned evaluation needs
# that was not already on disk. Sequential on purpose (timing runs must not
# overlap), each step under a timeout, failures logged and skipped.
#
#   nohup scripts/run_revision.sh > results/revision_logs/queue.log 2>&1 &
#
# Corpora (large to small): LANL 1.05B, THEIA 97.3M, CADETS 22.5M, wiki-talk 7.8M.
# The matrix (E33) runs smallest first so partial results land early.
set -u
cd "$(dirname "$0")/.."
REPO=$PWD
BIN=/home/jding/dataset/bin
LOG=$REPO/results/revision_logs
mkdir -p "$LOG" results/e33_matrix results/e13_storage results/e18_recovery \
         results/e23_ablation results/e25_dock results/e27_shard results/e22_pipeline
export OMP_NUM_THREADS=8
export GNOMON_PHASE_BUDGET_SEC=900   # GraphOne query jobs get the same 15-minute budget as the other baselines

step() {  # step <name> <timeout-sec> <cmd...>
    local name=$1 tmo=$2; shift 2
    if [ -f "$LOG/$name.done" ]; then echo "[skip] $name"; return; fi
    echo "[$(date +%H:%M:%S)] start $name"
    timeout "$tmo" "$@" > "$LOG/$name.log" 2>&1
    local rc=$?
    echo "[$(date +%H:%M:%S)] end   $name rc=$rc"
    [ $rc -eq 0 ] && touch "$LOG/$name.done" || echo "rc=$rc" > "$LOG/$name.fail"
}

CORP="wiki cadets theia lanl"
declare -A FILE=( [wiki]=wiki-talk-temporal [cadets]=tc-cadets-e3 [theia]=tc-theia-e3 [lanl]=lanl-auth )

# ---- 1. the aligned matrix: in-process systems, smallest corpus first ----
for c in $CORP; do
    step e33_${c}_tgsr      7200  python3 scripts/run_e33.py --corpora $c --systems tgsr
    step e33_${c}_livegraph 14400 python3 scripts/run_e33.py --corpora $c --systems livegraph --phase_budget_sec 900
    step e33_${c}_rocksdb   14400 python3 scripts/run_e33.py --corpora $c --systems rocksdb --phase_budget_sec 900 --phases ingest,compact,q1,q2,q3,q4
    step e33_${c}_duckdb    14400 python3 scripts/run_e33.py --corpora $c --systems duckdb --phase_budget_sec 900 --phases ingest,compact,q1,q2,q3,q4
    step e33_${c}_stinger   3000  python3 scripts/run_e33.py --corpora $c --systems stinger --ingest_budget_sec 2700
    step e33_${c}_sortledton 6000 python3 scripts/run_e33.py --corpora $c --systems sortledton
    # GraphOne: its own harness, same query files, job 20 ingest + 28/34/35/36 queries
    step e33_${c}_graphone  14400 /usr/bin/time -v -o $LOG/e33_${c}_graphone.time \
        python3 scripts/run_graphone.py \
        --corpora ${FILE[$c]} --out results/e33_matrix/graphone.csv \
        --query-dir results/e33_matrix --threads 8 --timeout 7200 --no-plain32
done

# ---- 1b. the 16-byte-reference build (no inline timestamp) through the same harness
step notscache_build 3600 cmake --build build_notscache --target bench_e33_matrix -j8
for c in $CORP; do
    step e33nots_${c} 7200 build_notscache/bench_e33_matrix --dataset $BIN/${FILE[$c]}.bin \
        --system tgsr --phases ingest,q2,q4 --outdir results/e23_ablation/notscache_e33
done

# ---- 2. TGSR-only analyses on the corpora they were missing ----
for c in theia cadets; do
    step e13_${c} 7200 build/bench_e13_storage_read --dataset $BIN/${FILE[$c]}.bin \
        --algo all --window_frac 0.05 --trials 3 --threads 8 \
        --output results/e13_storage/e13_aligned.csv
done
for c in cadets theia lanl; do
    step e18cost_${c} 7200 build/bench_e18_recovery --dataset $BIN/${FILE[$c]}.bin \
        --mode cost --threads 8 --wal_dir /home/jding/dataset/e18_wal \
        --output results/e18_recovery/cost.csv
done
for c in theia lanl; do
    step e23rc_${c}  7200 build/bench_e23_ablation --dataset $BIN/${FILE[$c]}.bin \
        --mode runclaim --output results/e23_ablation/runclaim_${c}.csv
    step e23thr_${c} 7200 build/bench_e23_ablation --dataset $BIN/${FILE[$c]}.bin \
        --mode threshold --output results/e23_ablation/threshold.csv
    step e23tau_${c} 7200 build/bench_e23_ablation --dataset $BIN/${FILE[$c]}.bin \
        --mode tau --output results/e23_ablation/tau.csv
    for t in 1 2 3; do
      step e25_${c}_t$t 7200 build/bench_e25_dock --dataset $BIN/${FILE[$c]}.bin \
        --threads 8 --output results/e25_dock/e25.csv
    done
    step e27_${c}    7200 build/bench_e27_shard --dataset $BIN/${FILE[$c]}.bin \
        --shards 1,2,4,8 --threads_total 16 --output results/e27_shard/e27.csv
done

# ---- 3. concurrent ingestion and query, three systems, the two large corpora ----
step e26_theia 14400 python3 scripts/run_e26.py --corpora tc-theia-e3 --mixes 8:4 \
    --systems tgsr,livegraph,graphone
step e26_tgsr_mixes 14400 python3 scripts/run_e26.py --corpora tc-theia-e3,lanl-auth --mixes 8:1,4:4,2:8 --systems tgsr
step e26_lanl  21600 python3 scripts/run_e26.py --corpora lanl-auth --mixes 8:4 \
    --systems tgsr,livegraph,graphone

# ---- 4. the analytics pipeline on LANL, one system per process ----
# ---- RocksDB with its WAL on, for the durability panel
for c in theia lanl; do
    step e24_${c} 7200 build/bench_e24_rocksdb --dataset $BIN/${FILE[$c]}.bin \
        --windows 0.01 --samples 100 --threads 8 --dbdir /home/jding/dataset/e24_rocksdb \
        --output results/e24_rocksdb/e24.csv
done

for s in tgsr duckdb livegraph; do
    step e22_lanl_$s 7200 build/bench_e22_pipeline --dataset $BIN/lanl-auth.bin \
        --windows 0.001,0.01,0.1 --threads 8 --systems $s \
        --output results/e22_pipeline/e22.csv
done

echo "[$(date +%H:%M:%S)] queue finished"
