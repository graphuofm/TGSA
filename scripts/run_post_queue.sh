#!/bin/bash
# Steps that must run on a quiet machine after scripts/run_revision.sh:
#   1. TGSA THEIA matrix rows redone (the first run had an exhausted vertex
#      index; its rows and .ref/.prefix are invalid)
#   2. the 16-byte-reference build through the matrix harness (the queue's
#      build step failed: build_notscache predated the target)
#   3. the sharding sweep on THEIA (first run hit the same exhaustion)
#   4. TGSA ingestion trials, both arrival protocols, three trials
# Each step logs to results/revision_logs/post_<name>.log.
set -u
cd "$(dirname "$0")/.."
BIN=/home/jding/dataset/bin
LOG=results/revision_logs
export OMP_NUM_THREADS=8
run() { local name=$1; shift; echo "[$(date +%H:%M:%S)] start $name"; "$@" > "$LOG/post_$name.log" 2>&1; echo "[$(date +%H:%M:%S)] end   $name rc=$?"; }

run theia_tgsr python3 scripts/run_e33.py --corpora theia --systems tgsr --force
for c in wiki cadets theia lanl; do
    declare -A FILE=( [wiki]=wiki-talk-temporal [cadets]=tc-cadets-e3 [theia]=tc-theia-e3 [lanl]=lanl-auth )
    run nots_$c build_notscache/bench_e33_matrix --dataset $BIN/${FILE[$c]}.bin \
        --system tgsr --phases ingest,q2,q4 --threads 8 --outdir results/e23_ablation/notscache_e33
done
run e27_theia build/bench_e27_shard --dataset $BIN/tc-theia-e3.bin --shards 1,2,4,8 \
    --threads_total 16 --output results/e27_shard/e27.csv
run ingest_trials scripts/run_tgsa_ingest_trials.sh
echo "[$(date +%H:%M:%S)] post-queue finished"
