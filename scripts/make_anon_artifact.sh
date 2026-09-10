#!/bin/sh
# Build the double-blind artifact tree for USENIX-style submission.
# Exports HEAD via git archive (no history, no committer identities), strips
# identity-bearing files, then VERIFIES no identity string survived.
set -e
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/barry-anon}
rm -rf "$OUT"; mkdir -p "$OUT"
git archive HEAD | tar -x -C "$OUT"
# identity-bearing content that must not ship anonymously
rm -rf "$OUT/docs/email_to_zephyr.txt" "$OUT/paper" "$OUT/.claude" 2>/dev/null || true
# scrub identity strings from EVERY text file (no extension whitelist -- the
# whitelist approach missed .json and .py on its first run)
grep -rIl "jding\|[Jj]iacheng\|memphis\|[Zz]ephyr" "$OUT" 2>/dev/null | while read -r f; do
    sed -i 's|/home/jding|/home/USER|g; s|xzws\.uom\.memphis\.edu|SERVER|g; s|[Jj]iacheng[ _]*[Dd]ing|AUTHOR|g; s|Jiacheng|AUTHOR|g; s|jiacheng|author|g; s|dingjiachengcn@outlook\.com|author@example.org|g; s|[Uu]niversity of [Mm]emphis|INSTITUTION|g; s|memphis|INSTITUTION|g; s|[Zz]ephyr|COLLABORATOR|g' "$f"
done
echo "=== identity scan (must print nothing) ==="
LEAKS=$(grep -rIn "jding\|[Jj]iacheng\|dingjiacheng\|memphis\|[Zz]ephyr" "$OUT" | grep -cv "Binary file" || true)
grep -rIn "jding\|[Jj]iacheng\|dingjiacheng\|memphis\|[Zz]ephyr" "$OUT" | grep -v "Binary file" | head -10 || true
[ "${LEAKS:-0}" -eq 0 ] && echo "CLEAN" || { echo "FAILED: $LEAKS leaks"; exit 1; }
echo "=== done: $OUT ==="
du -sh "$OUT"
