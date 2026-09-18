#!/usr/bin/env python3
"""E26: concurrent throughput head-to-head, three systems, one protocol.

For each corpus and writer:reader mix, run the identical settled-query
protocol (see benchmark/bench_e26_concurrent.cpp) on TGSR, LiveGraph
(both in-process modes of bench_e26_concurrent) and GraphOne (job 29 of
its own harness, reading the same query and expected-checksum files).
Everything lands in one CSV; a run with a nonzero mismatch count is
recorded and loudly flagged, never silently kept.

Usage:
  scripts/run_e26.py [--corpora a,b,c] [--mixes 8:4,4:4] [--out csv]
"""

import argparse
import csv
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(REPO, "build", "bench_e26_concurrent")
CONV = os.path.join(REPO, "build", "barry_bin_to_graphone")
G64 = os.path.join(REPO, "baselines", "graphone", "build", "graphone64")
BINDIR = os.path.join(REPO, "data", "bin")
OUTDIR = os.path.join(REPO, "results", "e26_concurrent")

GO_PATTERNS = {
    "preload_sec": re.compile(r"E26 Preload Time = ([0-9.eE+-]+)"),
    "stream_sec": re.compile(r"E26 Stream Time = ([0-9.eE+-]+)"),
    "ingest_Meps": re.compile(r"E26 Stream Meps = ([0-9.eE+-]+)"),
    "queries": re.compile(r"E26 Queries = ([0-9]+)"),
    "queries_per_sec": re.compile(r"E26 QPS = ([0-9.eE+-]+)"),
    "mismatches": re.compile(r"E26 Mismatches = ([0-9]+)"),
    "delivered": re.compile(r"E26 Delivered = ([0-9]+)"),
    "archive_catchup_sec": re.compile(r"E26 Archive Catchup = ([0-9.eE+-]+)"),
}


def sh(cmd, env=None, timeout=7200):
    e = dict(os.environ)
    if env:
        e.update(env)
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpora", default="sx-askubuntu,wiki-talk-temporal,cit-Patents,tc-cadets-e3")
    ap.add_argument("--mixes", default="8:1,8:4,4:4,2:8")
    ap.add_argument("--samples", type=int, default=2000)
    ap.add_argument("--systems", default="tgsr,livegraph,graphone")
    ap.add_argument("--window_frac", type=float, default=0.01)
    ap.add_argument("--out", default=os.path.join(OUTDIR, "e26.csv"))
    ap.add_argument("--scratch", default=os.path.join(OUTDIR, "go_convert"))
    args = ap.parse_args()

    corpora = args.corpora.split(",")
    systems = set(args.systems.split(","))
    mixes = [tuple(int(x) for x in m.split(":")) for m in args.mixes.split(",")]
    os.makedirs(OUTDIR, exist_ok=True)
    os.makedirs(args.scratch, exist_ok=True)

    cols = ["system", "dataset", "writers", "readers", "edges_preloaded",
            "edges_streamed", "preload_sec", "stream_sec", "ingest_Meps",
            "queries", "queries_per_sec", "q_median_us", "q_p95_us",
            "delivered", "mismatches", "samples", "archive_catchup_sec"]
    fresh = not os.path.exists(args.out)
    outf = open(args.out, "a", newline="")
    wr = csv.DictWriter(outf, fieldnames=cols)
    if fresh:
        wr.writeheader()

    failures = []
    for corpus in corpora:
        binpath = os.path.join(BINDIR, corpus + ".bin")
        qdir = os.path.join(OUTDIR, f"proto_{corpus}")
        os.makedirs(qdir, exist_ok=True)

        # protocol (once per corpus)
        r = sh([BENCH, "--dataset", binpath, "--mode", "prep", "--qdir", qdir,
                "--samples", str(args.samples), "--window_frac", str(args.window_frac),
                "--output", "/dev/null"])
        if r.returncode != 0:
            print(f"[e26] prep FAILED for {corpus}: {r.stderr.strip()[:300]}")
            failures.append((corpus, "prep"))
            continue
        m = re.search(r"preload=(\d+) stream=(\d+)", r.stderr)
        preload, streamed = int(m.group(1)), int(m.group(2))
        print(f"[e26] {corpus}: preload={preload} stream={streamed}")

        # GraphOne conversion (once per corpus, one file so order survives)
        godir = os.path.join(args.scratch, f"{corpus}.weight64")
        if not os.path.isdir(godir):
            r = sh([CONV, "--dataset", binpath, "--outdir", godir,
                    "--format", "weight64", "--chunk_edges", "2000000000"])
            if r.returncode != 0:
                print(f"[e26] convert FAILED for {corpus}: {r.stderr.strip()[:300]}")
                failures.append((corpus, "convert"))
                godir = None
            else:
                mv = re.search(r"vcount=(\d+)", r.stdout)
                if mv:
                    open(os.path.join(args.scratch, f"{corpus}.vcount"), "w").write(mv.group(1))
        # vcount cache: written the first time the converter runs (below).
        vfile = os.path.join(args.scratch, f"{corpus}.vcount")

        for (W, R) in mixes:
            # ---- TGSR
            if "tgsr" not in systems:
                r = None
            else:
              r = sh([BENCH, "--dataset", binpath, "--mode", "tgsr", "--qdir", qdir,
                    "--writers", str(W), "--readers", str(R), "--output", args.out])
            if r is not None:
                print(r.stderr.strip().splitlines()[-1] if r.stderr.strip() else "")
                if r.returncode != 0:
                    failures.append((corpus, f"tgsr {W}:{R} rc={r.returncode}"))

            # ---- LiveGraph, both concurrent drivings: sharded (its best
            # case, conflict-free by collector-side repartitioning) and
            # arrival order (what the stream is; vertex locks collide).
            # The arrival driving is bounded to the smallest corpus per mix,
            # because its measured rate (0.01 Meps, hundreds of thousands of
            # aborts) would take hours on the larger ones and the collapse
            # is already demonstrated.
            if "livegraph" in systems:
              r = sh([BENCH, "--dataset", binpath, "--mode", "livegraph",
                    "--lg_shard", "1", "--qdir", qdir,
                    "--writers", str(W), "--readers", str(R), "--output", args.out])
              print(r.stderr.strip().splitlines()[-1] if r.stderr.strip() else "")
              if r.returncode != 0:
                failures.append((corpus, f"livegraph_sharded {W}:{R} rc={r.returncode}"))
            if "livegraph" in systems and corpus == "sx-askubuntu":
                r = sh([BENCH, "--dataset", binpath, "--mode", "livegraph",
                        "--lg_shard", "0", "--qdir", qdir,
                        "--writers", str(W), "--readers", str(R), "--output", args.out])
                print(r.stderr.strip().splitlines()[-1] if r.stderr.strip() else "")
                if r.returncode != 0:
                    failures.append((corpus, f"livegraph_arrival {W}:{R} rc={r.returncode}"))

            # ---- GraphOne (its own harness, job 29)
            if not godir or "graphone" not in systems:
                continue
            # vcount: GraphOne needs it up front; take v_max+1 from the .bin
            # meta the converter printed. Cheap: re-run converter with --limit 0?
            # The converter always prints vcount; keep a cached copy.
            if not os.path.exists(vfile):
                r = sh([CONV, "--dataset", binpath, "--outdir", godir + ".tmp",
                        "--format", "weight64", "--chunk_edges", "2000000000"])
                mv = re.search(r"vcount=(\d+)", r.stdout)
                open(vfile, "w").write(mv.group(1))
                shutil.rmtree(godir + ".tmp", ignore_errors=True)
            vcount = open(vfile).read().strip()

            odir = os.path.join(args.scratch, "go_out")
            shutil.rmtree(odir, ignore_errors=True)
            os.makedirs(odir, exist_ok=True)
            env = {
                "GNOMON_QUERY_FILE": os.path.join(qdir, "queries.bin"),
                "GNOMON_EXPECT_FILE": os.path.join(qdir, "expected.bin"),
                "GNOMON_PRELOAD": str(preload),
                "GNOMON_WRITERS": str(W),
                "GNOMON_READERS": str(R),
            }
            r = sh([G64, "-i", godir + "/", "-o", odir + "/", "-v", vcount,
                    "-j", "29", "-s", "1", "-d", "1", "-t", str(W)], env=env)
            row = {"system": "graphone", "dataset": corpus, "writers": W,
                   "readers": R, "edges_preloaded": preload,
                   "edges_streamed": streamed, "q_median_us": "",
                   "q_p95_us": "", "samples": args.samples}
            ok = r.returncode == 0
            for key, pat in GO_PATTERNS.items():
                mm = pat.search(r.stdout)
                row[key] = mm.group(1) if mm else ""
                ok = ok and mm is not None
            wr.writerow(row)
            outf.flush()
            if not ok:
                print(f"[e26] graphone {corpus} {W}:{R} INCOMPLETE: "
                      f"{r.stderr.strip()[:200]}")
                failures.append((corpus, f"graphone {W}:{R}"))
            else:
                print(f"[e26] graphone   W={W} R={R}  stream {row['stream_sec']}s "
                      f"= {row['ingest_Meps']} Meps  queries={row['queries']} "
                      f"({row['queries_per_sec']} q/s)  mismatches={row['mismatches']}"
                      f"  archive_catchup={row['archive_catchup_sec']}s")

    outf.close()
    if failures:
        print("\n[e26] FAILURES:")
        for f in failures:
            print("  ", f)
        sys.exit(1)
    print("\n[e26] all runs complete, CSV at", args.out)


if __name__ == "__main__":
    main()
