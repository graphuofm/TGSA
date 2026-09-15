#!/usr/bin/env python3
"""Figures for the FAST submission, generated from recorded result CSVs.

Accessibility (FAST CFP requires submissions be intelligible in grayscale):
  * Okabe-Ito colorblind-safe palette.
  * Every series is redundantly encoded -- colour AND marker AND linestyle;
    every bar group redundantly encoded -- colour AND hatch.
  * No text below 7pt at final (column-width) size.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

R = '/home/jding/Barry/results/'
FIG = '/home/jding/Barry/paper_FAST/figures/'
os.makedirs(FIG, exist_ok=True)

plt.rcParams.update({
    'font.size': 8, 'font.family': 'serif',
    'font.serif': ['DejaVu Serif'],
    'axes.spines.top': False, 'axes.spines.right': False,
    'axes.labelsize': 8, 'axes.titlesize': 8.5,
    'xtick.labelsize': 7.5, 'ytick.labelsize': 7.5,
    'legend.fontsize': 7.2, 'figure.dpi': 400,
    'axes.grid': True, 'grid.alpha': 0.25, 'grid.linewidth': 0.4,
    'axes.axisbelow': True, 'lines.linewidth': 1.4, 'lines.markersize': 4,
})

# Okabe-Ito, colorblind-safe; luminance-separated so greyscale still ranks them.
STYLE = {
    'barry':  dict(c='#000000', m='o', ls='-',   h='',    lbl='Barry'),
    'tcsr':   dict(c='#0072B2', m='s', ls='--',  h='///',  lbl='sorted CSR (static)'),
    'ucsr':   dict(c='#E69F00', m='^', ls='-.',  h='\\\\\\', lbl='arrival CSR (ingesting)'),
    'csr':    dict(c='#009E73', m='D', ls=(0,(1,1)), h='xxx', lbl='topology CSR (optimum)'),
    'duckdb': dict(c='#D55E00', m='v', ls=(0,(3,1,1,1)), h='...', lbl='DuckDB (columnar)'),
    'tva':    dict(c='#CC79A7', m='P', ls=':',   h='+++',  lbl='TVA (versioned)'),
}
def med(v):
    v = sorted(v); n = len(v)
    return v[n//2] if n % 2 else 0.5*(v[n//2-1]+v[n//2])

def load(path):
    return list(csv.DictReader(open(R + path)))

# =====================================================================
# Fig. streaming: the dynamic axis -- ingest rate, and what static layouts lose
# =====================================================================
def fig_streaming():
    fig, (a, b) = plt.subplots(1, 2, figsize=(7.0, 2.15),
                               gridspec_kw={'width_ratios': [1.35, 1]})

    # (a) single-thread ingestion, log scale. Three bar classes, redundantly
    # encoded by colour AND hatch so the ranking survives greyscale printing.
    bars = [
        ('TVA',       1447,    '#CC79A7', '+++', 'versioned store (DNF bound)'),
        ('fsync\nper batch', 1.22e6, '#555555', '...', 'Barry, durable (WAL)'),
        ('64 MB\ncommit',    4.22e6, '#555555', '...', None),
        ('wiki-talk', 13.14e6, '#000000', '',    'Barry, volatile'),
        ('CADETS',    15.53e6, '#000000', '',    None),
        ('THEIA',     11.92e6, '#000000', '',    None),
        ('LANL\n12 writers', 6.76e6, '#000000', '', None),
    ]
    for i, (lbl, v, col, hat, _) in enumerate(bars):
        a.bar(i, v, width=0.68, color=col, hatch=hat,
              edgecolor='white', linewidth=0.7, zorder=3)
        a.text(i, v*1.45, (f"{v/1e6:.1f}M" if v >= 1e6 else f"$\\leq${v:,.0f}"),
               ha='center', fontsize=7, zorder=4)
    a.set_yscale('log'); a.set_ylim(500, 3e8)
    a.set_ylabel('events / s ingested')
    a.set_xticks(range(len(bars)))
    a.set_xticklabels([b[0] for b in bars], fontsize=6.6)
    a.set_xlim(-0.8, len(bars)-0.2)
    a.legend(handles=[Patch(facecolor=c_, hatch=h_, edgecolor='white', label=l_)
                      for _, _, c_, h_, l_ in bars if l_],
             frameon=False, loc='upper left', fontsize=6.5,
             handlelength=1.2, handletextpad=0.35, borderpad=0.1,
             labelspacing=0.32, bbox_to_anchor=(-0.02, 1.03))
    a.annotate('did not finish\nin 90 min', xy=(0, 2400), xytext=(0, 3.0e4),
               fontsize=6.3, style='italic', ha='center',
               arrowprops=dict(arrowstyle='->', lw=0.6, color='#777777'))
    a.set_title('(a) Streaming ingestion', loc='left', fontsize=8)

    # (b) what a static structure silently loses while it rebuilds
    ds = ['wiki-talk\n7.8M edges\n(rebuild 0.2 s)', 'LANL\n1.05B edges\n(rebuild 28.4 s)']
    mean_loss, worst_loss = [34.1, 33.2], [96.7, 99.5]
    x = [0, 1]
    b.bar([i-0.19 for i in x], mean_loss, width=0.34, color='#0072B2', hatch='///',
          edgecolor='white', linewidth=0.7, label='mean over queries', zorder=3)
    b.bar([i+0.19 for i in x], worst_loss, width=0.34, color='#D55E00', hatch='...',
          edgecolor='white', linewidth=0.7, label='worst query', zorder=3)
    for i in x:
        b.text(i-0.19, mean_loss[i]+3, f"{mean_loss[i]:.0f}%", ha='center', fontsize=7)
        b.text(i+0.19, worst_loss[i]+3, f"{worst_loss[i]:.0f}%", ha='center', fontsize=7)
    # Barry occupies the same axis at zero: no rebuild, so no blind window.
    b.bar([2.0], [0.6], width=0.5, color='#009E73', edgecolor='white',
          linewidth=0.7, zorder=3)
    b.text(2.0, 5, '0%', ha='center', fontsize=7.5, fontweight='bold',
           color='#007a58')
    b.axhline(0, color='#000000', lw=1.2, zorder=5)
    b.set_xticks(x + [2.0])
    b.set_xticklabels(ds + ['Barry\nany size\n(no rebuild)'], fontsize=6.8)
    b.set_ylabel('% of true answer\nsilently missing'); b.set_ylim(-6, 135)
    b.set_xlim(-0.55, 2.5)
    b.legend(frameon=False, loc='upper center', ncol=2, columnspacing=1.0,
             handlelength=1.3, handletextpad=0.4, fontsize=6.8, borderpad=0.15)
    b.set_title('(b) Queries served during a static rebuild', loc='left', fontsize=8)
    fig.tight_layout(pad=0.35, w_pad=1.8)
    fig.savefig(FIG + 'fig_streaming.pdf'); plt.close(fig)

# =====================================================================
# Fig. temporal: the time axis -- three ways time-keyed storage pays off
# =====================================================================
def fig_temporal():
    fig, (a, b, c) = plt.subplots(1, 3, figsize=(7.0, 2.1))

    # (a) traversal-to-fixpoint vs query window, two corpora
    rows = load('e9_columnar/e9_pilot.csv')
    pts = defaultdict(list)
    for r in rows:
        if r['query'] != 'traverse': continue
        pts[(r['dataset'], r['system'], float(r['window_frac']))].append(float(r['median_us']))
    for dsname, mark_fill in [('sx-mathoverflow', True), ('wiki-talk-temporal', False)]:
        for sysname in ['duckdb', 'barry', 'tcsr']:
            wfs = sorted({w for (d, s, w) in pts if d == dsname and s == sysname})
            if not wfs: continue
            ys = [med(pts[(dsname, sysname, w)]) for w in wfs]
            st = STYLE[sysname]
            a.plot(wfs, ys, marker=st['m'], ls=st['ls'], color=st['c'],
                   mfc=st['c'] if mark_fill else 'white', mew=1.0, zorder=3)
    a.set_xscale('log'); a.set_yscale('log')
    a.set_xlabel('query window (fraction of span)')
    a.set_ylabel('median latency (µs)')
    a.set_title('(a) Time-respecting traversal', loc='left', fontsize=8)
    a.legend(handles=[Line2D([], [], color=STYLE[s]['c'], marker=STYLE[s]['m'],
                             ls=STYLE[s]['ls'], label=STYLE[s]['lbl'])
                      for s in ['duckdb', 'barry', 'tcsr']],
             frameon=False, loc='upper left', handlelength=2.0, borderpad=0.2,
             fontsize=6.6, bbox_to_anchor=(-0.02, 0.72))


    # (b) time-windowed BFS: cost PER DELIVERED EDGE, the only size-independent
    # way to compare layouts across corpora spanning 60K--1.05B edges.
    per_edge = defaultdict(list)
    deg_of = {'CollegeMsg': 31.5, 'sx-mathoverflow': 20.4, 'sx-askubuntu': 6.1,
              'wiki-talk-temporal': 6.9, 'email-Eu-core-temporal': 337.1,
              'lanl-auth': 59517.0}
    for f in ['e13_storage/e13_full.csv', 'e13_storage/e13_lanl.csv']:
        for r in load(f):
            if r['algo'] != 'twbfs': continue
            d = deg_of.get(r['dataset']); n = int(r['edges_delivered'])
            if d and n > 1000:
                per_edge[(d, r['engine'])].append(float(r['storage_sec'])*1e9/n)
    degs = sorted({d for (d, _) in per_edge})
    for sysname in ['ucsr', 'barry', 'tcsr']:
        xs = [d for d in degs if (d, sysname) in per_edge]
        ys = [med(per_edge[(d, sysname)]) for d in xs]
        st = STYLE[sysname]
        b.plot(xs, ys, marker=st['m'], ls=st['ls'], color=st['c'], zorder=3)
    b.set_xscale('log'); b.set_yscale('log')
    b.set_xlabel('mean out-degree')
    b.set_ylabel('ns per delivered edge')
    b.set_title('(b) Windowed BFS vs density', loc='left', fontsize=8)
    if (59517.0, 'ucsr') in per_edge and (59517.0, 'barry') in per_edge:
        yu, yb = med(per_edge[(59517.0, 'ucsr')]), med(per_edge[(59517.0, 'barry')])
        b.annotate('', xy=(59517, yu*0.85), xytext=(59517, yb*1.2),
                   arrowprops=dict(arrowstyle='<->', lw=0.9, color='#333333'))
        b.text(26000, (yu*yb)**0.5, f'{yu/yb:.0f}$\\times$', fontsize=8.5,
               ha='right', fontweight='bold')
    b.axvspan(8000, 3e5, color='#000000', alpha=0.055, zorder=0)
    b.text(5.5e4, 3.2, 'event-stream\nregime', fontsize=6.3, ha='center',
           style='italic', color='#444444')
    b.set_ylim(1, 4500)
    b.legend(handles=[Line2D([], [], color=STYLE[s]['c'], marker=STYLE[s]['m'],
                             ls=STYLE[s]['ls'], label=STYLE[s]['lbl'])
                      for s in ['ucsr', 'barry', 'tcsr']],
             frameon=False, loc='upper left', handlelength=2.0, borderpad=0.2,
             fontsize=6.6)

    # (c) sparse temporal reachability: cost tracks the answer, not the graph
    rows = load('e7_provenance/e7_pilot.csv')
    by = defaultdict(dict)
    for r in rows: by[r['dataset']][r['query']] = r
    V, sp, dn = [], [], []
    for dsname, d in by.items():
        if 'p2' in d and 'p2d' in d:
            V.append(int(d['p2']['num_vertices']))
            sp.append(float(d['p2']['median_us']))
            dn.append(float(d['p2d']['median_us']))
    order = sorted(range(len(V)), key=lambda i: V[i])
    V = [V[i] for i in order]; sp = [sp[i] for i in order]; dn = [dn[i] for i in order]
    c.plot(V, dn, marker='v', ls=(0,(3,1,1,1)), color='#D55E00',
           label='dense: $O(|V|)$ per query', zorder=3)
    c.plot(V, sp, marker='o', ls='-', color='#000000',
           label='sparse: $O(|R|)$ per query', zorder=3)
    c.set_xscale('log'); c.set_yscale('log')
    c.set_xlabel('|V| (vertices)'); c.set_ylabel('median latency (µs)')
    c.set_title('(c) Reachability cost model', loc='left', fontsize=8)
    c.annotate(f'{dn[-1]/sp[-1]:,.0f}×', xy=(V[-1], (dn[-1]*sp[-1])**0.5),
               fontsize=8, ha='right', fontweight='bold')
    c.set_ylim(0.25, 6e4)
    c.legend(frameon=False, loc='upper left', handlelength=1.8, borderpad=0.15,
             fontsize=6.6, labelspacing=0.3)
    fig.tight_layout(pad=0.4, w_pad=1.5)
    fig.savefig(FIG + 'fig_temporal.pdf'); plt.close(fig)

# =====================================================================
# Fig. structural: the concession, priced across corpora and algorithms
# =====================================================================
def fig_structural():
    """The concession, priced. Horizontal layout keeps corpus names legible;
    series are labelled in place rather than by legend, saving column space."""
    fig, ax = plt.subplots(figsize=(3.34, 2.45))
    rows = load('e13_storage/e13_full.csv') + load('e13_storage/e13_lanl.csv')
    g = defaultdict(list)
    for r in rows:
        if r['algo'] in ('bfs', 'pr', 'cc') and int(r['edges_delivered']) > 1000:
            g[(r['dataset'], r['algo'], r['engine'])].append(float(r['storage_sec']))
    order = [('lanl-auth', 'LANL (1.05B)'), ('wiki-talk-temporal', 'wiki-talk (7.8M)'),
             ('sx-askubuntu', 'askubuntu (964K)'), ('sx-mathoverflow', 'mathoverflow (507K)'),
             ('email-Eu-core-temporal', 'email-Eu (332K)'), ('CollegeMsg', 'CollegeMsg (60K)')]
    algos = [('bfs', 'BFS', '#000000', ''),
             ('pr', 'PageRank', '#0072B2', '///'),
             ('cc', 'label prop.', '#E69F00', '...')]
    H, yt, yl, allr = 0.25, [], [], []
    for di, (dsname, disp) in enumerate(order):
        for ai, (al, aname, col, hat) in enumerate(algos):
            base, ours = g.get((dsname, al, 'csr')), g.get((dsname, al, 'barry'))
            if not (base and ours): continue
            ratio = med(ours) / med(base); allr.append(ratio)
            ypos = di + (1 - ai) * H
            ax.barh(ypos, ratio, height=H*0.86, color=col, hatch=hat,
                    edgecolor='white', linewidth=0.5, zorder=3)
            ax.text(ratio + 0.05, ypos, f'{ratio:.1f}', va='center', fontsize=6.2)
            if di == len(order) - 1:      # label the series in place, top group
                ax.text(0.08, ypos, aname, va='center', fontsize=6.2,
                        color='white' if ai == 0 else '#222222', zorder=4,
                        fontweight='bold')
        yt.append(di); yl.append(disp)
    ax.axvspan(min(allr), max(allr), color='#000000', alpha=0.045, zorder=0)
    ax.axvline(1.0, color='#009E73', lw=1.3, ls=(0, (1, 1)), zorder=4)
    ax.set_yticks(yt); ax.set_yticklabels(yl, fontsize=6.9)
    ax.set_xlabel('adjacency delivery, $\\times$ topology-only CSR', fontsize=7.5)
    ax.set_xlim(0, 3.3); ax.set_ylim(-0.55, len(order) - 0.3)
    ax.text(1.07, -0.48, 'topology-only CSR: optimum,\nno ingest path',
            fontsize=6.1, color='#007a58', style='italic', va='bottom')
    ax.text(3.24, len(order) - 0.38,
            f'{min(allr):.1f}–{max(allr):.1f}$\\times$ overall,'
            f'  median {sorted(allr)[len(allr)//2]:.1f}$\\times$',
            fontsize=6.5, ha='right', va='top')
    fig.tight_layout(pad=0.35)
    fig.savefig(FIG + 'fig_structural.pdf'); plt.close(fig)

fig_streaming(); fig_temporal(); fig_structural()
print('figures written to', FIG)
