#!/bin/sh
# Run UNICORN's analyzer (GraphChi, crimson-unicorn/analyzer bin/unicorn/main) on
# every base/stream pair in a graphs directory, in parallel, writing one sketch
# file per graph and a per-graph timing log. Shard files GraphChi drops next to
# the base graph are removed after each run.
#
# usage: unicorn_run_analyzer.sh <graphs_dir> <sketch_dir> <parallel> [batch] [decay] [lambda] [chunk_size]
set -eu
GR=$1; OUT=$2; PAR=$3; BATCH=${4:-3000}; DECAY=${5:-500}; LAMBDA=${6:-0.02}; CHUNK=${7:-50}
BIN=/home/jding/Barry/baselines/unicorn/analyzer/bin/unicorn/main
mkdir -p "$OUT"
cd /home/jding/Barry/baselines/unicorn/analyzer   # GraphChi reads conf/graphchi.cnf from cwd
ls "$GR"/base/ | sed 's/^base-//; s/\.txt$//' | sort | \
nice -n 10 xargs -P "$PAR" -I{} sh -c '
  name={}; b="$0/base/base-$name.txt"; s="$0/stream/stream-$name.txt"; sk="$1/sketch-$name.txt"
  [ -s "$sk" ] && exit 0
  rm -f "$sk"
  t0=$(date +%s.%N)
  OMP_NUM_THREADS=2 '"$BIN"' filetype edgelist base "$b" stream "$s" decay '"$DECAY"' lambda '"$LAMBDA"' batch '"$BATCH"' sketch "$sk" chunkify 1 chunk_size '"$CHUNK"' > "$1/log-$name.txt" 2>&1 || echo "FAILED $name" >> "$1/failed.txt"
  t1=$(date +%s.%N)
  rm -rf "$b".* "$0/base/base-$name.txt_"*
  echo "$name $(wc -l < "$s") $(wc -l < "$sk") $(echo "$t1 - $t0" | bc)" >> "$1/timing.tsv"
' "$GR" "$OUT"
