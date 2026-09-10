#!/bin/sh
# Reclaim disk after experiments. Results and method are the product; the bulky
# artifacts a run happens to generate are not, and every one of them is
# reproducible from the code plus the raw archive.
#
#   scripts/cleanup.sh          # show what would be freed
#   scripts/cleanup.sh --apply  # actually delete
#
# NEVER touched: results/**/*.csv, results/**/NOTES.md, paper/, src/, tools/,
# benchmark/, tests/, data/*.md -- all small, all precious.

APPLY=0
[ "$1" = "--apply" ] && APPLY=1
ROOT=/home/jding/Barry
BIN=/home/jding/dataset/bin
RAW=/home/jding/dataset/lanl

total=0
report() {
    [ -e "$1" ] || return 0
    sz=$(du -sm "$1" 2>/dev/null | cut -f1)
    [ -z "$sz" ] && return 0
    total=$((total + sz))
    printf '%8s MB  %s\n' "$sz" "$1"
    [ $APPLY -eq 1 ] && rm -rf "$1"
    return 0
}

echo "=== derived graphs (regenerate: tools/lanl_to_bin.cpp, tools/tc_cdm_to_bin.cpp) ==="
for f in "$BIN"/lanl-*.bin "$BIN"/tc-*.bin; do report "$f"; done

echo
echo "=== raw archives (re-downloadable; keep redteam.txt.gz, it is 5 KB and ground truth) ==="
for f in "$RAW"/auth.txt.gz "$RAW"/proc.txt.gz "$RAW"/flows.txt.gz "$RAW"/dns.txt.gz; do report "$f"; done

echo
echo "=== per-query detail dumps (superseded once summarised into NOTES.md) ==="
for f in "$ROOT"/results/*/*_detail.csv; do report "$f"; done

echo
echo "=== stale logs ==="
for f in "$ROOT"/logs/overnight_* "$ROOT"/logs/*.log.init "$ROOT"/logs/*.log.deps; do report "$f"; done

echo
if [ $APPLY -eq 1 ]; then
    echo "freed ~${total} MB"
else
    echo "would free ~${total} MB   (re-run with --apply)"
fi
df -h /home/jding | tail -1
