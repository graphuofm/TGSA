#!/usr/bin/env python3
"""Regenerate every SIGMOD figure.

The per-figure sources live NEXT TO the PDFs in paper_SIGMOD/figures/
(fig_X.py regenerates fig_X.pdf; shared style in figstyle.py), so each
figure can be edited and rebuilt on its own. This script just runs them
all. The two TikZ figures (fig_arch, fig_format) are LaTeX sources in
paper_SIGMOD/ and compile with the paper itself.
"""
import os
import subprocess
import sys

FIGDIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      'paper_SIGMOD', 'figures')

def main():
    fail = 0
    for f in sorted(os.listdir(FIGDIR)):
        if f.startswith('fig_') and f.endswith('.py'):
            r = subprocess.run([sys.executable, f], cwd=FIGDIR)
            if r.returncode != 0:
                print('FAILED:', f)
                fail += 1
    sys.exit(1 if fail else 0)

if __name__ == '__main__':
    main()
