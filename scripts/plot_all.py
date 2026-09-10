#!/usr/bin/env python3
"""Generate the publication figures from runs/<label>/data.csv files via
scripts/aggregate.py. Each run is labelled and we filter to the most useful
subset per experiment.

Usage:
  plot_all.py [exp...]             default: all available
  plot_all.py --runs e1=LABEL ...  pin which run(s) feed each plot
"""
import argparse
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

# Import the aggregator we wrote.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import aggregate

ROOT    = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
FIGS    = RESULTS / "figures"
FIGS.mkdir(parents=True, exist_ok=True)

def load(exp, run_filter=""):
    """Helper that loads via aggregate.load_experiment with optional filter."""
    return aggregate.load_experiment(exp, run_filter=run_filter, include_legacy=True)


# ----------------------------------------------------------------- styling
plt.rcParams.update({
    "figure.figsize": (7.5, 4.5),
    "font.size":      11,
    "axes.titlesize": 12,
    "axes.labelsize": 11,
    "xtick.labelsize": 10,
    "ytick.labelsize": 10,
    "legend.fontsize": 9,
    "axes.spines.top":   False,
    "axes.spines.right": False,
    "savefig.bbox":      "tight",
})


def median_per(group, col):
    return group[col].median()


def save(fig, name):
    out = FIGS / f"{name}.pdf"
    fig.savefig(out, dpi=200)
    print(f"  -> {out}")
    plt.close(fig)


# ----------------------------------------------------------------- E1
def plot_e1():
    df = load("e1_ingest")
    if df.empty: return

    g = (df.groupby(["system", "dataset", "batch"])
           .agg(eps=("throughput_eps", "median"))
           .reset_index())
    systems  = sorted(g["system"].unique())
    datasets = sorted(g["dataset"].unique())
    batches  = sorted(g["batch"].unique())

    # One subplot per batch, datasets on x, systems as bar groups.
    n = len(batches)
    fig, axes = plt.subplots(1, n, figsize=(4*n, 4.5), sharey=True)
    if n == 1: axes = [axes]
    width = 0.8 / max(1, len(systems))

    for ax, b in zip(axes, batches):
        x = np.arange(len(datasets))
        for i, sys_ in enumerate(systems):
            sub = (g[(g["batch"]==b) & (g["system"]==sys_)]
                   .set_index("dataset").reindex(datasets))
            ax.bar(x + i*width, sub["eps"]/1e6, width=width, label=sys_)
        ax.set_xticks(x + width*(len(systems)-1)/2)
        ax.set_xticklabels(datasets, rotation=45, ha="right")
        ax.set_yscale("log")
        ax.set_title(f"batch={b}")
        if ax is axes[0]:
            ax.set_ylabel("M edges / sec")
    axes[-1].legend(loc="upper left", bbox_to_anchor=(1.02, 1))
    fig.suptitle("E1: Ingestion throughput (median; higher is better)")
    save(fig, "e1_ingest")


# ----------------------------------------------------------------- E2
def plot_e2():
    df = load("e2_query")
    if df.empty: return

    g = (df.groupby(["system", "dataset", "query"])
           .agg(med=("median_us", "median"))
           .reset_index())
    queries  = sorted(g["query"].unique())
    systems  = sorted(g["system"].unique())
    datasets = sorted(g["dataset"].unique())

    n = len(queries)
    cols = 3
    rows = (n + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(5*cols, 4*rows), sharey=False)
    if rows*cols == 1: axes = [[axes]]
    elif rows == 1:    axes = [axes]
    width = 0.8 / max(1, len(systems))

    for k, q in enumerate(queries):
        ax = axes[k // cols][k % cols]
        x  = np.arange(len(datasets))
        for i, sys_ in enumerate(systems):
            sub = (g[(g["query"]==q) & (g["system"]==sys_)]
                   .set_index("dataset").reindex(datasets))
            ax.bar(x + i*width, sub["med"], width=width, label=sys_)
        ax.set_xticks(x + width*(len(systems)-1)/2)
        ax.set_xticklabels(datasets, rotation=45, ha="right")
        ax.set_yscale("log")
        ax.set_title(q.upper())
        if k % cols == 0: ax.set_ylabel("Median latency (µs)")
        if k == 0: ax.legend(loc="upper left")

    # Hide unused subplots.
    for k in range(n, rows*cols):
        axes[k // cols][k % cols].axis("off")

    fig.suptitle("E2: Temporal-query latency (lower is better; Barry vs baselines)")
    save(fig, "e2_query")


# ----------------------------------------------------------------- E3
def plot_e3():
    df = load("e3_compute")
    if df.empty: return

    g = (df.groupby(["system", "algo", "dataset"])
           .agg(secs=("run_seconds", "median"))
           .reset_index())

    pivot = g.pivot_table(index=["dataset","algo"], columns="system",
                          values="secs").reset_index()
    if "csr" not in pivot.columns or "barry" not in pivot.columns:
        return
    pivot["norm"] = pivot["barry"] / pivot["csr"]

    algos    = sorted(pivot["algo"].unique())
    datasets = sorted(pivot["dataset"].unique())

    fig, ax = plt.subplots()
    width = 0.8 / max(1, len(algos))
    x = np.arange(len(datasets))
    for i, a in enumerate(algos):
        sub = pivot[pivot["algo"]==a].set_index("dataset").reindex(datasets)
        ax.bar(x + i*width, sub["norm"], width=width, label=a.upper())
    ax.axhline(1.0, ls="--", color="gray", lw=0.8)
    ax.set_xticks(x + width*(len(algos)-1)/2)
    ax.set_xticklabels(datasets, rotation=30, ha="right")
    ax.set_ylabel("Run time / CSR run time")
    ax.set_title("E3: Graph algorithms (Barry slowdown vs static CSR)")
    ax.legend()
    save(fig, "e3_compute")


# ----------------------------------------------------------------- E4
def plot_e4():
    df = load("e4_scale")
    if df.empty: return

    g = (df.groupby(["dataset", "batch", "threads"])
           .agg(eps=("throughput_eps", "median"))
           .reset_index())

    fig, ax = plt.subplots()
    for (ds, batch), sub in g.groupby(["dataset", "batch"]):
        sub = sub.sort_values("threads")
        ax.plot(sub["threads"], sub["eps"]/1e6, "o-",
                label=f"{ds} (batch={batch})")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("Threads")
    ax.set_ylabel("Throughput (M edges / s)")
    ax.set_title("E4: Ingestion scalability (Barry)")
    ax.legend()
    save(fig, "e4_scale")


# ----------------------------------------------------------------- E5
def plot_e5():
    df = load("e5_memory")
    if df.empty: return

    df["raw_MB"]    = df["raw_bytes"] / 1e6
    df["arena_MB"]  = df["arena_used_bytes"] / 1e6
    df["vidx_MB"]   = df["vertex_idx_bytes"] / 1e6
    df["rss_MB"]    = df["rss_after_bytes"] / 1e6

    # Use vidx-on rows for a stacked bar; vidx-off rows are kept in the CSV.
    on  = df[df["vidx"]==1].set_index("dataset")
    off = df[df["vidx"]==0].set_index("dataset")
    datasets = list(on.index)

    fig, ax = plt.subplots()
    x = np.arange(len(datasets))
    ax.bar(x, on["raw_MB"],   label="raw edges")
    ax.bar(x, on["arena_MB"], bottom=on["raw_MB"], label="arena (blocks)")
    ax.bar(x, on["vidx_MB"],
           bottom=on["raw_MB"] + on["arena_MB"], label="vertex index")
    ax.set_xticks(x)
    ax.set_xticklabels(datasets, rotation=30, ha="right")
    ax.set_ylabel("Bytes (MB)")
    ax.set_yscale("log")
    ax.set_title("E5: Memory footprint (Barry)")
    ax.legend()
    save(fig, "e5_memory")


# ----------------------------------------------------------------- E6
def plot_e6():
    df = load("e6_cache")
    if df.empty: return

    g = (df.groupby(["dataset", "phase", "huge_pages"])
           .agg(ops_s=("ops_per_sec", "median"))
           .reset_index())

    fig, ax = plt.subplots()
    width = 0.35
    phases = sorted(g["phase"].unique())
    datasets = sorted(g["dataset"].unique())
    x = np.arange(len(datasets))
    for i, ph in enumerate(phases):
        sub = g[(g["phase"]==ph)].set_index("dataset").reindex(datasets)
        ax.bar(x + i*width, sub["ops_s"]/1e6, width=width, label=ph)
    ax.set_xticks(x + width/2)
    ax.set_xticklabels(datasets, rotation=30, ha="right")
    ax.set_ylabel("Operations / s (M)")
    ax.set_yscale("log")
    ax.set_title("E6: Operation throughput (timing only)")
    ax.legend()
    save(fig, "e6_cache_timing")

    p = aggregate.load_experiment("e6_cache", run_filter="perf")
    if p.empty: return
    p["value"] = pd.to_numeric(p["value"], errors="coerce")
    p = p.dropna()
    pivot = (p.groupby(["dataset","phase","event"])
              .agg(v=("value","median"))
              .reset_index())
    pivot.to_csv(RESULTS / "e6_cache" / "e6_perf_summary.csv", index=False)


# ----------------------------------------------------------------- main
def main():
    p = argparse.ArgumentParser()
    p.add_argument("exps", nargs="*",
                   default=["e1","e2","e3","e4","e5","e6"])
    args = p.parse_args()
    fns = {"e1":plot_e1,"e2":plot_e2,"e3":plot_e3,
           "e4":plot_e4,"e5":plot_e5,"e6":plot_e6}
    for e in args.exps:
        if e in fns:
            print(f"plotting {e}")
            try: fns[e]()
            except Exception as exc:
                print(f"  FAILED: {exc}", file=sys.stderr)

if __name__ == "__main__":
    main()
