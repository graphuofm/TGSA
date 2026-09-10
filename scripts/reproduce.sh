#!/bin/sh
# One-command reproduction of the provenance experiments (E7-E13).
# Prereqs: datasets converted to .bin (tools/lanl_to_bin, tools/tc_cdm_to_bin;
# see data/SECURITY_DATASETS.md), DuckDB C API at ~/opt/duckdb for E9/E10.
set -e
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j8 >/dev/null
cd build && ctest --output-on-failure && cd ..
BIN=${BARRY_DATA:-data/bin}; OUT=results; T=${THREADS:-8}
DS_SMALL="sx-mathoverflow"; DS_MID="wiki-talk-temporal"
LANL="$BIN/lanl-auth"   # prefix: .bin/.nodes.tsv/.redteam.tsv

for q in p1 p2 p2d; do
  ./build/bench_e7_provenance --dataset $BIN/$DS_MID.bin --query $q \
      --num_samples 100 --window_frac 0.01 --threads $T --trial 0 \
      --output $OUT/e7_provenance/e7.csv; done
[ -f "$LANL.bin" ] && for cfg in "full 0" "cpr 60" "cpr 3600" "cpr 86400" "random 50" "random 90"; do
  set -- $cfg
  ./build/bench_e8_attack --graph $LANL --policy $1 --param $2 --classes 1 \
      --windows 0.0001,0.001,0.01,0.05,0.2,1.0 --threads 16 --trial 0 \
      --output $OUT/e8_attack/e8.csv; done
for q in filter traverse; do
  ./build/bench_e9_columnar --dataset $BIN/$DS_MID.bin --query $q \
      --num_samples 30 --window_frac 0.1 --hops 64 --duck_index 1 \
      --threads $T --trial 0 --output $OUT/e9_columnar/e9.csv; done
[ -f "$LANL.bin" ] && ./build/bench_e11_sweep --graph $LANL \
      --alerts $LANL.redteam.tsv --window_frac 0.01 --threads 16 --trial 0 \
      --output $OUT/e11_sweep/e11.csv
[ -f "$LANL.bin" ] && ./build/bench_e12_staleness --dataset $LANL.bin \
      --redteam $LANL.redteam.tsv --split_frac 0.99 --num_queries 100 \
      --window_frac 0.01 --batch 200000 --threads 16 --trial 0 \
      --output $OUT/e12_staleness/e12.csv
for wf in 0.005 0.05 0.3; do
  ./build/bench_e13_storage_read --dataset $BIN/$DS_SMALL.bin --algo all \
      --window_frac $wf --trials 3 --threads $T --output $OUT/e13_storage/e13.csv; done
./scripts/measure_event_rate.sh /tmp/evrate.$$
echo "REPRODUCTION COMPLETE — results under $OUT/, protocols in $OUT/*/NOTES.md"
