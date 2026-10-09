#!/usr/bin/env python3
"""Produce a single-file main.tex for dropping into an existing Overleaf
project, by inlining the \\input{...} siblings that the project may not
have yet.

The repository keeps the figures as separate .tex files; an Overleaf
project that was uploaded before a figure was split out would break on
a bare main.tex replacement. This writes overleaf-main.tex, which has
those files inlined, so replacing the single file is enough.

Usage: python3 scripts/flatten_main.py [name ...]   (default: fig_overview)
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    'paper_SIGMOD')
names = sys.argv[1:] or ['fig_overview']

src = open(os.path.join(ROOT, 'main.tex')).read()
for n in names:
    body = open(os.path.join(ROOT, n + '.tex')).read().rstrip()
    pat = '\\input{%s}' % n
    if pat not in src:
        sys.exit('no \\input for %s in main.tex' % n)
    src = src.replace(pat,
                      '%% ---- inlined from %s.tex ----\n%s\n%% ---- end %s.tex ----'
                      % (n, body, n), 1)
    print('inlined', n + '.tex')

# Written as main.tex inside its own folder so it can be dragged
# straight into Overleaf, where the filename decides what it replaces.
dest = os.path.join(os.path.dirname(ROOT), 'overleaf-upload')
os.makedirs(dest, exist_ok=True)
out = os.path.join(dest, 'main.tex')
open(out, 'w').write(src)
print('wrote', out)
