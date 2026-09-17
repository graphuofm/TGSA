#!/usr/bin/env python3
"""Figures for the SIGMOD submission (TGSR), generated from recorded CSVs.

Same accessibility discipline as the FAST set: Okabe-Ito palette, every
series redundantly encoded (colour AND marker AND linestyle; bars colour AND
hatch), no text below ~6.5pt at final size.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch, FancyArrowPatch

R = '/home/jding/Barry/results/'
FIG = '/home/jding/Barry/paper_SIGMOD/figures/'
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

C = dict(sys='#000000', tcsr='#0072B2', ucsr='#E69F00', csr='#009E73',
         duckdb='#D55E00', tva='#CC79A7', lg='#E69F00', go='#56B4E9',
         rocks='#D55E00', grey='#555555')

def med(v):
    v = sorted(v); n = len(v)
    return v[n//2] if n % 2 else 0.5*(v[n//2-1]+v[n//2])

def load(path):
    return list(csv.DictReader(open(R + path)))

SHORT = {'sx-askubuntu': 'askubuntu', 'wiki-talk-temporal': 'wiki-talk',
         'cit-Patents': 'cit-Patents', 'tc-cadets-e3': 'CADETS',
         'tc-theia-e3': 'THEIA', 'lanl-auth': 'LANL',
         'sx-mathoverflow': 'mathoverflow', 'sx-superuser': 'superuser',
         'email-Eu-core-temporal': 'email-Eu', 'CollegeMsg': 'CollegeMsg',
         'act-mooc': 'act-mooc', 'sx-stackoverflow': 'stackoverflow',
         'soc-LiveJournal1': 'LiveJournal', 'bitcoin-temporal': 'bitcoin'}

# =====================================================================
# Fig. 1 (motivation): the CADETS attack chain on a timeline, retention
# closing in from the left. Drawn, not measured; every element is from the
# published ground truth narrative.
# =====================================================================
def fig_motivation():
    fig, ax = plt.subplots(figsize=(3.34, 1.42))
    ax.set_xlim(0, 11); ax.set_ylim(-0.15, 1.18)
    ax.axis('off')

    # timeline arrow
    ax.annotate('', xy=(10.9, 0.0), xytext=(0.1, 0.0),
                arrowprops=dict(arrowstyle='-|>', lw=1.1, color='#000000'))
    ax.text(10.85, -0.14, 'time (11 days)', ha='right', fontsize=6.3,
            style='italic')

    # retention horizon sweeping from the left
    ax.axvspan(0.1, 2.6, ymin=0.08, ymax=0.98, color='#000000', alpha=0.08,
               zorder=0)
    ax.annotate('', xy=(3.15, 0.94), xytext=(2.0, 0.94),
                arrowprops=dict(arrowstyle='-|>', lw=0.9, color='#777777'))
    ax.text(1.35, 1.035, 'retention horizon\n(evidence deleted)', ha='center',
            fontsize=5.9, color='#555555', style='italic')

    # background event ticks: the stream the chain hides in
    import random
    random.seed(7)
    for x in [random.uniform(0.3, 10.6) for _ in range(130)]:
        ax.plot([x, x], [0.02, 0.10], color='#aaaaaa', lw=0.45, zorder=1)

    # the attack chain: time-respecting path. Labels alternate above/below
    # their node so none can touch the query arrows drawn at 0.24 and 0.86.
    chain = [(3.4, 'implant\ndrop'), (5.1, 'privilege\nescalation'),
             (6.9, 'C\\&C\ntraffic'), (8.6, 'process\ninjection'),
             (10.1, 'alert')]
    ys = [0.42, 0.62, 0.42, 0.62, 0.50]
    above = [True, False, True, False, True]
    for (x, lbl), y, up in zip(chain, ys, above):
        last = (lbl == 'alert')
        ax.scatter([x], [y], s=46 if last else 30,
                   c='#D55E00' if last else '#0072B2',
                   marker='*' if last else 'o', zorder=4,
                   edgecolors='white', linewidths=0.5)
        ax.plot([x, x], [0.02, y - 0.045], color='#0072B2', lw=0.5,
                ls=(0, (1, 1)), zorder=2)
        ty = y + 0.115 if up else y - 0.245
        ax.text(x, ty, lbl.replace('\\&', '&'), ha='center', fontsize=5.8,
                linespacing=0.95, zorder=5,
                bbox=dict(boxstyle='square,pad=0.05', fc='white', ec='none',
                          alpha=0.75))
    for (x0, _), y0, (x1, _), y1 in zip(chain, ys, chain[1:], ys[1:]):
        ax.add_patch(FancyArrowPatch((x0 + 0.13, y0), (x1 - 0.13, y1),
                     arrowstyle='-|>', mutation_scale=7, lw=0.9,
                     color='#0072B2', zorder=3,
                     connectionstyle='arc3,rad=-0.18'))

    # the two queries
    ax.annotate('', xy=(3.6, 0.24), xytext=(9.9, 0.24),
                arrowprops=dict(arrowstyle='-|>', lw=0.8, color='#009E73',
                                linestyle=(0, (3, 2))))
    ax.text(6.7, 0.145, 'backward: what caused the alert?', ha='center',
            fontsize=5.9, color='#007a58')
    ax.annotate('', xy=(10.3, 0.90), xytext=(3.6, 0.90),
                arrowprops=dict(arrowstyle='-|>', lw=0.8, color='#D55E00',
                                linestyle=(0, (3, 2))))
    ax.text(7.2, 0.955, 'forward: what did the implant touch?', ha='center',
            fontsize=5.9, color='#b34700')
    fig.tight_layout(pad=0.25)
    fig.savefig(FIG + 'fig_motivation.pdf'); plt.close(fig)

# =====================================================================
# Fig. ingest: (a) streaming ingestion bars incl. all baseline categories
#              (b) answers silently lost during a static rebuild
# =====================================================================
def fig_ingest():
    fig, (a, b) = plt.subplots(1, 2, figsize=(7.0, 1.95),
                               gridspec_kw={'width_ratios': [1.65, 1]})
    bars = [
        ('TVA',        1447,    C['tva'],  '+++', 'versioned store (DNF bound)'),
        ('RocksDB',    2.25e6,  C['rocks'], '...', 'KV, time-encoded key'),
        ('LiveGraph',  5.08e6,  C['lg'],   '///', 'streaming stores'),
        ('GraphOne',  24.27e6,  C['lg'],   '///', None),
        ('fsync/batch', 1.11e6, C['grey'], 'xxx', '\\sys durable (WAL)'.replace('\\sys', 'TGSR')),
        ('write-only WAL', 28.8e6, C['grey'], 'xxx', None),
        ('wiki-talk', 73.2e6,  '#000000', '',    'TGSR volatile'),
        ('CADETS',    85.4e6,  '#000000', '',    None),
        ('LANL',      77.5e6,  '#000000', '',    None),
        ('THEIA',     94.6e6,  '#000000', '',    None),
    ]
    for i, (lbl, v, col, hat, _) in enumerate(bars):
        a.bar(i, v, width=0.66, color=col, hatch=hat,
              edgecolor='white', linewidth=0.7, zorder=3)
        a.text(i, v * 1.6, (f"{v/1e6:.1f}M" if v >= 1e6 else f"$\\leq${v:,.0f}"),
               ha='center', fontsize=6.2, zorder=4)
    a.set_yscale('log'); a.set_ylim(300, 6e9)
    a.set_ylabel('events / s ingested', fontsize=7.5)
    a.tick_params(axis='y', labelsize=6.5)
    a.set_xticks(range(len(bars)))
    a.set_xticklabels([x[0] for x in bars], fontsize=6.2,
                      rotation=38, ha='right', rotation_mode='anchor')
    a.set_xlim(-0.75, len(bars) - 0.25)
    a.legend(handles=[Patch(facecolor=c_, hatch=h_, edgecolor='white', label=l_)
                      for _, _, c_, h_, l_ in bars if l_],
             frameon=False, loc='upper left', fontsize=6.0, ncol=2,
             handlelength=1.1, handletextpad=0.3, borderpad=0.1,
             columnspacing=0.8, labelspacing=0.22, bbox_to_anchor=(-0.02, 1.05))
    a.text(0.0, 1.8e4, 'DNF in\n90 min', fontsize=5.9, style='italic',
           ha='center', color='#555555')
    a.set_title('(a) Streaming ingestion, eight threads (baselines: wiki-talk)',
                loc='left', fontsize=8)

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
    b.bar([2.0], [0.6], width=0.5, color='#009E73', edgecolor='white',
          linewidth=0.7, zorder=3)
    b.text(2.0, 5, '0%', ha='center', fontsize=7.5, fontweight='bold',
           color='#007a58')
    b.axhline(0, color='#000000', lw=1.2, zorder=5)
    b.set_xticks(x + [2.0])
    b.set_xticklabels(ds + ['TGSR\nany size\n(no rebuild)'], fontsize=6.2)
    b.set_ylabel('% of true answer\nsilently missing', fontsize=7.5)
    b.tick_params(axis='y', labelsize=6.5)
    b.set_ylim(-6, 138); b.set_xlim(-0.55, 2.5)
    b.legend(frameon=False, loc='upper center', ncol=2, columnspacing=1.0,
             handlelength=1.3, handletextpad=0.4, fontsize=6.8, borderpad=0.15)
    b.set_title('(b) Answers lost during a static rebuild', loc='left', fontsize=8)
    fig.tight_layout(pad=0.35, w_pad=1.8)
    fig.savefig(FIG + 'fig_ingest.pdf'); plt.close(fig)

# =====================================================================
# Fig. baselines: windowed neighborhoods across four systems, and GraphOne's
# read amplification -- the axis that separates the streaming designs.
# =====================================================================
def fig_baselines():
    fig, (a, b, c) = plt.subplots(1, 3, figsize=(7.0, 1.85),
                                  gridspec_kw={'width_ratios': [1.45, 0.85, 1]})
    corpora = ['sx-askubuntu', 'wiki-talk-temporal', 'cit-Patents', 'tc-cadets-e3']

    lg, tg_c, tg_u = {}, {}, {}
    for r in load('e19_livegraph/e19.csv'):
        d = r['dataset']
        if d not in corpora: continue
        v = float(r['ns_per_delivered_edge'])
        if r['system'] == 'livegraph' and r['mode'] == 'batch_loader':
            lg.setdefault(d, []).append(v)
        elif r['system'] == 'gnomon' and r['mode'] == 'compacted' and r['threads'] == '1':
            tg_c.setdefault(d, []).append(v)
        elif r['system'] == 'gnomon' and r['mode'] == 'uncompacted' and r['threads'] == '1':
            tg_u.setdefault(d, []).append(v)
    go, amp = {}, {}
    for r in load('e20_graphone/graphone.csv'):
        d = r['dataset']
        if d in corpora and r['window_ns_per_delivered']:
            go[d] = float(r['window_ns_per_delivered'])
            amp[d] = float(r['window_scanned']) / float(r['window_delivered'])
    # (a) the shared, checksummed window suite: LiveGraph / GraphOne / TGSR.
    # RocksDB is NOT in this panel because its query set is E24's own; it
    # gets panel (c), where both bars come from the same paired harness.
    series = [
        ('LiveGraph',        {d: med(v) for d, v in lg.items()},  C['lg'], '///'),
        ('GraphOne',         go,  C['go'],   '\\\\\\'),
        ('TGSR uncompacted', {d: med(v) for d, v in tg_u.items()}, '#888888', ''),
        ('TGSR compacted',   {d: med(v) for d, v in tg_c.items()}, '#000000', ''),
    ]
    W = 0.19
    for j, (lbl, vals, col, hat) in enumerate(series):
        xs = [i + (j - 1.5) * W for i in range(len(corpora))]
        ys = [vals.get(d, float('nan')) for d in corpora]
        a.bar(xs, ys, width=W, color=col, hatch=hat, edgecolor='white',
              linewidth=0.5, label=lbl, zorder=3)
    a.set_yscale('log'); a.set_ylim(1, 3e4)
    a.set_ylabel('ns per delivered edge')
    a.set_xticks(range(len(corpora)))
    a.set_xticklabels([SHORT[d] for d in corpora], fontsize=6.3)
    a.legend(frameon=False, fontsize=5.9, ncol=2, loc='upper left',
             handlelength=1.1, handletextpad=0.3, columnspacing=0.8,
             labelspacing=0.25, borderpad=0.1)
    a.set_title('(a) Windowed neighborhoods', loc='left', fontsize=8)

    # (b) read amplification on that suite
    xs = range(len(corpora))
    b.bar(xs, [amp.get(d, 0) for d in corpora], width=0.55, color=C['go'],
          hatch='\\\\\\', edgecolor='white', linewidth=0.6, zorder=3)
    b.axhline(1.0, color='#000000', lw=1.2, ls=(0, (1, 1)), zorder=4)
    b.text(-0.35, 0.62, 'TGSR $\\approx$ 1', fontsize=6.3, ha='left',
           style='italic')
    for i, d in enumerate(corpora):
        if d in amp:
            b.text(i, amp[d] * 1.15, f'{amp[d]:.0f}$\\times$', ha='center',
                   fontsize=6.6)
    b.set_yscale('log'); b.set_ylim(0.4, 300)
    b.set_ylabel('edges scanned /\nedge delivered')
    b.set_xticks(list(xs))
    b.set_xticklabels([SHORT[d] for d in corpora], fontsize=6.3,
                      rotation=24, ha='right', rotation_mode='anchor')
    b.set_title('(b) GraphOne read\namplification', loc='left', fontsize=8)

    # (c) entity expansion on a time-only key: E24's own paired runs,
    # identical delivered sets and checksums on both sides.
    rk, tg24 = {}, {}
    for r in load('e24_rocksdb/e24.csv'):
        d = r['dataset']
        if d in corpora and r['phase'] == 'neighbors':
            (rk if r['system'] == 'rocksdb' else tg24)[d] = \
                float(r['seconds']) * 1e3
    for j, (vals, col, hat, lbl) in enumerate([
            (rk, C['rocks'], '...', 'RocksDB, time-encoded key'),
            (tg24, '#000000', '', 'TGSR')]):
        c.bar([i + (j - 0.5) * 0.34 for i in range(len(corpora))],
              [vals.get(d, float('nan')) for d in corpora], width=0.34,
              color=col, hatch=hat, edgecolor='white', linewidth=0.5,
              label=lbl, zorder=3)
    for i, d in enumerate(corpora):
        if d in rk and d in tg24:
            c.text(i, rk[d] * 1.5, f'{rk[d]/tg24[d]:,.0f}$\\times$',
                   ha='center', fontsize=6.2, fontweight='bold')
    c.set_yscale('log'); c.set_ylim(0.2, 1e8)
    c.set_ylabel('total query time (ms)')
    c.set_xticks(range(len(corpora)))
    c.set_xticklabels([SHORT[d] for d in corpora], fontsize=6.3,
                      rotation=24, ha='right', rotation_mode='anchor')
    c.legend(frameon=False, fontsize=5.9, loc='upper left', handlelength=1.1,
             handletextpad=0.3, borderpad=0.1, labelspacing=0.25)
    c.set_title('(c) Neighbors on a KV store', loc='left', fontsize=8)
    fig.tight_layout(pad=0.35, w_pad=1.3)
    fig.savefig(FIG + 'fig_baselines.pdf'); plt.close(fig)

# =====================================================================
# Fig. recovery: (a) the durability cost spectrum; (b) replay curve to 1.024B
# =====================================================================
def fig_recovery():
    fig, (a, b) = plt.subplots(1, 2, figsize=(7.0, 1.75))
    modes = ['none', 'write_only', 'async_64MB', 'async_4MB', 'sync_4MB', 'fsync_batch']
    mlbl = ['no log', 'write-\nonly', 'async\n64MB', 'async\n4MB', 'sync\n4MB', 'fsync/\nbatch']
    style = {'wiki-talk-temporal': ('#000000', 'o', '-'),
             'cit-Patents': ('#0072B2', 's', '--'),
             'sx-stackoverflow': ('#E69F00', '^', '-.')}
    rows = load('e18_recovery/cost.csv')
    for ds, (col, mk, ls) in style.items():
        ys = []
        for m in modes:
            v = [float(r['ingest_Meps']) for r in rows
                 if r['dataset'] == ds and r['durability'] == m]
            ys.append(med(v) if v else float('nan'))
        a.plot(range(len(modes)), ys, marker=mk, ls=ls, color=col,
               label=SHORT[ds], zorder=3)
    a.set_yscale('log')
    a.set_xticks(range(len(modes))); a.set_xticklabels(mlbl, fontsize=6.4)
    a.set_ylabel('ingest (M events/s)')
    a.legend(frameon=False, fontsize=6.6, loc='upper right', handlelength=1.9,
             borderpad=0.15, labelspacing=0.3)
    a.text(0.02, 0.04, 'bounded by the device, not the engine',
           transform=a.transAxes, fontsize=6.2, style='italic', color='#555555')
    a.set_title('(a) The durability spectrum', loc='left', fontsize=8)

    rows = load('e18_recovery/curve.csv')
    xs = [int(r['edges']) for r in rows]
    wr = [float(r['write_Meps']) for r in rows]
    rp = [float(r['replay_Meps']) for r in rows]
    sec = [float(r['replay_sec']) for r in rows]
    b.plot(xs, rp, marker='o', ls='-', color='#000000', label='replay (recovery)', zorder=4)
    b.plot(xs, wr, marker='s', ls='--', color='#0072B2', label='log write (durable ingest)', zorder=3)
    b.set_xscale('log')
    b.set_ylim(0, 44)
    b.set_xlabel('logged edges (LANL prefix)')
    b.set_ylabel('M events/s')
    b.annotate(f'1.024B edges\nin {sec[-1]:.1f} s', xy=(xs[-1], rp[-1]),
               xytext=(0.97, 0.52), textcoords='axes fraction', ha='right',
               fontsize=6.8, arrowprops=dict(arrowstyle='->', lw=0.7,
                                             color='#333333'))
    b.legend(frameon=False, fontsize=6.6, loc='upper left', handlelength=1.9,
             borderpad=0.15, labelspacing=0.3)
    b.set_title('(b) Recovery is re-ingestion, flat to a billion edges',
                loc='left', fontsize=8)
    fig.tight_layout(pad=0.35, w_pad=1.6)
    fig.savefig(FIG + 'fig_recovery.pdf'); plt.close(fig)

# =====================================================================
# Fig. ablation: one panel per mechanism.
#   (a) run claiming: thread scaling with the switch on/off
#   (b) binary-search threshold: plateau then cliff
#   (c) timestamp cache: temporal queries both builds
#   (d) writers vs readers: the concurrency trade
# =====================================================================
def fig_ablation():
    fig, axes = plt.subplots(1, 4, figsize=(7.0, 1.62))
    a, b, c, d = axes

    # (a) run claiming
    for fn, ds, col in [('runclaim_wiki.csv', 'wiki-talk', '#000000'),
                        ('runclaim_cadets.csv', 'CADETS', '#0072B2')]:
        rows = load('e23_ablation/' + fn)
        for rc, ls, mk, fill in [('1', '-', 'o', True), ('0', ':', 'x', False)]:
            xs = [int(r['threads']) for r in rows if r['run_claim'] == rc]
            ys = [float(r['ingest_Meps']) for r in rows if r['run_claim'] == rc]
            a.plot(xs, ys, marker=mk, ls=ls, color=col,
                   mfc=col if fill else 'none', ms=3.6, lw=1.15, zorder=3)
    a.set_xscale('log', base=2); a.set_xticks([1, 2, 4, 8])
    a.set_xticklabels(['1', '2', '4', '8'])
    a.set_xlabel('writer threads'); a.set_ylabel('M events/s')
    a.legend(handles=[
        Line2D([], [], color='#000000', ls='-', marker='o', label='wiki-talk, on'),
        Line2D([], [], color='#000000', ls=':', marker='x', label='wiki-talk, off'),
        Line2D([], [], color='#0072B2', ls='-', marker='o', label='CADETS, on'),
        Line2D([], [], color='#0072B2', ls=':', marker='x', label='CADETS, off')],
        frameon=False, fontsize=5.6, loc='upper left', handlelength=1.7,
        borderpad=0.1, labelspacing=0.2)
    a.set_title('(a) Run claiming', loc='left', fontsize=8)

    # (b) threshold sweep, normalized to each corpus's plateau median
    rows = load('e23_ablation/threshold.csv')
    per = defaultdict(dict)
    for r in rows:
        per[r['dataset']][int(r['bs_min'])] = float(r['total_ms'])
    style = {'sx-askubuntu': ('#009E73', 'D', (0, (1, 1))),
             'wiki-talk-temporal': ('#000000', 'o', '-'),
             'cit-Patents': ('#E69F00', '^', '-.'),
             'tc-cadets-e3': ('#0072B2', 's', '--')}
    for ds, (col, mk, ls) in style.items():
        vals = per[ds]
        plateau = med([v for k, v in vals.items() if k <= 256])
        ks = sorted(vals)
        xs = list(range(len(ks)))
        b.plot(xs, [vals[k] / plateau for k in ks], marker=mk, ls=ls,
               color=col, label=SHORT[ds], ms=3.4, lw=1.1, zorder=3)
    ks = sorted(per['wiki-talk-temporal'])
    b.set_xticks(range(len(ks)))
    b.set_xticklabels([('off' if k > 1e6 else str(k)) for k in ks],
                      fontsize=5.6, rotation=35, ha='right',
                      rotation_mode='anchor')
    b.set_yscale('log')
    b.set_xlabel('binary-search threshold')
    b.set_ylabel('slowdown vs plateau')
    b.annotate('26$\\times$', xy=(7, per['tc-cadets-e3'][1000000000] /
               med([v for k, v in per['tc-cadets-e3'].items() if k <= 256])),
               xytext=(-14, -2), textcoords='offset points', fontsize=7,
               fontweight='bold')
    b.legend(frameon=False, fontsize=5.6, loc='upper left', handlelength=1.6,
             borderpad=0.1, labelspacing=0.2)
    b.set_title('(b) Search threshold', loc='left', fontsize=8)

    # (c) ts cache: ns per delivered edge, 16B vs 24B builds
    def collect(path):
        out = {}
        for r in load(path):
            if r['system'] == 'gnomon' and r['threads'] == '1':
                out[(r['dataset'], r['mode'])] = float(r['ns_per_delivered_edge'])
        return out
    b24 = collect('e19_livegraph/e19.csv')
    b16 = collect('e23_ablation/notscache_e19.csv')
    corp = ['wiki-talk-temporal', 'tc-cadets-e3', 'cit-Patents']
    xs = list(range(len(corp)))
    for j, (bld, col, hat, lbl) in enumerate([
            (b16, '#E69F00', '///', '16B ref (no ts)'),
            (b24, '#000000', '', '24B ref (ts cached)')]):
        ys = [bld.get((ds, 'uncompacted'), float('nan')) for ds in corp]
        c.bar([x + (j - 0.5) * 0.32 for x in xs], ys, width=0.32, color=col,
              hatch=hat, edgecolor='white', linewidth=0.5, label=lbl, zorder=3)
    for x, ds in zip(xs, corp):
        if (ds, 'uncompacted') in b16 and (ds, 'uncompacted') in b24:
            r = b16[(ds, 'uncompacted')] / b24[(ds, 'uncompacted')]
            c.text(x - 0.16, b16[(ds, 'uncompacted')] * 1.12,
                   f'{r:.1f}$\\times$', ha='center', fontsize=6.2)
    c.set_yscale('log')
    c.set_ylim(top=max(b16.get((ds, 'uncompacted'), 1) for ds in corp) * 12)
    c.set_xticks(xs)
    c.set_xticklabels([SHORT[ds] for ds in corp], fontsize=5.8,
                      rotation=20, ha='right', rotation_mode='anchor')
    c.set_ylabel('ns / delivered edge')
    c.legend(frameon=False, fontsize=5.6, loc='upper left', handlelength=1.1,
             handletextpad=0.3, borderpad=0.1, labelspacing=0.2)
    c.set_title('(c) Timestamp cache', loc='left', fontsize=8)

    # (d) R/W mix (e21 concurrent): ingest vs query throughput trade
    mixes = ['8:1', '8:2', '8:4', '8:8', '4:8', '2:8', '1:8']
    ing = [44.20, 39.33, 34.51, 29.60, 38.64, 27.81, 19.53]
    qps = [36799, 72616, 149568, 257347, 319155, 470180, 565769]
    d.plot(range(len(mixes)), ing, marker='o', ls='-', color='#000000',
           label='ingest (M events/s)', zorder=3)
    d.set_ylabel('M events/s'); d.set_ylim(0, 62)
    d2 = d.twinx()
    d2.plot(range(len(mixes)), [q/1000 for q in qps], marker='s', ls='--',
            color='#D55E00', label='queries (K/s)', zorder=3)
    d2.set_ylabel('K queries/s', color='#b34700', fontsize=7.5)
    d2.tick_params(axis='y', labelsize=6.5, colors='#b34700')
    d2.spines['right'].set_visible(True)
    d2.spines['right'].set_color('#D55E00')
    d2.grid(False)
    d.set_xticks(range(len(mixes)))
    d.set_xticklabels(mixes, fontsize=6.4)
    d.set_xlabel('writers : readers')
    d.legend(handles=[
        Line2D([], [], color='#000000', ls='-', marker='o', label='ingest (M/s)'),
        Line2D([], [], color='#D55E00', ls='--', marker='s', label='queries (K/s)')],
        frameon=False, fontsize=5.6, loc='upper left', handlelength=1.7,
        borderpad=0.1, labelspacing=0.2)
    d.set_title('(d) Mixed load', loc='left', fontsize=8)

    fig.tight_layout(pad=0.35, w_pad=1.05)
    fig.savefig(FIG + 'fig_ablation.pdf'); plt.close(fig)

# =====================================================================
# Fig. pipeline: E22 extract -> compute -> writeback, three systems
# =====================================================================
def fig_pipeline():
    fig, (a, b) = plt.subplots(1, 2, figsize=(7.0, 1.7))
    rows = load('e22_pipeline/e22.csv')
    ds = 'wiki-talk-temporal'
    sysstyle = [('gnomon', '#000000', 'o', '-', 'TGSR'),
                ('duckdb', C['duckdb'], 'v', (0, (3, 1, 1, 1)), 'DuckDB'),
                ('livegraph', C['lg'], '^', '-.', 'LiveGraph')]
    for s, col, mk, ls, lbl in sysstyle:
        pts = sorted([(float(r['window_frac']), float(r['total_sec']))
                      for r in rows if r['system'] == s and r['dataset'] == ds])
        a.plot([p[0] for p in pts], [p[1] for p in pts], marker=mk, ls=ls,
               color=col, label=lbl, zorder=3)
    a.set_xscale('log'); a.set_yscale('log')
    a.set_xlabel('window (fraction of span)')
    a.set_ylabel('extract+compute+\nwriteback (s)')
    a.legend(frameon=False, fontsize=6.4, loc='upper left', handlelength=1.9,
             borderpad=0.15, labelspacing=0.3)
    a.set_title('(a) Windowed analytics pipeline (wiki-talk)', loc='left',
                fontsize=8)

    # (b) extract share of the pipeline: where each system's time goes
    for s, col, mk, ls, lbl in sysstyle:
        pts = sorted([(float(r['window_frac']), 100*float(r['extract_share']))
                      for r in rows if r['system'] == s and r['dataset'] == ds])
        b.plot([p[0] for p in pts], [p[1] for p in pts], marker=mk, ls=ls,
               color=col, label=lbl, zorder=3)
    b.set_xscale('log'); b.set_ylim(0, 105)
    b.set_xlabel('window (fraction of span)')
    b.set_ylabel('extraction share of\npipeline time (%)')
    b.text(0.03, 0.06, 'extraction dominates their loop, not ours',
           transform=b.transAxes, fontsize=6.2, style='italic', color='#555555')
    b.set_title('(b) Where the pipeline time goes', loc='left', fontsize=8)
    fig.tight_layout(pad=0.35, w_pad=1.6)
    fig.savefig(FIG + 'fig_pipeline.pdf'); plt.close(fig)

fig_motivation(); fig_ingest(); fig_baselines(); fig_recovery()
fig_ablation(); fig_pipeline()
print('figures written to', FIG)
