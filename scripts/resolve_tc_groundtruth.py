#!/usr/bin/env python3
"""Resolve a DARPA TC ground-truth TSV to node ids for bench_e14_tc.

Input  (data/tc_<corpus>_groundtruth.tsv):  attack  kind  match  mode  role
Output (<prefix>.seeds.tsv):                 attack  kind  match  node_id  node_kind  label

Resolution rule (reverse-engineered from tc-cadets-e3.seeds.tsv, which this
script reproduces byte-for-byte):
  * rows are processed in file order; '#' lines and the header are skipped;
  * a node matches a row when its label (third column of <prefix>.nodes.tsv)
    contains the match string (mode=substr) or equals it (mode=exact).  The
    row's kind column is documentary: it is NOT used as a filter (CADETS'
    "process nginx" resolves to file nodes, because CADETS carries no process
    labels);
  * matches are emitted in ascending node id; at most --max-per-row of them
    (default 6: CADETS' "nginx" row has 31 substring matches and the seeds
    file keeps the first six);
  * a row with no match is reported on stderr and emits nothing; the same
    node may appear under several rows/attacks (shared C2 infrastructure).

Usage:
  resolve_tc_groundtruth.py --gt data/tc_x_groundtruth.tsv \
      --nodes /path/x.nodes.tsv --out /path/x.seeds.tsv [--max-per-row 6]
"""
import argparse
import sys


def load_rows(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "attack":
                continue
            if len(parts) < 4:
                sys.exit(f"malformed ground-truth row: {line!r}")
            rows.append((parts[0], parts[1], parts[2], parts[3]))
    return rows


def resolve(rows, nodes_path, cap):
    hits = [[] for _ in rows]
    substr = [(i, m) for i, (_, _, m, mode) in enumerate(rows) if mode == "substr"]
    exact = {}
    for i, (_, _, m, mode) in enumerate(rows):
        if mode == "exact":
            exact.setdefault(m, []).append(i)
        elif mode != "substr":
            sys.exit(f"unknown mode {mode!r} in row {rows[i]}")
    with open(nodes_path, encoding="utf-8", errors="surrogateescape") as f:
        f.readline()
        for line in f:
            parts = line.rstrip("\n").split("\t", 2)
            label = parts[2] if len(parts) > 2 else ""
            if not label:
                continue
            for i in exact.get(label, ()):
                if len(hits[i]) < cap:
                    hits[i].append((parts[0], parts[1], label))
            for i, m in substr:
                if m in label and len(hits[i]) < cap:
                    hits[i].append((parts[0], parts[1], label))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gt", required=True)
    ap.add_argument("--nodes", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-per-row", type=int, default=6)
    a = ap.parse_args()

    rows = load_rows(a.gt)
    hits = resolve(rows, a.nodes, a.max_per_row)
    n_ids = 0
    with open(a.out, "w", encoding="utf-8", errors="surrogateescape") as out:
        out.write("attack\tkind\tmatch\tnode_id\tnode_kind\tlabel\n")
        for (attack, kind, match, mode), h in zip(rows, hits):
            if not h:
                print(f"UNRESOLVED attack {attack} {kind} {match!r} ({mode})", file=sys.stderr)
                continue
            for nid, nkind, label in h:
                out.write(f"{attack}\t{kind}\t{match}\t{nid}\t{nkind}\t{label}\n")
                n_ids += 1
    per_attack = {}
    for (attack, _, _, _), h in zip(rows, hits):
        r, i = per_attack.get(attack, (0, 0))
        per_attack[attack] = (r + (1 if h else 0), i + len(h))
    for attack, (r, i) in sorted(per_attack.items()):
        total = sum(1 for row in rows if row[0] == attack)
        print(f"attack {attack}: {r}/{total} rows resolved -> {i} node ids", file=sys.stderr)
    print(f"wrote {a.out}: {n_ids} seed rows", file=sys.stderr)


if __name__ == "__main__":
    main()
