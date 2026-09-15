#!/usr/bin/env python3
"""E20: GraphOne (FAST'19) as an in-category baseline.

GraphOne is the nearest prior work at this venue and the only one of our
baselines that was designed for the same thing we were: a store that absorbs a
continuous edge stream while analytics run against it. It is also the one whose
numbers are least comparable by accident, so this script pins down what is
being compared.

Three GraphOne timings come out of a run and they mean different things:

  Batch Update Time   edges appended to the circular edge log. This is a memcpy
                      into memory; nothing is indexed and nothing is on disk.
  Durable Graph Time  the log writer has write()n those edges. GraphOne does
                      not fsync (src/sgraph.h leaves the call commented out),
                      so this is "reached the page cache", which matches its own
                      statement that an unplanned shutdown may lose recent data
                      and recovery depends on an upstream backup such as Kafka.
  Make Graph Time     archiving into the adjacency store has caught up, i.e.
                      the point where a vertex-centric read sees the edges.

Against Gnomon those line up as: Batch Update vs our ingest without a log,
Durable Graph vs our write_only mode (E18 cost), and Make Graph vs our ingest
plus finalize(). The comparison is only fair in the -j 20 configuration, which
uses the weighted edge type so the timestamp can actually travel with the edge;
GraphOne's headline -j 0 configuration is an 8-byte structural edge with no
room for time at all.

Usage:
  scripts/run_graphone.py --corpora sx-askubuntu,wiki-talk-temporal
                          --out results/e20_graphone/graphone.csv
"""

import argparse
import csv
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
G64 = os.path.join(REPO, "baselines", "graphone", "build", "graphone64")
G32 = os.path.join(REPO, "baselines", "graphone", "build", "graphone32")
CONV = os.path.join(REPO, "build", "barry_bin_to_graphone")
BINDIR = os.path.join(REPO, "data", "bin")

PATTERNS = {
    "batch_update_sec": re.compile(r"Batch Update Time = ([0-9.eE+-]+)"),
    "durable_sec":      re.compile(r"Durable Graph Time = ([0-9.eE+-]+)"),
    "make_graph_sec":   re.compile(r"Make Graph Time = ([0-9.eE+-]+)"),
    "read_alloc_sec":   re.compile(r"Read/alloc time = ([0-9.eE+-]+)"),
    "batching_sec":     re.compile(r"Batching time = ([0-9.eE+-]+)"),
    "bfs_sec":          re.compile(r"BFS root = \d+Time = ([0-9.eE+-]+)"),
    "window_sec":              re.compile(r"Window Query Time = ([0-9.eE+-]+)"),
    "window_delivered":        re.compile(r"Window Delivered = ([0-9]+)"),
    "window_scanned":          re.compile(r"Window Scanned = ([0-9]+)"),
    "window_checksum":         re.compile(r"Window Checksum = ([0-9]+)"),
    "window_ns_per_delivered": re.compile(r"Window ns per delivered edge = ([0-9.eE+-]+)"),
}


def convert(corpus, fmt, scratch, limit):
    """Materialise the corpus in GraphOne's binary layout. Returns (dir, edges, vcount)."""
    outdir = os.path.join(scratch, f"{corpus}.{fmt}")
    cmd = [CONV, "--dataset", os.path.join(BINDIR, corpus + ".bin"),
           "--outdir", outdir, "--format", fmt]
    if limit:
        cmd += ["--limit", str(limit)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"convert failed for {corpus}: {r.stderr.strip()}")
    kv = dict(tok.split("=", 1) for tok in r.stdout.strip().split())
    return outdir, int(kv["edges"]), int(kv["vcount"])


def run_graphone(binary, idir, odir, vcount, job, threads, timeout, env_extra=None):
    if os.path.isdir(odir):
        shutil.rmtree(odir)
    os.makedirs(odir, exist_ok=True)
    cmd = [binary, "-i", idir + "/", "-o", odir + "/", "-v", str(vcount),
           "-j", str(job), "-s", "1", "-d", "1", "-t", str(threads)]
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None, time.time() - t0, "timeout"
    wall = time.time() - t0
    if r.returncode != 0:
        return None, wall, f"exit {r.returncode}: {r.stderr.strip()[:300]}"
    out = {}
    for key, pat in PATTERNS.items():
        m = pat.search(r.stdout)
        out[key] = float(m.group(1)) if m else ""
    return out, wall, ""


def check_checksum(e19_csv, corpus, checksum, delivered):
    """Compare GraphOne's delivered set against Gnomon's, via the shared checksum.

    Returns "yes"/"no"/"" so a faster number that answered a different question
    is visible in the table rather than silently accepted.
    """
    if not os.path.exists(e19_csv) or not checksum:
        return ""
    try:
        with open(e19_csv, newline="") as fh:
            for row in csv.DictReader(fh):
                if row["dataset"] == corpus and row["system"] == "gnomon" \
                        and row["mode"] == "compacted":
                    same = (str(int(float(row["checksum"]))) == str(int(float(checksum)))
                            and str(int(float(row["delivered_edges"])))
                                == str(int(float(delivered))))
                    return "yes" if same else "no"
    except (KeyError, ValueError):
        return ""
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpora", required=True,
                    help="comma-separated corpus basenames under data/bin")
    ap.add_argument("--out", required=True)
    ap.add_argument("--scratch", default="/home/jding/dataset/graphone")
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=3600)
    ap.add_argument("--keep", action="store_true",
                    help="keep the converted corpora (they are large)")
    ap.add_argument("--query-dir", default="",
                    help="directory holding <corpus>.queries written by bench_e19; "
                         "enables the windowed-query job (-j 28)")
    ap.add_argument("--e19-csv", default="results/e19_livegraph/e19.csv",
                    help="used only to check GraphOne's checksum against Gnomon's")
    args = ap.parse_args()

    for path in (G64, CONV):
        if not os.path.exists(path):
            sys.exit(f"missing {path}")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    os.makedirs(args.scratch, exist_ok=True)
    cols = ["system", "dataset", "job", "config", "edges", "vertices", "threads",
            "read_alloc_sec", "batching_sec", "batch_update_sec", "durable_sec",
            "make_graph_sec", "bfs_sec", "wall_sec",
            "batch_update_Meps", "make_graph_Meps", "bytes_per_edge",
            "window_sec", "window_delivered", "window_scanned", "window_checksum",
            "window_ns_per_delivered", "window_checksum_matches", "note"]
    new = not os.path.exists(args.out) or os.path.getsize(args.out) == 0
    fh = open(args.out, "a", newline="")
    w = csv.DictWriter(fh, fieldnames=cols)
    if new:
        w.writeheader()

    odir = os.path.join(args.scratch, "_out")
    for corpus in args.corpora.split(","):
        corpus = corpus.strip()
        if not corpus:
            continue
        # -j 20 is the weighted edge type: 24 bytes, timestamp in the weight
        # slot. -j 0 is the 8-byte structural edge, kept so the reader can see
        # what the extra 16 bytes of time cost GraphOne.
        # -j 28 is the windowed-query job added for this comparison; it needs
        # the query list Gnomon wrote, so it only runs when that file exists.
        qfile = os.path.join(args.query_dir, corpus + ".queries") if args.query_dir else ""
        w64_jobs = [20] + ([28] if qfile and os.path.exists(qfile) else [])
        for fmt, binary, jobs, bpe in (("weight64", G64, w64_jobs, 24),
                                       ("plain32",  G32, [0],  8)):
            try:
                idir, edges, vcount = convert(corpus, fmt, args.scratch, args.limit)
            except RuntimeError as e:
                print(f"[e20] {corpus} {fmt}: {e}", file=sys.stderr)
                continue
            for job in jobs:
                env_extra = {"GNOMON_QUERY_FILE": qfile} if job == 28 else None
                res, wall, note = run_graphone(binary, idir, odir, vcount, job,
                                               args.threads, args.timeout, env_extra)
                row = {"system": "graphone", "dataset": corpus, "job": job,
                       "config": fmt, "edges": edges, "vertices": vcount,
                       "threads": args.threads, "wall_sec": round(wall, 3),
                       "bytes_per_edge": bpe, "note": note}
                if res:
                    row.update({k: v for k, v in res.items()})
                    if job == 28:
                        row["window_checksum_matches"] = check_checksum(
                            os.path.join(REPO, args.e19_csv), corpus,
                            res.get("window_checksum", ""),
                            res.get("window_delivered", ""))
                    bu = res.get("batch_update_sec") or 0
                    mg = res.get("make_graph_sec") or 0
                    row["batch_update_Meps"] = round(edges / bu / 1e6, 3) if bu else ""
                    row["make_graph_Meps"] = round(edges / mg / 1e6, 3) if mg else ""
                w.writerow(row)
                fh.flush()
                print(f"[e20] {corpus:<24} {fmt:<9} j{job:<3} "
                      f"batch={row.get('batch_update_sec','-')}s "
                      f"durable={row.get('durable_sec','-')}s "
                      f"make={row.get('make_graph_sec','-')}s "
                      f"({row.get('make_graph_Meps','-')} Meps) {note}",
                      file=sys.stderr)
            if not args.keep:
                shutil.rmtree(idir, ignore_errors=True)

    shutil.rmtree(odir, ignore_errors=True)
    fh.close()


if __name__ == "__main__":
    main()
