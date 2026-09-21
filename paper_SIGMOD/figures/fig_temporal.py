#!/usr/bin/env python3
"""fig_temporal.pdf -- regenerate with: python3 fig_temporal.py (edits welcome)."""
from figstyle import *

def fig_temporal():
    fig, (a, b, c) = plt.subplots(1, 3, figsize=(7.0, 1.58))

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
    a.set_ylim(0.04, 3e5)
    a.set_xlabel('query window (fraction of span)')
    a.set_ylabel('median latency (µs)')
    a.set_title('(a) Time-respecting traversal', loc='left', fontsize=8)
    a.legend(handles=[Line2D([], [], color=STYLE[s]['c'], marker=STYLE[s]['m'],
                             ls=STYLE[s]['ls'], label=STYLE[s]['lbl'])
                      for s in ['duckdb', 'barry', 'tcsr']],
             frameon=False, loc='upper left', handlelength=2.0, borderpad=0.1,
             fontsize=6.2, labelspacing=0.25, bbox_to_anchor=(-0.02, 1.04))

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


if __name__ == "__main__":
    fig_temporal()
    print("wrote", FIG + "fig_temporal.pdf")
