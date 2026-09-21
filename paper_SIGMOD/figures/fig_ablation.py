#!/usr/bin/env python3
"""fig_ablation.pdf -- regenerate with: python3 fig_ablation.py (edits welcome)."""
from figstyle import *

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


if __name__ == "__main__":
    fig_ablation()
    print("wrote", FIG + "fig_ablation.pdf")
