# LEGACY: orchestrates the E1-E6 structural experiments (April 2026 framing).
# The provenance experiments (E7-E13) are driven by scripts/reproduce.sh instead.
#!/usr/bin/env python3
"""Orchestrator for the 6 experiments.

Storage convention:
  Each invocation creates results/<exp>/runs/<label>/data.csv with a
  manifest.json describing what this sweep was. CSVs are append-only; nothing
  is ever wiped. Use scripts/aggregate.py to merge across runs.

Usage:
    run_all.py e1 [--small | --full | --huge] [--datasets ...]
                  [--label NAME] [--note "description"]
                  [--threads N] [--trials N] [--timeout SECS]
    run_all.py e1_neo4j ...
    run_all.py e2_neo4j ...
    run_all.py all [--small | --full]
"""

import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT      = Path(__file__).resolve().parents[1]
BUILD     = ROOT / "build"
DATA_BIN  = Path("/home/jding/dataset/bin")
RESULTS   = ROOT / "results"

# ---------------------------------------------------------------------- TIERS
TINY    = ["CollegeMsg", "email-Eu-core-temporal", "act-mooc",
           "sx-mathoverflow", "sx-askubuntu"]
SMALL   = TINY + ["sx-superuser", "wiki-talk-temporal"]
MEDIUM  = SMALL + ["cit-Patents"]
LARGE   = MEDIUM + ["sx-stackoverflow", "soc-LiveJournal1", "bitcoin-temporal"]
HUGE    = LARGE + ["temporal-reddit-reply"]
DEFAULT_SMALL = SMALL
DEFAULT_FULL  = LARGE

# ---------------------------------------------------------------------- HELPERS
def log(msg):
    print(f"[run] {msg}", flush=True)

def drop_caches():
    try:
        subprocess.run(["sync"], check=False)
        subprocess.run(["sudo", "-n", "sysctl", "-q", "vm.drop_caches=3"],
                       check=False, stderr=subprocess.DEVNULL)
    except Exception:
        pass

def must_exist(path):
    if not path.exists():
        log(f"MISSING: {path}")
        return False
    return True

def run_bench(binary, bench_args, timeout=None):
    drop_caches()
    cmd = [str(binary)] + bench_args
    log("$ " + " ".join(cmd))
    try:
        return subprocess.run(cmd, timeout=timeout,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
    except subprocess.TimeoutExpired:
        log(f"TIMEOUT after {timeout}s")
        return None
    except Exception as e:
        log(f"EXCEPTION: {e}")
        return None

def datasets_for(args, default):
    if args.datasets:    return args.datasets
    if args.huge:        return HUGE
    if args.full:        return DEFAULT_FULL
    if args.small:       return DEFAULT_SMALL
    return default

def tier_for(args):
    if args.datasets: return "custom"
    if args.huge:     return "huge"
    if args.full:     return "full"
    if args.small:    return "small"
    return "default"

# ---- per-run directory + manifest ---------------------------------------
def now_iso():
    return datetime.datetime.utcnow().strftime("%Y-%m-%dT%H_%M")

def make_run_dir(exp: str, args, datasets, extra: dict = None):
    """Create a fresh runs/<label>/ directory for this sweep, write env.json
    and an initial manifest.json. Returns the directory and the data.csv path."""
    tier = tier_for(args)
    label_parts = [now_iso(), tier]
    if args.note:
        # sanitize for filename
        safe = "".join(c if c.isalnum() or c in "_-" else "_" for c in args.note)
        label_parts.append(safe)
    elif args.label:
        label_parts = [args.label]
    label = "_".join(label_parts)

    run_dir = RESULTS / exp / "runs" / label
    run_dir.mkdir(parents=True, exist_ok=True)
    data_csv = run_dir / "data.csv"

    # env.json — record machine + compiler fingerprint.
    rec = BUILD / "barry_record_env"
    if rec.exists():
        out = subprocess.run([str(rec)], stdout=subprocess.PIPE, text=True).stdout
        (run_dir / "env.json").write_text(out)

    # git sha if available
    git_sha = ""
    try:
        git_sha = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        pass

    manifest = {
        "experiment":   exp,
        "label":        label,
        "tier":         tier,
        "started_utc":  datetime.datetime.utcnow().isoformat() + "Z",
        "ended_utc":    None,
        "command":      " ".join(sys.argv),
        "git_sha":      git_sha,
        "datasets":     datasets,
        "threads":      args.threads,
        "trials":       args.trials,
        "timeout":      args.timeout,
        "note":         args.note or "",
    }
    if extra:
        manifest.update(extra)
    (run_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    log(f"[{exp}] run dir: {run_dir.relative_to(ROOT)}")
    return run_dir, data_csv, manifest

def finalize_manifest(run_dir: Path, manifest: dict, status: str = "ok"):
    manifest["ended_utc"] = datetime.datetime.utcnow().isoformat() + "Z"
    manifest["status"]    = status
    (run_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

# ---------------------------------------------------------------------- E1
def run_e1(args):
    bin_path  = BUILD / "bench_e1_ingest"
    datasets  = datasets_for(args, DEFAULT_SMALL)
    threads   = args.threads or 16
    trials    = args.trials  or 3
    batches   = args.batches or [10, 1000, 100_000, 10_000_000]

    run_dir, csv_path, manifest = make_run_dir(
        "e1_ingest", args, datasets,
        extra={"batches": batches, "system": "barry"})

    try:
        for ds in datasets:
            ds_path = DATA_BIN / f"{ds}.bin"
            if not must_exist(ds_path): continue
            for batch in batches:
                for trial in range(trials):
                    run_bench(bin_path, [
                        "--dataset", str(ds_path),
                        "--batch", str(batch),
                        "--threads", str(threads),
                        "--trial", str(trial),
                        "--output", str(csv_path),
                    ], timeout=args.timeout)
    finally:
        finalize_manifest(run_dir, manifest)

# ---------------------------------------------------------------------- E1 (Neo4j)
NEO4J_TINY  = ["CollegeMsg", "email-Eu-core-temporal", "act-mooc",
               "sx-mathoverflow", "sx-askubuntu", "sx-superuser"]
NEO4J_BENCH = ROOT / "baselines" / "neo4j" / "bench_neo4j.py"

def run_e1_neo4j(args):
    if not NEO4J_BENCH.exists():
        log("neo4j wrapper missing — skipping"); return
    datasets = args.datasets or NEO4J_TINY
    batches  = args.batches  or [10, 1000, 100_000]
    trials   = args.trials   or 2
    run_dir, csv_path, manifest = make_run_dir(
        "e1_ingest", args, datasets,
        extra={"batches": batches, "system": "neo4j"})

    try:
        for ds in datasets:
            ds_path = DATA_BIN / f"{ds}.bin"
            if not must_exist(ds_path): continue
            for batch in batches:
                for trial in range(trials):
                    drop_caches()
                    cmd = ["python3", str(NEO4J_BENCH), "ingest",
                           "--dataset", str(ds_path),
                           "--batch", str(batch),
                           "--threads", "1",
                           "--trial", str(trial),
                           "--reset", "1",
                           "--output", str(csv_path)]
                    log("$ " + " ".join(cmd))
                    subprocess.run(cmd, timeout=args.timeout or 600,
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL,
                                   check=False)
    finally:
        finalize_manifest(run_dir, manifest)

# ---------------------------------------------------------------------- E1 (DCSR via BYO)
DCSR_INGEST_BIN = (ROOT / "baselines" / "BYO" / "bazel-bin" /
                   "benchmarks" / "run_structures" / "run_dcsr_ingest")

def run_e1_dcsr(args):
    if not DCSR_INGEST_BIN.exists():
        log(f"DCSR ingest binary missing at {DCSR_INGEST_BIN} — skipping"); return
    datasets = datasets_for(args, DEFAULT_SMALL)
    threads  = args.threads or 16
    trials   = args.trials  or 3
    batches  = args.batches or [10, 1000, 100_000, 10_000_000]
    run_dir, csv_path, manifest = make_run_dir(
        "e1_ingest", args, datasets,
        extra={"batches": batches, "system": "dcsr",
               "binary": str(DCSR_INGEST_BIN.relative_to(ROOT))})
    try:
        for ds in datasets:
            ds_path = DATA_BIN / f"{ds}.bin"
            if not must_exist(ds_path): continue
            for batch in batches:
                for trial in range(trials):
                    run_bench(DCSR_INGEST_BIN, [
                        "--dataset", str(ds_path),
                        "--batch", str(batch),
                        "--threads", str(threads),
                        "--trial", str(trial),
                        "--output", str(csv_path),
                    ], timeout=args.timeout)
    finally:
        finalize_manifest(run_dir, manifest)

# ---------------------------------------------------------------------- E2
def run_e2(args):
    bin_path  = BUILD / "bench_e2_query"
    datasets  = datasets_for(args, DEFAULT_SMALL)
    queries   = args.queries or ["q1", "q2", "q3", "q4", "q5", "q6"]
    threads   = args.threads or 16
    trials    = args.trials  or 3
    samples   = {"q1": 200, "q2": 200, "q3": 5,
                 "q4": 100, "q5": 200, "q6": 10}

    run_dir, csv_path, manifest = make_run_dir(
        "e2_query", args, datasets,
        extra={"queries": queries, "samples": samples, "system": "barry"})

    try:
        for ds in datasets:
            ds_path = DATA_BIN / f"{ds}.bin"
            if not must_exist(ds_path): continue
            for q in queries:
                for trial in range(trials):
                    run_bench(bin_path, [
                        "--dataset", str(ds_path),
                        "--query", q,
                        "--num_samples", str(samples[q]),
                        "--threads", str(threads),
                        "--trial", str(trial),
                        "--output", str(csv_path),
                    ], timeout=args.timeout)
    finally:
        finalize_manifest(run_dir, manifest)

# ---------------------------------------------------------------------- E2 (Neo4j)
def run_e2_neo4j(args):
    if not NEO4J_BENCH.exists():
        log("neo4j wrapper missing — skipping"); return
    datasets = args.datasets or NEO4J_TINY
    queries  = ["q1", "q2", "q5"]
    trials   = args.trials or 2
    samples  = {"q1": 30, "q2": 30, "q5": 30}

    run_dir, csv_path, manifest = make_run_dir(
        "e2_query", args, datasets,
        extra={"queries": queries, "samples": samples, "system": "neo4j"})

    try:
        for ds in datasets:
            ds_path = DATA_BIN / f"{ds}.bin"
            if not must_exist(ds_path): continue
            log(f"[neo4j-e2] loading {ds} into Neo4j...")
            subprocess.run(
                ["python3", str(NEO4J_BENCH), "ingest",
                 "--dataset", str(ds_path),
                 "--batch", "10000", "--threads", "1",
                 "--trial", "0", "--reset", "1",
                 "--output", "/tmp/_neo4j_e2_ingest.csv"],
                timeout=args.timeout or 1800,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                check=False)
            for q in queries:
                for trial in range(trials):
                    drop_caches()
                    cmd = ["python3", str(NEO4J_BENCH), "query",
                           "--dataset", str(ds_path),
                           "--query", q,
                           "--num_samples", str(samples[q]),
                           "--threads", "1",
                           "--trial", str(trial),
                           "--output", str(csv_path)]
                    log("$ " + " ".join(cmd))
                    subprocess.run(cmd, timeout=args.timeout or 1800,
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL,
                                   check=False)
    finally:
        finalize_manifest(run_dir, manifest)

# ---------------------------------------------------------------------- E3
def run_e3(args):
    bin_path = BUILD / "bench_e3_compute"
    datasets = datasets_for(args, DEFAULT_SMALL)
    algos    = args.algos or ["bfs", "pr", "tc", "sssp", "cc", "kcore"]
    systems  = args.systems or ["barry", "csr"]
    threads  = args.threads or 16
    trials   = args.trials  or 3

    run_dir, csv_path, manifest = make_run_dir(
        "e3_compute", args, datasets,
        extra={"algos": algos, "systems": systems})

    try:
        for ds in datasets:
            ds_path = DATA_BIN / f"{ds}.bin"
            if not must_exist(ds_path): continue
            for algo in algos:
                for sys_ in systems:
                    for trial in range(trials):
                        run_bench(bin_path, [
                            "--dataset", str(ds_path),
                            "--algo", algo,
                            "--system", sys_,
                            "--threads", str(threads),
                            "--trial", str(trial),
                            "--output", str(csv_path),
                        ], timeout=args.timeout)
    finally:
        finalize_manifest(run_dir, manifest)

# ---------------------------------------------------------------------- E4
def run_e4(args):
    bin_path = BUILD / "bench_e4_scale"
    if args.datasets:
        datasets = args.datasets
    elif args.small or not (args.full or args.huge):
        datasets = ["sx-stackoverflow"]
    else:
        datasets = datasets_for(args, DEFAULT_FULL)
    n_cpu = os.cpu_count() or 16
    thread_grid = [t for t in [1, 2, 4, 8, 16, 20, 32, 64] if t <= n_cpu]
    if thread_grid and thread_grid[-1] != n_cpu and n_cpu > 1:
        thread_grid.append(n_cpu)
    batches = args.batches or [1000, 1_000_000]
    trials  = args.trials  or 3

    run_dir, csv_path, manifest = make_run_dir(
        "e4_scale", args, datasets,
        extra={"thread_grid": thread_grid, "batches": batches})

    try:
        for ds in datasets:
            ds_path = DATA_BIN / f"{ds}.bin"
            if not must_exist(ds_path): continue
            for t in thread_grid:
                for batch in batches:
                    for trial in range(trials):
                        run_bench(bin_path, [
                            "--dataset", str(ds_path),
                            "--threads", str(t),
                            "--batch", str(batch),
                            "--trial", str(trial),
                            "--output", str(csv_path),
                        ], timeout=args.timeout)
    finally:
        finalize_manifest(run_dir, manifest)

# ---------------------------------------------------------------------- E5
def run_e5(args):
    bin_path = BUILD / "bench_e5_memory"
    datasets = datasets_for(args, DEFAULT_FULL)
    threads  = args.threads or 16

    run_dir, csv_path, manifest = make_run_dir(
        "e5_memory", args, datasets, extra={"vidx_modes": [1, 0]})

    try:
        for ds in datasets:
            ds_path = DATA_BIN / f"{ds}.bin"
            if not must_exist(ds_path): continue
            for vidx in (1, 0):
                run_bench(bin_path, [
                    "--dataset", str(ds_path),
                    "--threads", str(threads),
                    "--trial", "0",
                    "--vidx", str(vidx),
                    "--output", str(csv_path),
                ], timeout=args.timeout)
    finally:
        finalize_manifest(run_dir, manifest)

# ---------------------------------------------------------------------- E6
def run_e6(args):
    bin_path = BUILD / "bench_e6_cache"
    if args.datasets:
        datasets = args.datasets
    elif args.small or not (args.full or args.huge):
        datasets = ["sx-superuser", "cit-Patents"]
    else:
        datasets = datasets_for(args, ["sx-superuser", "cit-Patents", "sx-stackoverflow"])
    phases   = ["ingest", "scan"]
    huge_set = [0]
    threads  = args.threads or 16
    trials   = args.trials  or 3
    perf_events = ",".join([
        "cycles", "instructions",
        "L1-dcache-load-misses", "L1-dcache-loads",
        "LLC-load-misses", "LLC-loads",
        "dTLB-load-misses", "dTLB-loads",
    ])

    run_dir, csv_path, manifest = make_run_dir(
        "e6_cache", args, datasets,
        extra={"phases": phases, "huge_pages": huge_set,
               "perf_events": perf_events.split(",")})

    perf_csv = run_dir / "perf.csv"
    perf_csv.write_text("dataset,phase,huge_pages,trial,threads,event,value\n")

    perf_available = shutil.which("perf") is not None
    if not perf_available:
        log("perf not in PATH; collecting timing only")

    try:
        for ds in datasets:
            ds_path = DATA_BIN / f"{ds}.bin"
            if not must_exist(ds_path): continue
            for phase in phases:
                for hp in huge_set:
                    for trial in range(trials):
                        bench_args = [
                            str(bin_path),
                            "--dataset", str(ds_path),
                            "--phase", phase,
                            "--huge_pages", str(hp),
                            "--threads", str(threads),
                            "--trial", str(trial),
                            "--output", str(csv_path),
                        ]
                        if perf_available:
                            cmd = ["perf", "stat", "-x", ",", "-e", perf_events] + bench_args
                        else:
                            cmd = bench_args
                        drop_caches()
                        log("$ " + " ".join(cmd))
                        try:
                            proc = subprocess.run(cmd, timeout=args.timeout,
                                                  stdout=subprocess.PIPE,
                                                  stderr=subprocess.PIPE,
                                                  text=True)
                        except subprocess.TimeoutExpired:
                            log("TIMEOUT"); continue
                        if perf_available:
                            with open(perf_csv, "a") as fp:
                                for line in proc.stderr.splitlines():
                                    cols = line.split(",")
                                    if len(cols) >= 3 and cols[0].replace(".","").isdigit():
                                        fp.write(f"{ds},{phase},{hp},{trial},{threads},"
                                                 f"{cols[2] if len(cols)>2 else ''},{cols[0]}\n")
    finally:
        finalize_manifest(run_dir, manifest)

# ---------------------------------------------------------------------- MAIN
def main():
    p = argparse.ArgumentParser()
    p.add_argument("exp", choices=["e1","e2","e3","e4","e5","e6","all",
                                   "e1_neo4j","e2_neo4j","e1_dcsr"])
    # tier selection (mutually exclusive)
    p.add_argument("--small", action="store_true")
    p.add_argument("--full",  action="store_true")
    p.add_argument("--huge",  action="store_true")
    p.add_argument("--datasets", nargs="+")
    # per-experiment overrides
    p.add_argument("--batches",  nargs="+", type=int)
    p.add_argument("--queries",  nargs="+")
    p.add_argument("--algos",    nargs="+")
    p.add_argument("--systems",  nargs="+")
    p.add_argument("--threads",  type=int, default=0)
    p.add_argument("--trials",   type=int, default=0)
    p.add_argument("--timeout",  type=int, default=0)
    # labelling
    p.add_argument("--label",    default=None,
                   help="explicit run label (overrides auto timestamp+tier)")
    p.add_argument("--note",     default=None,
                   help="short note appended to auto-generated label")
    args = p.parse_args()

    if args.timeout == 0: args.timeout = None

    if args.exp == "all":
        for fn in (run_e1, run_e2, run_e3, run_e4, run_e5, run_e6):
            t0 = time.time()
            fn(args)
            log(f"finished {fn.__name__} in {time.time()-t0:.1f}s")
    else:
        {"e1": run_e1, "e2": run_e2, "e3": run_e3,
         "e4": run_e4, "e5": run_e5, "e6": run_e6,
         "e1_neo4j": run_e1_neo4j,
         "e2_neo4j": run_e2_neo4j,
         "e1_dcsr":  run_e1_dcsr,
         }[args.exp](args)

if __name__ == "__main__":
    main()
