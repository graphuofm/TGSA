#!/usr/bin/env python3
"""fig_baselines.pdf -- regenerate with: python3 fig_baselines.py (edits welcome)."""
from figstyle import *

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


if __name__ == "__main__":
    fig_baselines()
    print("wrote", FIG + "fig_baselines.pdf")
