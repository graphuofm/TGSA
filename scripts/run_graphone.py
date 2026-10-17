#!/usr/bin/env python3
"""E20/E33: GraphOne (FAST'19) as an in-category baseline.

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

Query jobs (docs/E33_SPEC.md), all on the archived adjacency after Make Graph,
all reading the same <query-dir>/<corpus>.queries list every other system
answers, checksummed against <query-dir>/<corpus>.ref when it is present:

  -j 28  q2  windowed neighbours        analytics/window_query.h
  -j 34  q1  window scan                analytics/e33_queries.h
  -j 35  q3  earliest-arrival reach     analytics/e33_queries.h
  -j 36  q4  as-of neighbours           analytics/e33_queries.h

(30-33 are upstream GraphOne streaming jobs, hence 34-36.)

Two CSVs come out: --out keeps the one-row-per-job table this script has
always written (new columns appended), and <out dir>/e33_graphone.csv gets the
spec-shaped rows (system,dataset,phase,...) the matrix is assembled from.

Usage:
  scripts/run_graphone.py --corpora sx-askubuntu,wiki-talk-temporal
                          --out results/e20_graphone/graphone.csv
  scripts/run_graphone.py --corpora wiki-talk-temporal --query-dir results/e33_matrix
                          --out results/e33_matrix/graphone.csv --threads 8
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
DEFAULT_BINDIR = "/home/jding/dataset/bin"

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
    "window_median_us":        re.compile(r"Window Median us = ([0-9.eE+-]+)"),
    "window_p95_us":           re.compile(r"Window P95 us = ([0-9.eE+-]+)"),
    "window_done":             re.compile(r"Window Done = (\d+/\d+)"),
    "scan_sec":                re.compile(r"Scan Query Time = ([0-9.eE+-]+)"),
    "scan_count":              re.compile(r"Scan Count = ([0-9]+)"),
    "scan_checksum":           re.compile(r"Scan Checksum = ([0-9]+)"),
    "reach_sec":               re.compile(r"Reach Query Time = ([0-9.eE+-]+)"),
    "reach_count":             re.compile(r"Reach Count = ([0-9]+)"),
    "reach_checksum":          re.compile(r"Reach Checksum = ([0-9]+)"),
    "reach_median_us":         re.compile(r"Reach Median us = ([0-9.eE+-]+)"),
    "reach_p95_us":            re.compile(r"Reach P95 us = ([0-9.eE+-]+)"),
    "reach_done":              re.compile(r"Reach Done = (\d+/\d+)"),
    "asof_sec":                re.compile(r"Asof Query Time = ([0-9.eE+-]+)"),
    "asof_count":              re.compile(r"Asof Count = ([0-9]+)"),
    "asof_checksum":           re.compile(r"Asof Checksum = ([0-9]+)"),
    "asof_median_us":          re.compile(r"Asof Median us = ([0-9.eE+-]+)"),
    "asof_p95_us":             re.compile(r"Asof P95 us = ([0-9.eE+-]+)"),
    "asof_done":               re.compile(r"Asof Done = (\d+/\d+)"),
}
# Fields kept as text rather than parsed to float (k/N fractions, integers that
# must survive the round trip through the CSV at full 64-bit width).
TEXT_FIELDS = {"window_done", "reach_done", "asof_done",
               "window_delivered", "window_scanned", "window_checksum",
               "scan_count", "scan_checksum", "reach_count", "reach_checksum",
               "asof_count", "asof_checksum"}

# Query jobs: job -> (E33 phase, output-line prefix used in PATTERNS keys).
QUERY_JOBS = {28: ("q2", "window"), 34: ("q1", "scan"), 35: ("q3", "reach"), 36: ("q4", "asof")}
COUNT_KEY = {"window": "window_delivered", "scan": "scan_count",
             "reach": "reach_count", "asof": "asof_count"}

E33_COLS = ["system", "dataset", "phase", "threads", "events", "samples_total",
            "samples_done", "seconds", "rate", "median_us", "p95_us", "count",
            "checksum", "checksum_ok", "rss_MB", "note"]


def convert(corpus, fmt, scratch, limit, bindir):
    """Materialise the corpus in GraphOne's binary layout. Returns (dir, edges, vcount)."""
    outdir = os.path.join(scratch, f"{corpus}.{fmt}")
    cmd = [CONV, "--dataset", os.path.join(bindir, corpus + ".bin"),
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
        if not m:
            out[key] = ""
        elif key in TEXT_FIELDS:
            out[key] = m.group(1)
        else:
            out[key] = float(m.group(1))
    return out, wall, ""


def read_ref(path):
    """<corpus>.ref: one `phase count checksum` line per phase, or {} if absent."""
    ref = {}
    if not path or not os.path.exists(path):
        return ref
    with open(path) as fh:
        for line in fh:
            parts = line.split()
            if len(parts) >= 3:
                ref[parts[0]] = (parts[1], parts[2])
    return ref


def ref_matches(ref, phase, count, checksum):
    """1 if count and checksum both match the ref line, 0 if not, -1 if no ref."""
    if phase not in ref or count == "" or checksum == "":
        return -1
    want = ref[phase]
    return 1 if (str(count) == want[0] and str(checksum) == want[1]) else 0


def check_e19_checksum(e19_csv, corpus, checksum, delivered):
    """Legacy q2 check against Gnomon's E19 CSV, used only when no .ref exists."""
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


def query_count(qfile):
    try:
        return os.path.getsize(qfile) // 24
    except OSError:
        return 0


def parse_done(text, total):
    """'k/N' -> (k, N); falls back to (total, total) when the job printed none."""
    m = re.match(r"(\d+)/(\d+)", str(text or ""))
    if m:
        return int(m.group(1)), int(m.group(2))
    return total, total


def open_append_csv(path, cols):
    """Append to a CSV, reusing an existing header so old files stay well formed.

    Returns (fh, writer). Columns the existing header lacks are dropped with a
    warning rather than silently producing ragged rows.
    """
    existing = None
    if os.path.exists(path) and os.path.getsize(path) > 0:
        with open(path, newline="") as fh:
            existing = next(csv.reader(fh), None)
    fh = open(path, "a", newline="")
    if existing:
        missing = [c for c in cols if c not in existing]
        if missing:
            print(f"[e20] {path}: existing header lacks {missing}; those columns are "
                  f"dropped from appended rows (use a new --out to get them)",
                  file=sys.stderr)
        w = csv.DictWriter(fh, fieldnames=existing, extrasaction="ignore")
    else:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
    return fh, w


def e33_row(corpus, phase, threads, edges, **kw):
    row = {"system": "graphone", "dataset": corpus, "phase": phase, "threads": threads,
           "events": edges, "samples_total": 0, "samples_done": 0, "seconds": 0,
           "rate": 0, "median_us": 0, "p95_us": 0, "count": 0, "checksum": 0,
           "checksum_ok": -1, "rss_MB": 0, "note": "archived adjacency"}
    row.update(kw)
    return row


def e33_rows_for_job(job, corpus, threads, edges, res, note, qtotal, ref):
    """Spec-shaped rows for one GraphOne job run (job 20 -> ingest+compact;
    query jobs -> their phase). A failed run still yields rows, with the note."""
    rows = []
    if job == 20:
        bu = (res or {}).get("batch_update_sec") or 0
        mg = (res or {}).get("make_graph_sec") or 0
        rows.append(e33_row(corpus, "ingest", threads, edges,
                            seconds=round(bu, 6) if bu else 0,
                            rate=round(edges / bu, 3) if bu else 0,
                            checksum_ok=-1,
                            note=note or "Batch Update Time (edge log absorbed)"))
        rows.append(e33_row(corpus, "compact", threads, edges,
                            seconds=round(mg, 6) if mg else 0,
                            checksum_ok=-1,
                            note=note or "Make Graph Time (ingest+archive, same start as ingest)"))
        return rows
    if job not in QUERY_JOBS:
        return rows
    phase, pre = QUERY_JOBS[job]
    total = 1 if phase == "q1" else qtotal
    if not res:
        rows.append(e33_row(corpus, phase, threads, edges, samples_total=total,
                            note=note or "no output"))
        return rows
    sec = res.get(f"{pre}_sec") or 0
    count = res.get(COUNT_KEY[pre], "")
    chk = res.get(f"{pre}_checksum", "")
    if phase == "q1":
        done, total = 1, 1
        med = p95 = round(sec * 1e6, 3) if sec else 0
    else:
        done, total = parse_done(res.get(f"{pre}_done"), total)
        med = res.get(f"{pre}_median_us") or 0
        p95 = res.get(f"{pre}_p95_us") or 0
    rows.append(e33_row(corpus, phase, threads, edges, samples_total=total,
                        samples_done=done, seconds=round(sec, 6) if sec else 0,
                        rate=round(done / sec, 3) if sec else 0,
                        median_us=med, p95_us=p95,
                        count=count if count != "" else 0,
                        checksum=chk if chk != "" else 0,
                        checksum_ok=ref_matches(ref, phase, count, chk),
                        note=note or ("archived adjacency" if done == total
                                      else f"archived adjacency; budget cut {done}/{total}")))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpora", required=True,
                    help="comma-separated corpus basenames under --bindir")
    ap.add_argument("--out", required=True)
    ap.add_argument("--bindir", default=DEFAULT_BINDIR,
                    help="directory holding <corpus>.bin (default %(default)s)")
    ap.add_argument("--scratch", default="/home/jding/dataset/graphone")
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=3600,
                    help="per-job wall clock cap in seconds (ingest + queries)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the converted corpora (they are large)")
    ap.add_argument("--query-dir", default="",
                    help="directory holding <corpus>.queries (and optionally <corpus>.ref); "
                         "enables the query jobs (-j 28, 34, 35, 36)")
    ap.add_argument("--jobs", default="",
                    help="comma-separated weight64 job list to run instead of the default "
                         "(20 plus the query jobs when the query file exists)")
    ap.add_argument("--no-plain32", action="store_true",
                    help="skip the 8-byte structural -j 0 run on graphone32")
    ap.add_argument("--e19-csv", default="results/e19_livegraph/e19.csv",
                    help="legacy q2 checksum source, used only when no .ref exists")
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
            "window_ns_per_delivered", "window_checksum_matches",
            "window_median_us", "window_p95_us", "window_done",
            "scan_sec", "scan_count", "scan_checksum", "scan_checksum_matches",
            "reach_sec", "reach_count", "reach_checksum", "reach_median_us",
            "reach_p95_us", "reach_done", "reach_checksum_matches",
            "asof_sec", "asof_count", "asof_checksum", "asof_median_us",
            "asof_p95_us", "asof_done", "asof_checksum_matches", "note"]
    fh, w = open_append_csv(args.out, cols)
    e33_path = os.path.join(os.path.dirname(os.path.abspath(args.out)), "e33_graphone.csv")
    e33_fh, e33_w = open_append_csv(e33_path, E33_COLS)

    odir = os.path.join(args.scratch, "_out")
    for corpus in args.corpora.split(","):
        corpus = corpus.strip()
        if not corpus:
            continue
        # -j 20 is the weighted edge type: 24 bytes, timestamp in the weight
        # slot. -j 0 is the 8-byte structural edge, kept so the reader can see
        # what the extra 16 bytes of time cost GraphOne.
        # The query jobs need the query list the tgsr harness wrote, so they
        # only run when that file exists.
        qfile = os.path.join(args.query_dir, corpus + ".queries") if args.query_dir else ""
        have_q = bool(qfile) and os.path.exists(qfile)
        ref = read_ref(os.path.join(args.query_dir, corpus + ".ref")) if args.query_dir else {}
        qtotal = query_count(qfile) if have_q else 0
        if args.jobs:
            w64_jobs = [int(j) for j in args.jobs.split(",") if j.strip()]
        else:
            w64_jobs = [20] + (sorted(QUERY_JOBS) if have_q else [])
        configs = [("weight64", G64, w64_jobs, 24)]
        if not args.no_plain32:
            configs.append(("plain32", G32, [0], 8))
        for fmt, binary, jobs, bpe in configs:
            try:
                idir, edges, vcount = convert(corpus, fmt, args.scratch, args.limit, args.bindir)
            except RuntimeError as e:
                print(f"[e20] {corpus} {fmt}: {e}", file=sys.stderr)
                continue
            for job in jobs:
                env_extra = {"GNOMON_QUERY_FILE": qfile} if job in QUERY_JOBS else None
                res, wall, note = run_graphone(binary, idir, odir, vcount, job,
                                               args.threads, args.timeout, env_extra)
                row = {"system": "graphone", "dataset": corpus, "job": job,
                       "config": fmt, "edges": edges, "vertices": vcount,
                       "threads": args.threads, "wall_sec": round(wall, 3),
                       "bytes_per_edge": bpe, "note": note}
                if res:
                    row.update({k: v for k, v in res.items()})
                    if job in QUERY_JOBS:
                        phase, pre = QUERY_JOBS[job]
                        ok = ref_matches(ref, phase, res.get(COUNT_KEY[pre], ""),
                                         res.get(f"{pre}_checksum", ""))
                        if ok == -1 and job == 28:
                            row["window_checksum_matches"] = check_e19_checksum(
                                os.path.join(REPO, args.e19_csv), corpus,
                                res.get("window_checksum", ""),
                                res.get("window_delivered", ""))
                        else:
                            row[f"{pre}_checksum_matches"] = {1: "yes", 0: "no"}.get(ok, "")
                    bu = res.get("batch_update_sec") or 0
                    mg = res.get("make_graph_sec") or 0
                    row["batch_update_Meps"] = round(edges / bu / 1e6, 3) if bu else ""
                    row["make_graph_Meps"] = round(edges / mg / 1e6, 3) if mg else ""
                w.writerow(row)
                fh.flush()
                if fmt == "weight64":
                    for r in e33_rows_for_job(job, corpus, args.threads, edges, res,
                                              note, qtotal, ref):
                        e33_w.writerow(r)
                    e33_fh.flush()
                extra = ""
                if job in QUERY_JOBS and res:
                    phase, pre = QUERY_JOBS[job]
                    extra = (f"{phase}: {res.get(f'{pre}_sec','-')}s "
                             f"count={res.get(COUNT_KEY[pre],'-')} "
                             f"chk={res.get(f'{pre}_checksum','-')} "
                             f"match={row.get(f'{pre}_checksum_matches','')} "
                             f"done={res.get(f'{pre}_done','-')}")
                print(f"[e20] {corpus:<24} {fmt:<9} j{job:<3} "
                      f"batch={row.get('batch_update_sec','-')}s "
                      f"durable={row.get('durable_sec','-')}s "
                      f"make={row.get('make_graph_sec','-')}s "
                      f"({row.get('make_graph_Meps','-')} Meps) {extra} {note}",
                      file=sys.stderr)
            if not args.keep:
                shutil.rmtree(idir, ignore_errors=True)

    shutil.rmtree(odir, ignore_errors=True)
    fh.close()
    e33_fh.close()


if __name__ == "__main__":
    main()
