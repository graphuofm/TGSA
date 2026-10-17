#!/usr/bin/env python3
"""Shared style for the SIGMOD figures.

Each fig_*.pdf in this directory has a same-named fig_*.py next to it;
running `python3 fig_X.py` regenerates fig_X.pdf in place. This module
holds everything they share: the ONE color legend of the paper, rcParams,
CSV loaders, name maps.

Color legend (identical to the \\definecolor block in main.tex; change both
or neither). A color means one thing in every figure of the paper:

    systems (evaluation plots)            paths (architecture drawings only)
    TGSA        blue    #0072B2           insert path (writes)  vermillion #D55E00
    GraphOne    orange  #E69F00           queries Q1..Q4 (reads) green     #009E73
    LiveGraph   purple  #CC79A7
    RocksDB     brown   #8C564B
    DuckDB      yellow  #F0E442 (black edge)
    Stinger     grey    #aaaaaa
    Sortledton  grey    #555555 (hatched)
    static CSR  white bar, black hatch (a reference, not a system)

Blue is the paper's tone: TGSA and its structures are blue in every figure,
and TGSA-only variants use shades of the same blue (BLUES). The two accent
colors are never used for a system. Greys are for references and notes.
Every series is also encoded redundantly (marker and linestyle for lines,
hatch for bars), so the figures survive greyscale printing.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch, FancyArrowPatch

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, '..', '..'))
R = os.path.join(_REPO, 'results') + '/'
FIG = _HERE + '/'

plt.rcParams.update({
    'font.size': 8, 'font.family': 'serif',
    'font.serif': ['DejaVu Serif'],
    'axes.spines.top': False, 'axes.spines.right': False,
    'axes.labelsize': 8, 'axes.titlesize': 8.5,
    'xtick.labelsize': 7.5, 'ytick.labelsize': 7.5,
    'legend.fontsize': 7.2, 'figure.dpi': 400,
    'axes.grid': True, 'grid.alpha': 0.25, 'grid.linewidth': 0.4,
    'axes.axisbelow': True, 'lines.linewidth': 1.4, 'lines.markersize': 4,
    'hatch.linewidth': 0.5,
})

# ---------------------------------------------------------------- legend
# Canonical system keys are the `system` column values of results/e33_matrix.
SYS = {
    'tgsr':       dict(c='#0072B2', m='o', ls='-',            hatch='',     lbl='TGSA'),
    'graphone':   dict(c='#E69F00', m='s', ls='--',           hatch='',     lbl='GraphOne'),
    'livegraph':  dict(c='#CC79A7', m='^', ls='-.',           hatch='',     lbl='LiveGraph'),
    'rocksdb':    dict(c='#8C564B', m='D', ls=(0, (3, 1, 1, 1)), hatch='', lbl='RocksDB'),
    'duckdb':     dict(c='#F0E442', m='v', ls=(0, (1, 1)),    hatch='',     lbl='DuckDB'),
    'stinger':    dict(c='#aaaaaa', m='P', ls=(0, (4, 2)),    hatch='',     lbl='Stinger'),
    'sortledton': dict(c='#555555', m='X', ls=(0, (2, 2)),    hatch='///',  lbl='Sortledton'),
    'csr':        dict(c='#ffffff', m='x', ls='--',           hatch='xxx',  lbl='static CSR (reference)'),
    'tva':        dict(c='#ffffff', m='+', ls=':',            hatch='...',  lbl='TVA'),
}
# Shades of the paper's blue for TGSA variants (durability modes, modules,
# corpora in a TGSA-only sweep), dark to light.
BLUES = ['#0072B2', '#2F87C2', '#5E9DD1', '#8CB4DF', '#B5CDEB', '#D9E6F5']
# Architecture-drawing path colors, for the rare plot that needs them
# (Figure 1 draws the analyst's questions in the neighbor-query color).
PATH = {'insert': '#D55E00', 'qwin': '#009E73', 'qnbr': '#009E73'}
GREY = '#555555'
LIGHT = '#aaaaaa'

# Aliases that older CSVs use for our own engine.
ALIAS = {'barry': 'tgsr', 'gnomon': 'tgsr', 'TGSA': 'tgsr',
         'livegraph_sharded': 'livegraph', 'go': 'graphone', 'lg': 'livegraph'}

def sysk(name):
    return ALIAS.get(name, name)

def bar_kw(system):
    """Keyword arguments for ax.bar so every system looks the same everywhere."""
    s = SYS[sysk(system)]
    return dict(facecolor=s['c'], edgecolor='black', linewidth=0.5, hatch=s['hatch'])

def line_kw(system):
    s = SYS[sysk(system)]
    return dict(color=s['c'] if s['c'] != '#ffffff' else 'black',
                marker=s['m'], linestyle=s['ls'], label=s['lbl'],
                markeredgecolor='black', markeredgewidth=0.4)

def legend_handles(systems):
    return [Patch(label=SYS[sysk(s)]['lbl'], **bar_kw(s)) for s in systems]

# ------------------------------------------------------------- corpora
# The aligned corpus set of the evaluation, large to small.
CORPORA = ['lanl-auth', 'tc-theia-e3', 'tc-cadets-e3', 'wiki-talk-temporal']
SHORT = {'sx-askubuntu': 'askubuntu', 'wiki-talk-temporal': 'wiki-talk',
         'cit-Patents': 'cit-Patents', 'tc-cadets-e3': 'CADETS',
         'tc-theia-e3': 'THEIA', 'lanl-auth': 'LANL',
         'sx-mathoverflow': 'mathoverflow', 'sx-superuser': 'superuser',
         'email-Eu-core-temporal': 'email-Eu', 'CollegeMsg': 'CollegeMsg',
         'act-mooc': 'act-mooc', 'sx-stackoverflow': 'stackoverflow',
         'soc-LiveJournal1': 'LiveJournal', 'bitcoin-temporal': 'bitcoin',
         'ldbc-snb-sf10': 'LDBC-SNB'}
EVENTS = {'lanl-auth': '1.05B', 'tc-theia-e3': '97.3M', 'tc-cadets-e3': '22.5M',
          'wiki-talk-temporal': '7.8M'}

def med(v):
    v = sorted(v); n = len(v)
    return v[n//2] if n % 2 else 0.5*(v[n//2-1]+v[n//2])

def load(path):
    return list(csv.DictReader(open(R + path)))

def fnum(x, default=0.0):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default

def panel_label(ax, text, x=-0.02, y=1.04):
    ax.text(x, y, text, transform=ax.transAxes, fontsize=8.5,
            fontweight='bold', ha='left', va='bottom')

# ------------------------------------------------------- aligned matrix
SYS_ORDER = ['tgsr', 'graphone', 'livegraph', 'rocksdb', 'duckdb', 'stinger', 'sortledton']

def try_load(path):
    try:
        return load(path)
    except FileNotFoundError:
        return []

def load_e33():
    """Every row of the aligned matrix in the E33 schema, from the in-process
    harness, the GraphOne driver, and the Stinger/Sortledton benches."""
    rows = try_load('e33_matrix/e33.csv') + try_load('e33_matrix/e33_graphone.csv')
    for f, fallback in [('e33_matrix/e31e32.csv', None),
                        ('e31_streaming_baselines/e31.csv', 'stinger'),
                        ('e31_streaming_baselines/e32.csv', 'sortledton')]:
        for r in try_load(f):
            s = r.get('system') or fallback
            rate = fnum(r.get('ingest_Meps') or r.get('ingest_Mpairs_per_s')) * 1e6
            rows.append(dict(system=s, dataset=r['dataset'], phase='ingest',
                             threads=r.get('threads', '8'), events=r['events'],
                             samples_total='0', samples_done='0',
                             seconds=r['ingest_sec'], rate=str(rate), median_us='0',
                             p95_us='0', count='0', checksum='0', checksum_ok='1',
                             rss_MB=r['rss_MB'], note=''))
    for r in rows:
        r['system'] = sysk(r['system'])
    return rows

def e33_cell(rows, system, dataset, phase):
    """The last recorded row for (system, dataset, phase), or None."""
    hits = [r for r in rows if r['system'] == system and r['dataset'] == dataset
            and r['phase'] == phase]
    return hits[-1] if hits else None

def corpus_label(ds):
    """Short corpus name only; the event counts live in Table 3 and the captions."""
    return SHORT.get(ds, ds)

def grouped_bars(ax, corpora, systems, value, log=True, width=None, annotate=None):
    """value(system, corpus) -> (y, note) with y None/0 for a missing cell.
    Draws one group per corpus, one bar per system, in the paper's legend.
    note 'n/a' prints a small mark at the baseline; other notes print above."""
    n = len(systems)
    width = width or 0.8 / n
    import numpy as np
    xs = np.arange(len(corpora))
    floor = None
    for j, s in enumerate(systems):
        ys, notes = [], []
        for c in corpora:
            y, note = value(s, c)
            ys.append(y if y else float('nan')); notes.append(note or '')
        pos = xs - 0.4 + width * (j + 0.5)
        ax.bar(pos, ys, width=width * 0.92, **bar_kw(s), zorder=3)
        for p, y, note in zip(pos, ys, notes):
            if note:
                if y != y or y == 0:   # nan: missing cell
                    ax.text(p, 0, note, rotation=90, ha='center', va='bottom',
                            fontsize=5.2, color=GREY, transform=ax.get_xaxis_transform())
                else:
                    ax.text(p, y, note, rotation=90, ha='center', va='bottom',
                            fontsize=5.2, color='black')
            elif annotate and y == y:
                ax.text(p, y, annotate(y), rotation=90, ha='center', va='bottom',
                        fontsize=5.0, color='black')
    if log:
        ax.set_yscale('log')
    ax.set_xticks(xs)
    ax.set_xticklabels([corpus_label(c) for c in corpora], fontsize=7)
    ax.set_xlim(-0.55, len(corpora) - 0.45)
    ax.grid(axis='x', visible=False)
