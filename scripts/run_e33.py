#!/usr/bin/env python3
"""E33 queue: the aligned comparison matrix, one process per (system, corpus).

Runs bench_e33_matrix for tgsr first on each corpus (it writes the shared
query list and the checksum reference), then every other in-process system,
one process at a time so a DNF or an OOM takes down only its own cell.
stinger and sortledton go through their existing harnesses (bench_e31_stinger,
bench_e32_sortledton); their ingest row is translated into e33.csv and their
q1..q4/bfs cells are written as n/a with the spec's reason.

Usage:
  scripts/run_e33.py --corpora wiki,cadets,theia,lanl \\
                     --systems tgsr,livegraph,rocksdb,duckdb,stinger,sortledton \\
                     [--phases ingest,compact,q1,q2,q3,q4,bfs] [--outdir results/e33_matrix]
                     [--force] [--dry_run]
"""

import argparse
import csv
import glob
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINDIR = "/home/jding/dataset/bin"
CORPORA = {            # short name -> .bin basename, in docs/E33_SPEC.md
    "lanl":   "lanl-auth",
    "theia":  "tc-theia-e3",
    "cadets": "tc-cadets-e3",
    "wiki":   "wiki-talk-temporal",
}
MATRIX_SYSTEMS = ["tgsr", "livegraph", "rocksdb", "duckdb"]
PAIR_SYSTEMS = {       # one edge per (src,dst): ingest only, via their own harness
    "stinger":    "bench_e31_stinger",
    "sortledton": "bench_e32_sortledton",
}
COLS = ["system", "dataset", "phase", "threads", "events", "samples_total",
        "samples_done", "seconds", "rate", "median_us", "p95_us", "count",
        "checksum", "checksum_ok", "rss_MB", "note"]
PAIR_NA = "n/a: one edge per src-dst pair; the event multiset cannot round-trip"


def existing_pairs(csv_path):
    pairs = set()
    if not os.path.exists(csv_path):
        return pairs
    with open(csv_path, newline="") as fh:
        for row in csv.DictReader(fh):
            pairs.add((row["system"], row["dataset"]))
    return pairs


def drop_pair(csv_path, system, dataset):
    """--force: remove the pair's old rows so the file never holds two answers."""
    if not os.path.exists(csv_path):
        return
    with open(csv_path, newline="") as fh:
        rows = [r for r in csv.DictReader(fh)
                if not (r["system"] == system and r["dataset"] == dataset)]
    with open(csv_path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=COLS)
        w.writeheader()
        w.writerows(rows)


def append_rows(csv_path, rows):
    new = not os.path.exists(csv_path) or os.path.getsize(csv_path) == 0
    with open(csv_path, "a", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=COLS)
        if new:
            w.writeheader()
        for r in rows:
            r = dict(r)
            r["note"] = str(r.get("note", "")).replace(",", ";")   # keep rows unquoted, like the harness
            w.writerow({c: r.get(c, "") for c in COLS})


def blank_row(system, dataset, phase, threads, events, note, **kw):
    row = {"system": system, "dataset": dataset, "phase": phase, "threads": threads,
           "events": events, "samples_total": 0, "samples_done": 0, "seconds": 0,
           "rate": 0, "median_us": 0, "p95_us": 0, "count": 0, "checksum": 0,
           "checksum_ok": -1, "rss_MB": 0, "note": note}
    row.update(kw)
    return row


def run_logged(cmd, log_path, dry):
    print(f"[e33] $ {' '.join(cmd)}", flush=True)
    if dry:
        return 0, 0.0
    t0 = time.time()
    with open(log_path, "w") as log:
        log.write("$ " + " ".join(cmd) + "\n")
        log.flush()
        rc = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT, cwd=REPO)
    return rc, time.time() - t0


def run_matrix(system, corpus, a, csv_path, logdir):
    ds = CORPORA[corpus]
    cmd = ["nice", "-n", "0", os.path.join(REPO, "build", "bench_e33_matrix"),
           "--dataset", os.path.join(BINDIR, ds + ".bin"),
           "--system", system, "--phases", a.phases,
           "--batch", str(a.batch), "--samples", str(a.samples),
           "--threads", str(a.threads),
           "--ingest_budget_sec", str(a.ingest_budget_sec),
           "--phase_budget_sec", str(a.phase_budget_sec),
           "--rss_guard_gb", str(a.rss_guard_gb),
           "--outdir", a.outdir]
    if a.limit:
        cmd += ["--limit", str(a.limit)]
    rc, wall = run_logged(cmd, os.path.join(logdir, f"{system}_{ds}.log"), a.dry_run)
    if system == "rocksdb" and not a.dry_run:
        # A hard stop (exit 4) or a kill leaves the on-disk DB behind.
        for d in glob.glob(f"/home/jding/dataset/e33_rocksdb_{ds}*"):
            shutil.rmtree(d, ignore_errors=True)
    if rc not in (0, 4) and not a.dry_run:
        # The harness writes its own DNF rows; a crash does not, so the cell
        # must still say what happened.
        if (system, ds) not in existing_pairs(csv_path):
            append_rows(csv_path, [blank_row(system, ds, "ingest", a.threads, 0,
                                             f"crash: exit {rc} after {wall:.0f} s")])
    return rc, wall


def run_pair_system(system, corpus, a, csv_path, logdir):
    """stinger / sortledton through bench_e31 / bench_e32, translated into e33 rows."""
    ds = CORPORA[corpus]
    binary = os.path.join(REPO, "build", PAIR_SYSTEMS[system])
    if not os.path.exists(binary):
        print(f"[e33] {binary} not built; skipping {system}", file=sys.stderr)
        return 2, 0.0
    side_csv = os.path.join(a.outdir, "e31e32.csv")
    cmd = ["nice", "-n", "0", "timeout", str(int(a.ingest_budget_sec)), binary,
           "--dataset", os.path.join(BINDIR, ds + ".bin"),
           "--threads", str(a.threads), "--output", side_csv]
    rc, wall = run_logged(cmd, os.path.join(logdir, f"{system}_{ds}.log"), a.dry_run)
    if a.dry_run:
        return rc, wall
    rows = []
    hit = None
    if os.path.exists(side_csv):
        with open(side_csv, newline="") as fh:
            for row in csv.DictReader(fh):
                if row["system"] == system and row["dataset"] == ds:
                    hit = row           # the last row for the pair is the newest
    if rc == 124:
        rows.append(blank_row(system, ds, "ingest", a.threads, 0,
                              "DNF", seconds=a.ingest_budget_sec))
    elif hit is None:
        rows.append(blank_row(system, ds, "ingest", a.threads, 0,
                              f"crash: exit {rc} after {wall:.0f} s"))
    else:
        events = int(hit["events"])
        sec = float(hit["ingest_sec"])
        note = f"one edge per pair; unique_pairs={hit['unique_pairs']}; probe_ok={hit['probe_ok']}"
        rows.append(blank_row(system, ds, "ingest", hit["threads"], events, note,
                              seconds=sec, rate=events / sec if sec else 0,
                              rss_MB=hit["rss_MB"]))
    events = rows[0]["events"]
    rows.append(blank_row(system, ds, "compact", a.threads, events,
                          "n/a: no separate compaction step"))
    for ph in ("q1", "q2", "q3", "q4"):
        rows.append(blank_row(system, ds, ph, a.threads, events, PAIR_NA))
    rows.append(blank_row(system, ds, "bfs", a.threads, events,
                          "n/a: harness measures ingest only"))
    append_rows(csv_path, rows)
    return rc, wall


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpora", default="wiki,cadets,theia,lanl",
                    help="comma list of " + ",".join(CORPORA))
    ap.add_argument("--systems", default=",".join(MATRIX_SYSTEMS),
                    help="comma list of " + ",".join(MATRIX_SYSTEMS + list(PAIR_SYSTEMS)))
    ap.add_argument("--phases", default="ingest,compact,q1,q2,q3,q4,bfs")
    ap.add_argument("--outdir", default=os.path.join(REPO, "results", "e33_matrix"))
    ap.add_argument("--samples", type=int, default=1000)
    ap.add_argument("--batch", type=int, default=65536)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--ingest_budget_sec", type=float, default=5400)
    ap.add_argument("--phase_budget_sec", type=float, default=1800)
    ap.add_argument("--rss_guard_gb", type=float, default=150)
    ap.add_argument("--force", action="store_true",
                    help="rerun pairs that already have rows (their old rows are removed)")
    ap.add_argument("--dry_run", action="store_true")
    a = ap.parse_args()

    corpora = [c.strip() for c in a.corpora.split(",") if c.strip()]
    for c in corpora:
        if c not in CORPORA:
            sys.exit(f"unknown corpus '{c}' (choose from {','.join(CORPORA)})")
    systems = [s.strip() for s in a.systems.split(",") if s.strip()]
    for s in systems:
        if s not in MATRIX_SYSTEMS and s not in PAIR_SYSTEMS:
            sys.exit(f"unknown system '{s}'")
    # tgsr first: it writes <ds>.queries and <ds>.ref that everyone else reads.
    systems.sort(key=lambda s: 0 if s == "tgsr" else 1)

    os.makedirs(a.outdir, exist_ok=True)
    logdir = os.path.join(a.outdir, "logs")
    os.makedirs(logdir, exist_ok=True)
    csv_path = os.path.join(a.outdir, "e33.csv")

    for corpus in corpora:
        ds = CORPORA[corpus]
        if not os.path.exists(os.path.join(BINDIR, ds + ".bin")):
            print(f"[e33] missing {ds}.bin; skipping corpus", file=sys.stderr)
            continue
        for system in systems:
            if (system, ds) in existing_pairs(csv_path):
                if not a.force:
                    print(f"[e33] {system:<10} {ds:<20} rows exist; skipping (--force to rerun)")
                    continue
                drop_pair(csv_path, system, ds)
            runner = run_pair_system if system in PAIR_SYSTEMS else run_matrix
            rc, wall = runner(system, corpus, a, csv_path, logdir)
            status = {0: "ok", 2: "not built", 4: "hard stop (budget/rss)", 124: "timeout"}.get(rc, f"exit {rc}")
            print(f"[e33] {system:<10} {ds:<20} {status:<22} {wall:8.0f} s", flush=True)


if __name__ == "__main__":
    main()
