#!/usr/bin/env python3
"""fig_ingest.pdf -- regenerate with: python3 fig_ingest.py (edits welcome)."""
from figstyle import *

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


if __name__ == "__main__":
    fig_ingest()
    print("wrote", FIG + "fig_ingest.pdf")
