#!/bin/bash
# Durability-cost reruns for the three corpora whose first run logged nothing
# (the queue passed a --wal_dir that did not exist; the bench printed
# "No such file or directory" and measured the no-log path six times).
# Uses the bench's default /tmp, the same device the wiki-talk rows used, and
# refuses to accept a row whose log is empty.
set -u
cd "$(dirname "$0")/.."
BIN=/home/jding/dataset/bin
LOG=results/revision_logs
for c in tc-cadets-e3 tc-theia-e3 lanl-auth; do
    echo "[$(date +%H:%M:%S)] start e18cost_$c"
    build/bench_e18_recovery --dataset $BIN/$c.bin --mode cost --threads 8 \
        --output results/e18_recovery/cost.csv > "$LOG/post_e18cost_$c.log" 2>&1
    rc=$?
    if grep -q "No such file" "$LOG/post_e18cost_$c.log"; then echo "  WAL could not be opened"; rc=99; fi
    echo "[$(date +%H:%M:%S)] end   e18cost_$c rc=$rc  $(grep -c 'wal=   0.00 GB' "$LOG/post_e18cost_$c.log") zero-size log rows"
done
rm -f /tmp/e18_cost.wal
echo "[$(date +%H:%M:%S)] post-queue-2 finished"
