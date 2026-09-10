#!/usr/bin/env python3
"""Aggregate all runs for an experiment into a single tall DataFrame.

Layout assumed:
  results/<exp>/runs/<label>/data.csv
  results/<exp>/runs/<label>/manifest.json    (optional but recommended)

CLI:
  aggregate.py <exp> [--filter <substring>] [--out merged.csv] [--list]
  exp values: e1_ingest, e2_query, e3_compute, e4_scale, e5_memory, e6_cache
"""

import argparse
import json
import sys
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"


def list_runs(exp: str):
    base = RESULTS / exp / "runs"
    if not base.exists():
        return []
    out = []
    for run_dir in sorted(base.iterdir()):
        if not run_dir.is_dir():
            continue
        d = run_dir / "data.csv"
        m = run_dir / "manifest.json"
        manifest = {}
        if m.exists():
            try:
                manifest = json.loads(m.read_text())
            except Exception:
                manifest = {}
        out.append({
            "label":    run_dir.name,
            "rows":     sum(1 for _ in d.open()) - 1 if d.exists() else 0,
            "tier":     manifest.get("tier", "?"),
            "started":  manifest.get("started_utc", "?"),
            "datasets": manifest.get("datasets", []),
            "data":     str(d),
        })
    return out


def load_experiment(exp: str, run_filter: str = "", include_legacy: bool = True) -> pd.DataFrame:
    """Concatenate all runs/<label>/data.csv files for an experiment."""
    base = RESULTS / exp / "runs"
    if not base.exists():
        return pd.DataFrame()
    dfs = []
    for run_dir in sorted(base.iterdir()):
        if not run_dir.is_dir():
            continue
        if run_filter and run_filter not in run_dir.name:
            continue
        if not include_legacy and "legacy" in run_dir.name:
            continue
        data = run_dir / "data.csv"
        if not data.exists():
            continue
        try:
            df = pd.read_csv(data)
        except pd.errors.EmptyDataError:
            continue
        df["run_label"] = run_dir.name
        manifest_path = run_dir / "manifest.json"
        if manifest_path.exists():
            try:
                mf = json.loads(manifest_path.read_text())
                df["run_tier"]    = mf.get("tier", "")
                df["run_started"] = mf.get("started_utc", "")
            except Exception:
                pass
        dfs.append(df)
    return pd.concat(dfs, ignore_index=True) if dfs else pd.DataFrame()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exp", choices=[
        "e1_ingest", "e2_query", "e3_compute",
        "e4_scale", "e5_memory", "e6_cache"])
    ap.add_argument("--filter", default="",
                    help="substring filter on run label")
    ap.add_argument("--list", action="store_true",
                    help="list available runs")
    ap.add_argument("--out",  default="",
                    help="write merged CSV to this path")
    ap.add_argument("--no-legacy", action="store_true",
                    help="skip runs whose label contains 'legacy'")
    args = ap.parse_args()

    if args.list:
        rows = list_runs(args.exp)
        if not rows:
            print(f"(no runs in results/{args.exp}/runs/)")
            return
        for r in rows:
            ds = ",".join(r["datasets"]) if isinstance(r["datasets"], list) else str(r["datasets"])
            print(f"{r['label']:<55s} rows={r['rows']:<6d} tier={r['tier']:<8s}  ds=[{ds}]")
        return

    df = load_experiment(args.exp, run_filter=args.filter,
                         include_legacy=not args.no_legacy)
    if df.empty:
        print("(no data)")
        return

    print(f"# {args.exp}: {len(df)} rows from {df.run_label.nunique()} runs", file=sys.stderr)
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        df.to_csv(args.out, index=False)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        df.to_csv(sys.stdout, index=False)


if __name__ == "__main__":
    main()
