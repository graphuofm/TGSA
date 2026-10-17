#!/bin/bash
# TGSA ingestion on the four aligned corpora, quiet machine, two arrival
# protocols, three trials each:
#   batched  64K-event batches through bench_e33_matrix (the aligned protocol
#            every system in Figure 5 gets)
#   single   the whole corpus in one insert_batch call (bench_e33_matrix
#            --batch 0), the engine's ceiling, quoted in the analysis text
# Run only after scripts/run_revision.sh has finished (nothing else running).
set -u
cd "$(dirname "$0")/.."
BIN=/home/jding/dataset/bin
OUT=results/e33_matrix/tgsr_ingest_trials
mkdir -p "$OUT"
for c in wiki-talk-temporal tc-cadets-e3 tc-theia-e3 lanl-auth; do
  for t in 1 2 3; do
    build/bench_e33_matrix --dataset $BIN/$c.bin --system tgsr --phases ingest,compact \
        --threads 8 --outdir "$OUT/batched_t$t" > "$OUT/batched_${c}_t$t.log" 2>&1
    build/bench_e33_matrix --dataset $BIN/$c.bin --system tgsr --phases ingest,compact \
        --threads 8 --batch 0 --outdir "$OUT/single_t$t" > "$OUT/single_${c}_t$t.log" 2>&1
  done
done
echo "done: $(date)"
