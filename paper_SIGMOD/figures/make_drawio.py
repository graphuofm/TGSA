#!/usr/bin/env python3
"""Emit editable .drawio sources for the four hand-drawn figures.

The data charts in this directory are generated from CSVs and have .py
sources; a drawio file would be the wrong thing for them. The four
figures below are drawings, so each gets a same-named .drawio that
opens in diagrams.net and can be rearranged and re-exported to PDF.

The originals remain what the paper compiles: fig_motivation.py
(matplotlib) and ../fig_overview.tex, ../fig_arch.tex (whose Panel C is
the formats strip), ../fig_runclaim.tex (TikZ). When you change a drawio and want it in the
paper, export to PDF here and swap the \\input for an \\includegraphics,
or mirror the change back into the source.

Geometry is 1:1 with the paper at 96 dpi: 37.8 px per TikZ cm, so
\\scriptsize (7pt) is fontSize 9 and \\footnotesize (8pt) is fontSize 11.
Every position below is written in the source's own coordinates (cm for
TikZ, data units for matplotlib) and mapped through a per-figure X()/Y(),
so a change to a .tex coordinate is a one-number change here.

Run: python3 make_drawio.py
"""
import html
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
CM = 37.8                     # px per cm
MM = CM / 10

# palette, matching the paper's macros and figstyle.py
SYSC = '#0072B2'                                  # TGSA and its structures (blue)
QWIN, QNBR, WINS = '#009E73', '#009E73', '#D55E00'  # queries (green), insert path (vermillion)
GREY, LIGHT = '#555555', '#aaaaaa'
BLUE, GREEN, YELLOW, BLACK = '#0000FF', '#00FF00', '#FFFF00', '#000000'

def tint(hex_, pct):
    """TikZ `<color>!<pct>`: pct % of the color mixed on white."""
    r, g, b = (int(hex_[i:i + 2], 16) for i in (1, 3, 5))
    f = pct / 100
    return '#%02X%02X%02X' % tuple(round(255 - f * (255 - c)) for c in (r, g, b))

HDRC  = tint(SYSC, 30)        # block / bucket header (sysc!30)
EDGEC = tint(SYSC, 12)        # arena edge (sysc!12)
REFC  = '#FFFFFF'             # EdgeRef (white)
PANEL = tint(SYSC, 5)         # panel / group background (sysc!5)

# ------------------------------------------------------------ primitives
def _n(v):
    return f'{v:.1f}'.rstrip('0').rstrip('.')

def cell(cid, value, style, x, y, w, h, parent='1'):
    return (f'        <mxCell id="{cid}" value="{html.escape(value)}" '
            f'style="{style}" vertex="1" parent="{parent}">\n'
            f'          <mxGeometry x="{_n(x)}" y="{_n(y)}" width="{_n(w)}" '
            f'height="{_n(h)}" as="geometry"/>\n'
            f'        </mxCell>\n')

def edge(cid, style, sx, sy, tx, ty, points=()):
    pts = ''
    if points:
        pts = ('            <Array as="points">\n'
               + ''.join(f'              <mxPoint x="{_n(px)}" y="{_n(py)}"/>\n'
                         for px, py in points)
               + '            </Array>\n')
    return (f'        <mxCell id="{cid}" value="" style="{style}" edge="1" parent="1">\n'
            f'          <mxGeometry relative="1" as="geometry">\n'
            f'            <mxPoint x="{_n(sx)}" y="{_n(sy)}" as="sourcePoint"/>\n'
            f'            <mxPoint x="{_n(tx)}" y="{_n(ty)}" as="targetPoint"/>\n'
            + pts +
            f'          </mxGeometry>\n'
            f'        </mxCell>\n')

TEXT   = 'text;html=1;verticalAlign=middle;spacing=0;'
ARROW  = 'endArrow=block;endFill=1;html=1;rounded=0;'             # TikZ Stealth
DARROW = 'startArrow=block;startFill=1;endArrow=block;endFill=1;html=1;rounded=0;'
LINE   = 'endArrow=none;html=1;rounded=0;'

# anchor -> (align, x fraction, y fraction) of the geometry at the anchor point
_ANCHOR = {'w': ('left', 0, .5), 'e': ('right', 1, .5), 'c': ('center', .5, .5),
           'n': ('center', .5, 0), 's': ('center', .5, 1),
           'nw': ('left', 0, 0), 'ne': ('right', 1, 0),
           'sw': ('left', 0, 1), 'se': ('right', 1, 1)}

def text(cid, value, x, y, w, h=14, anchor='w', fs=9, color=None,
         bold=False, mono=False, extra=''):
    """Text node; (x, y) is the TikZ anchor point in px, like node[anchor=..]."""
    align, ax, ay = _ANCHOR[anchor]
    style = TEXT + f'align={align};fontSize={fs};'
    if color:
        style += f'fontColor={color};'
    if bold:
        style += 'fontStyle=1;'
    if mono:
        style += 'fontFamily=Courier New;'
    return cell(cid, value, style + extra, x - ax * w, y - ay * h, w, h)

def box(cid, value, cx, cy, w, h, fill='#FFFFFF', stroke='#666666', fs=9,
        color=None, rounded=False, align='center', extra=''):
    """Bordered node centred at (cx, cy) in px, like node[draw] at (cx, cy)."""
    style = ('rounded=1;absoluteArcSize=1;arcSize=3;' if rounded else 'rounded=0;')
    style += (f'html=1;align={align};verticalAlign=middle;fontSize={fs};'
              f'fillColor={fill};strokeColor={stroke};')
    if color:
        style += f'fontColor={color};'
    return cell(cid, value, style + extra, cx - w / 2, cy - h / 2, w, h)

def step(cid, n, color, cx, cy, d=11):
    """Numbered step circle (TikZ wstep / bstep / gstep): white digit on color."""
    return cell(cid, str(n),
                f'ellipse;html=1;fillColor={color};strokeColor=none;fontColor=#FFFFFF;'
                'fontSize=9;fontStyle=1;align=center;verticalAlign=middle;spacing=0;',
                cx - d / 2, cy - d / 2, d, d)

def steptext(cid, n, color, value, x, y, w, h=14):
    """`\\stepc{..}{n}\\;text`, anchored west at (x, y)."""
    return (step(cid + '_c', n, color, x + 5.5, y)
            + text(cid, value, x + 15, y, w, h, 'w', 9, color))

# inline markup (draw.io labels are HTML)
def tt(s):  return f'<font face="Courier New">{s}</font>'       # \texttt
def sm(s):  return f'<font style="font-size: 9px">{s}</font>'   # \scriptsize inside \footnotesize
def sub(s): return f'<sub>{s}</sub>'
def sup(s): return f'<sup>{s}</sup>'
def it(s):  return f'<i>{s}</i>'
def bf(s):  return f'<b>{s}</b>'

def write(name, w, h, body):
    xml = ('<mxfile host="app.diagrams.net" type="device">\n'
           f'  <diagram id="{name}" name="{name}">\n'
           f'    <mxGraphModel dx="1400" dy="800" grid="1" gridSize="10" '
           f'guides="1" tooltips="1" connect="1" arrows="1" fold="1" page="1" '
           f'pageScale="1" pageWidth="{w}" pageHeight="{h}" math="0" shadow="0">\n'
           '      <root>\n        <mxCell id="0"/>\n'
           '        <mxCell id="1" parent="0"/>\n'
           + body +
           '      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')
    path = os.path.join(HERE, name + '.drawio')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(xml)
    print('wrote', os.path.relpath(path))

# ===================================================== fig_overview
def overview():
    """../fig_overview.tex: engine components and the paths between them."""
    X = lambda x: (x + 0.85) * CM
    Y = lambda y: (4.80 - y) * CM
    comp = dict(fill=tint(SYSC, 8), stroke=SYSC, fs=11, rounded=True)   # comp/.style
    b = ''
    # groups first so they sit behind their components
    b += box('qe', '', X(6.3), Y(2.45), 40 * MM, 21 * MM, PANEL, tint(SYSC, 70), rounded=True)
    b += box('se', '', X(4.15), Y(0.25), 83 * MM, 16 * MM, PANEL, tint(SYSC, 70), rounded=True)
    b += text('qet', f'{bf("Query engine")} (§5)', X(6.3), Y(3.45), 120, 16, 'n', 11)
    b += text('set', f'{bf("Storage engine")} (§4)', X(4.15), Y(1.0), 130, 16, 'n', 11)
    # inputs
    b += box('stream', 'event stream', X(1.75), Y(4.25), 66, 6.5 * MM,
             '#FFFFFF', tint(BLACK, 65), 11)
    b += box('query', 'temporal queries Q1–Q4', X(6.3), Y(4.25), 118, 6.5 * MM,
             '#FFFFFF', QWIN, 11, QWIN)
    # ingest path
    b += box('ingest', f'{bf("Ingest path")} (§4.2)<br>{sm("shift, claim run, publish")}',
             X(1.75), Y(3.05), 32 * MM, 9 * MM, tint(SYSC, 8), WINS, 11, rounded=True,
             extra='whiteSpace=wrap;')
    b += edge('a_in', ARROW + f'strokeColor={WINS};', X(1.75), Y(4.25 - 0.325), X(1.75), Y(3.05 + 0.45))
    # query engine
    for cid, x, y, lbl, sec in (('qw', 5.35, 2.55, 'Q1 window', '§5.1'),
                                ('qx', 7.25, 2.55, 'Q2 neighbors', '§5.2'),
                                ('qt', 5.35, 1.72, 'Q3 traversal', '§5.3'),
                                ('qa', 7.25, 1.72, 'Q4 as-of', '§5.2')):
        b += box(cid, f'{lbl}<br>{sm(sec)}', X(x), Y(y), 17 * MM, 7 * MM, **comp)
    b += edge('a_q', ARROW + f'strokeColor={QWIN};', X(6.3), Y(4.25 - 0.325), X(6.3), Y(3.5))
    # storage engine
    for cid, x, lbl, sec in (('tl', 1.05, 'timeline', '§4.1'), ('ar', 3.1, 'arena', '§4.1'),
                             ('vi', 5.2, 'vertex index', '§4.3'), ('wal', 7.25, 'log', '§4.4')):
        b += box(cid, f'{lbl}<br>{sm(sec)}', X(x), Y(0.05), 18 * MM, 7 * MM, **comp)
    b += edge('a_is', ARROW + f'strokeColor={WINS};', X(1.75), Y(2.6), X(1.75), Y(1.05))
    b += edge('a_qs', ARROW + f'strokeColor={QNBR};', X(6.3), Y(1.4), X(6.3), Y(1.05))
    # lifecycle operators
    b += box('ret', f'{bf("Retention")} (§4.4)<br>{sm("release an address prefix")}',
             X(1.75), Y(-1.25), 31 * MM, 9 * MM, tint(SYSC, 8), tint(BLACK, 55), 11, rounded=True,
             extra='whiteSpace=wrap;')
    b += box('rec', f'{bf("Recovery")} (§4.4)<br>{sm("replay the log through ingest")}',
             X(6.55), Y(-1.25), 31 * MM, 9 * MM, tint(SYSC, 8), tint(BLACK, 55), 11, rounded=True,
             extra='whiteSpace=wrap;')
    b += edge('a_ret', ARROW, X(1.75), Y(-0.8), X(1.75), Y(-0.55))
    b += edge('a_rec', ARROW, X(6.55), Y(-0.8), X(6.55), Y(-0.55))
    # the abstraction underneath
    b += text('sub', 'every structure above is placed by Eq. (2): the timestamp is the address (§3.1)',
              X(4.15), Y(-1.95), 380, 14, 'n', 9, tint(BLACK, 70))
    write('fig_overview', 380, 280, b)

# ===================================================== fig_arch
def arch():
    """../fig_arch.tex: three stacked panels; Q1 and Q2 drawn on the structures, formats below."""
    X = lambda x: (x + 0.45) * CM
    Y = lambda y: (9.85 - y) * CM
    note, grey = tint(BLACK, 65), tint(BLACK, 60)
    cst = dict(stroke=tint(BLACK, 55))                 # cell/.style border
    pair = lambda v, t: f'({it("v")}{sub(v)},{it("t")}{sub(t)})'   # $(v_3,t_{17})$
    ref = lambda v, t: f'{it("v")}{sub(v)} | {it("t")}{sub(t)}'    # $v_9\,|\,t_{18}$
    b = ''
    # ---- panels (behind everything)
    for pid, y0, hmm in (('pA', 9.7, 46), ('pB', 4.75, 45), ('pC', -0.1, 27)):
        b += cell(pid, '', f'rounded=1;absoluteArcSize=1;arcSize=3;html=1;'
                  f'fillColor={PANEL};strokeColor={tint(SYSC, 70)};', X(-0.25), Y(y0), 176 * MM, hmm * MM)
    b += text('pAt', f'{bf("Time-keyed storage")} (§4.1, §4.2): {tt("Timeline")} = '
              f'{tt("tmap")}⟨block ptr⟩ and {tt("Arena")} of 20 B edges',
              X(-0.1), Y(9.65), 540, 16, 'nw', 11)
    b += text('pBt', f'{bf("Vertex index")} (§4.3): per-vertex append chains of 24 B '
              f'references, sorted only by {tt("compact()")}',
              X(-0.1), Y(4.7), 560, 16, 'nw', 11)

    # ======================= Panel A: time-keyed storage
    # stream and the insert path
    b += box('stream', f'event stream, never ends<br>{it("e")} = ({it("src")}, {it("dst")}, '
             f'{it("t")}, {it("w")}), immutable',
             X(1.55), Y(8.35), 148, 32, 'none', grey, 9, align='left', extra='spacingLeft=4;')
    b += text('arn', 'one anonymous reservation;<br>arena: 20 B packed edges,<br>never sorted on insert',
              X(-0.05), Y(7.45), 160, 36, 'w', 9, note)
    b += steptext('st2', 2, WINS, f'one {tt("fetch_add")} claims the run', X(-0.05), Y(6.5), 170)
    b += steptext('st3', 3, WINS, f'payload first, {it("t")} last<br>(release, T5)',
                  X(-0.05), Y(5.95), 150, 26)
    # insert step 1: the address computation
    b += steptext('eq', 1, WINS, f'{it("A")}({it("T")}) = {it("A")}{sub("base")} + '
                  f'(({it("T")} − {it("T")}{sub("0")}) ≫ {it("S")}) × {it("P")}',
                  X(3.55), Y(8.75), 200)
    b += edge('a_eq', ARROW + f'strokeColor={WINS};', X(1.55) + 74, Y(8.35), X(3.6), Y(8.5))
    # timeline slot array
    fills = {0: tint(BLACK, 25), 1: tint(BLACK, 25), 2: tint(BLACK, 25),
             4: '#FFFFFF', 5: tint(WINS, 40), 7: '#FFFFFF', 10: '#FFFFFF'}
    for i in range(12):
        b += box(f's{i}', '0' if i in (4, 7, 10) else '', X(4.1 + i * 0.72), Y(7.75),
                 7 * MM, 5.2 * MM, fills.get(i, tint(BLACK, 10)), **cst)
    b += text('wl', f'W{sub("0")} .. W{sub("11")}, 2{sup(it("S"))} ticks each',
              X(12.55), Y(7.95), 170, 14, 'w', 9, mono=True)
    b += text('t2', '0 = never faulted, costs nothing (T2)', X(12.55), Y(7.55), 190, 14, 'w', 9, note)
    b += edge('evl', LINE + f'strokeColor={tint(BLACK, 50)};', X(3.75), Y(7.49), X(5.89), Y(7.49))
    b += text('evt', f'evicted prefix: {tt("madvise")} (T3)', X(4.82), Y(7.42), 140, 14, 'n', 9, note)
    b += edge('a_s5', ARROW + f'strokeColor={WINS};', X(5.35), Y(8.55), X(7.7), Y(8.01))
    # arena blocks: header then edge cells chained to the right
    for bid, x0, evs in (('b1', 4.3, ((3, 17), (9, 18), (3, 21))),
                         ('b2', 9.0, ((1, 40), (3, 41), (8, 42))),
                         ('b3', 14.25, ((3, 44),))):
        b += box(f'{bid}h', 'cnt 4<br>flags', X(x0), Y(6.35), 11 * MM, 5.2 * MM, HDRC, **cst)
        x = x0 + 0.55
        for k, (v, t) in enumerate(evs):
            fill = tint(WINS, 25) if (bid, k) == ('b2', 2) else EDGEC
            b += box(f'{bid}e{k}', pair(v, t), X(x + 0.575), Y(6.35), 11.5 * MM, 5.2 * MM, fill, **cst)
            x += 1.15
    b += box('b3e1', f'{it("t")}=0', X(16.35), Y(6.35), 8 * MM, 5.2 * MM, EDGEC, **cst)
    b += edge('a_b1', ARROW + f'strokeColor={grey};', X(6.26), Y(7.49), X(6.575), Y(6.61))
    b += edge('a_b2', ARROW + f'strokeColor={grey};', X(7.7), Y(7.49), X(10.125), Y(6.61))
    b += edge('a_cas', ARROW + f'strokeColor={grey};dashed=1;', X(13.0), Y(6.35), X(13.7), Y(6.35))
    b += text('cas', 'CAS chain', X(13.35), Y(6.29), 60, 14, 'n', 9, note)
    # Q1 on the timeline
    b += box('q1', f'{bf("Q1")} “all events in [{it("t")}{sub("1")}, {it("t")}{sub("2")}]”<br>'
             'touches only the timeline',
             X(15.55), Y(8.75), 132, 32, 'none', QWIN, 9, QWIN, align='left',
             extra='strokeWidth=2;spacingLeft=4;')
    b += edge('q1w', DARROW + f'strokeColor={QWIN};strokeWidth=2;', X(8.07), Y(8.13), X(10.93), Y(8.13))
    b += steptext('q1s1', 1, QWIN, f'map {it("t")}{sub("1")}, {it("t")}{sub("2")}', X(9.3), Y(8.75), 50)
    b += steptext('q1s2', 2, QWIN, f'walk W{sub("6")} .. W{sub("9")}', X(9.3) + 72, Y(8.75), 62)
    b += edge('a_q1', ARROW + f'strokeColor={QWIN};', X(15.55) - 66, Y(8.75), X(9.3) + 152, Y(8.75))
    b += steptext('q1s3', 3, QWIN, 'interior blocks copied comparison-free', X(12.3), Y(5.55), 175)
    b += edge('a_q1b', ARROW + f'strokeColor={QWIN};curved=1;', X(9.86), Y(7.49), X(15.375), Y(6.61),
              [(X(12.64), Y(7.29))])                    # to[bend left=10]

    # ======================= Panel B: vertex index
    for i in range(1, 5):
        b += box(f'h{i}', f'{it("v")}{sub(i)}', X(0.45 + i * 0.8), Y(3.75), 8 * MM, 5.2 * MM,
                 tint(QNBR, 30) if i == 3 else tint(SYSC, 12), **cst)
    b += text('hn', 'heads, one per vertex', X(4.05), Y(3.35), 150, 14, 'w', 9, mono=True)
    # bucket 1: sorted prefix
    b += box('k1', f'cnt 3<br>{bf("sorted 1")}', X(1.1), Y(2.75), 13 * MM, 5.2 * MM, HDRC, **cst)
    for k, (v, t) in enumerate(((9, 18), (3, 21), (1, 40))):
        b += box(f'r{k + 1}', ref(v, t), X(2.45 + k * 1.4), Y(2.75), 14 * MM, 5.2 * MM, REFC, **cst)
    # bucket 2: arrival-order tail
    b += box('k2', f'cnt 2<br>{bf("sorted 0")}', X(1.1), Y(1.6), 13 * MM, 5.2 * MM, HDRC, **cst)
    b += box('r4', ref(3, 44), X(2.45), Y(1.6), 14 * MM, 5.2 * MM, REFC, **cst)
    b += box('r5', 'appending…', X(3.85), Y(1.6), 14 * MM, 5.2 * MM, tint(SYSC, 4), **cst)
    b += edge('a_h3', ARROW + f'strokeColor={grey};curved=1;', X(2.85), Y(3.49), X(1.1), Y(3.01),
              [(X(1.95), Y(3.34))])                     # to[bend right=12]
    b += edge('a_k2', ARROW + f'strokeColor={grey};curved=1;', X(1.1), Y(2.49), X(0.45), Y(1.6),
              [(X(0.68), Y(2.12))])                     # to[bend right=25]
    b += text('refn', f'24 B EdgeRef = {{dst, block, offset, {bf("ts inline")}}} (formats below)',
              X(-0.05), Y(0.9), 320, 14, 'w', 9, mono=True)
    b += text('refn2', f'an append clears {it("sorted")} {bf("before")} it bumps {it("cnt")} (Theorem 4)',
              X(-0.05), Y(0.5), 320, 14, 'w', 9, note)
    # Q2 on the index
    b += box('q2', f'{bf("Q2")} “what did {it("v")}{sub("3")} touch in [{it("t")}{sub("1")}, '
             f'{it("t")}{sub("2")}]”<br>index first, arena only per hit',
             X(14.9), Y(3.75), 160, 32, 'none', QNBR, 9, QNBR, align='left',
             extra='strokeWidth=2;spacingLeft=4;')
    b += steptext('q2s1', 1, QNBR, f'head of {it("v")}{sub("3")}', X(7.6), Y(3.75), 80)
    b += edge('a_q2', ARROW + f'strokeColor={QNBR};', X(14.9) - 80, Y(3.75), X(9.75), Y(3.75))
    b += edge('a_q2h', ARROW + f'strokeColor={QNBR};', X(7.55), Y(3.75), X(3.0), Y(3.75))
    b += edge('q2w', DARROW + f'strokeColor={QNBR};strokeWidth=2;', X(1.75), Y(2.37), X(5.95), Y(2.37))
    b += steptext('q2s2', 2, QNBR, 'binary-search the sorted prefix on the inline timestamps',
                  X(7.6), Y(2.75), 300)
    b += steptext('q2s3', 3, QNBR, 'dereference the arena only for references that pass the window test',
                  X(7.6), Y(1.6), 340)
    b += edge('a_x', ARROW + f'strokeColor={QNBR};strokeWidth=2;curved=1;', X(2.45), Y(1.86),
              X(7.725), Y(6.09), [(X(4.27), Y(4.12)), (X(6.48), Y(5.23))])   # .. controls ..

    # ======================= Panel C: formats
    b += text('pCt', f'{bf("Formats")}: cell widths proportional to bytes; the release-published timestamp '
              f'(<font color="{WINS}">vermillion</font>) is the only synchronization either read path needs',
              X(-0.1), Y(-0.15), 660, 16, 'nw', 11)
    W22 = tint(WINS, 22)

    def frow(prefix, y, x0, fields):
        """Cells chained right=0mm of each other; the first is centred at x0 (cm)."""
        out, x = '', x0 - fields[0][1] / 20             # left edge of the first cell, cm
        for k, (lbl, w_mm, fill) in enumerate(fields):
            w = w_mm * MM
            out += box(f'{prefix}{k + 1}', lbl, X(x) + w / 2, Y(y), w, 5.2 * MM, fill, **cst)
            x += w_mm / 10
        return out

    # Edge, 20 B at 3.0 mm/B (the two "stored last" cells are widened to fit their labels
    # in Helvetica, 26/29 mm instead of 24; the notes after them move right by the same amount)
    b += text('fl1', f'{tt("Edge")} 20 B', X(-0.1), Y(-1.0), 70, 14, 'w', 9, bold=True)
    b += frow('fe', -1.0, 2.0, [('src 4', 12, EDGEC), ('dst 4', 12, EDGEC),
                                ('timestamp 8, stored last', 26, W22), ('w 4', 12, EDGEC)])
    b += text('fn1', f'{it("t")}=0 marks an unpublished slot (T5)', X(7.7), Y(-1.0), 190, 14, 'w', 9, note)
    # EdgeRef, 24 B at 3.0 mm/B
    b += text('fl2', f'{tt("EdgeRef")} 24 B', X(-0.1), Y(-1.75), 70, 14, 'w', 9, bold=True)
    b += frow('fr', -1.75, 2.0, [('dst 4', 12, REFC), ('offset 4', 12, REFC),
                                 ('block pointer 8, stored last', 29, REFC), (bf('timestamp copy 8'), 24, W22)])
    b += text('fn2', 'predicates read the copy, never the arena (§4.3)', X(9.25), Y(-1.75), 230, 14, 'w', 9, note)
    # Block header, one 64 B line
    b += text('fl3', f'{tt("Block")} header 64 B', X(-0.1), Y(-2.5), 90, 14, 'w', 9, bold=True)
    b += frow('fh', -2.5, 2.65, [('cnt 4', 9, HDRC), ('cap 4', 9, HDRC), ('next 8', 11, HDRC),
                                 ('flags 4', 10, HDRC), ('pad to 64', 14, tint(BLACK, 6)),
                                 (f'Edge{sub("0")}', 11, EDGEC), (f'Edge{sub(it("cap") + "−1")}', 13, EDGEC)])
    b += text('fn3', f'property sidecar: prop({it("e")}) = {it("P")}{sub("base")} + ⌊(&amp;{it("e")} − '
              f'{it("A")}{sub("base")})/20⌋ × {tt("prop_bytes")} (§4.1)',
              X(9.95), Y(-2.5), 295, 14, 'w', 9, note)
    write('fig_arch', 690, 490, b)

# ===================================================== fig_runclaim
def runclaim():
    """../fig_runclaim.tex: per-event claim vs run claim, three writers."""
    X = lambda x: (x + 1.0) * CM
    Y = lambda y: (3.05 - y) * CM
    note = tint(BLACK, 65)
    rows = ((1, 1.85), (2, 1.25), (3, 0.65))
    EV = dict(fill=EDGEC, stroke=tint(BLACK, 60))       # ev/.style
    b = ''
    # ================= left: per-event claim
    b += text('t1', 'Per-event claim', X(-0.2), Y(2.7), 120, 16, 'w', 11, bold=True)
    b += text('t1s', f'one {tt("fetch_add")} per event', X(-0.2), Y(2.33), 150, 14, 'w', 9, note)
    for w, y in rows:
        b += text(f'wl{w}', f'writer {it("w")}{sub(w)}', X(0.35), Y(y), 50, 14, 'e', 9)
        for i in range(3):
            b += box(f'e{w}{i}', '', X(0.85 + i * 0.72), Y(y), 6.4 * MM, 4.2 * MM, **EV)
    b += box('c1', f'{tt("count")} +=1', X(1.57), Y(-0.2), 16 * MM, 5 * MM, HDRC, tint(BLACK, 70))
    for w, y in rows:
        for i in range(3):
            b += edge(f'a{w}{i}', ARROW + f'strokeColor={WINS};endSize=4;',
                      X(0.85 + i * 0.72), Y(y - 0.21), X(1.57), Y(0.05))
    b += text('n1', '9 atomics on one cache line', X(1.57), Y(-0.55), 140, 14, 'n', 9, WINS)
    # ================= right: run claim
    b += text('t2', 'Run claim', X(4.1), Y(2.7), 120, 16, 'w', 11, bold=True)
    b += text('t2s', f'one {tt("fetch_add")} per run', X(4.1), Y(2.33), 140, 14, 'w', 9, note)
    for w, y in rows:
        b += text(f'rl{w}', f'writer {it("w")}{sub(w)}', X(4.65), Y(y), 50, 14, 'e', 9)
        b += cell(f'run{w}', '', f'rounded=1;absoluteArcSize=1;arcSize=3;html=1;fillColor=none;'
                  f'strokeColor={SYSC};strokeWidth=1.5;', X(4.78), Y(y + 0.27), 2.22 * CM, 0.54 * CM)
        for i in range(3):
            b += box(f'f{w}{i}', '', X(5.17 + i * 0.72), Y(y), 6.4 * MM, 4.2 * MM, **EV)
    b += box('c2', f'{tt("count")} +=3', X(5.89), Y(-0.2), 16 * MM, 5 * MM, HDRC, tint(BLACK, 70))
    for w, y in rows:
        b += edge(f'ra{w}', ARROW + f'strokeColor={SYSC};endSize=4;', X(5.89), Y(y - 0.29), X(5.89), Y(0.05))
    b += text('n2', '3 atomics, same 9 events placed', X(5.89), Y(-0.55), 140, 14, 'n', 9, SYSC)
    b += edge('div', LINE + f'strokeColor={tint(BLACK, 35)};', X(3.85), Y(-1.0), X(3.85), Y(2.9))
    write('fig_runclaim', 340, 165, b)

# ===================================================== fig_motivation
def motivation():
    """fig_motivation.py: the CADETS chain, two Q3 questions, retention horizon.

    matplotlib data coordinates x in [0, 11], y in [-0.44, 1.22]; drawn at
    1.25x the paper's 3.34 x 1.72 in so the 5.8pt labels can be fontSize 9.
    """
    KX, KY = 400 / 11, 206 / 1.66
    X = lambda x: 10 + x * KX
    Y = lambda y: 10 + (1.22 - y) * KY
    G = QNBR
    b = ''
    # (3) retention: the oldest prefix is deleted (axvspan, 7 % black, axes fractions .17-.93)
    b += cell('ret', '', f'rounded=0;html=1;fillColor={tint(BLACK, 7)};strokeColor=none;',
              X(0.05), Y(-0.44 + 0.93 * 1.66), 2.4 * KX, 0.76 * 1.66 * KY)
    # (1) the stream: event ticks in arrival order along the time axis (same seed as the .py)
    random.seed(7)
    for k, x in enumerate(random.uniform(0.3, 10.6) for _ in range(130)):
        b += edge(f'tick{k}', LINE + f'strokeColor={LIGHT};strokeWidth=0.6;', X(x), Y(0.02), X(x), Y(0.10))
    b += edge('axis', ARROW + 'strokeColor=#000000;strokeWidth=1.5;', X(0.05), Y(0), X(10.95), Y(0))
    b += text('time', 'time', X(10.9), Y(-0.1), 40, 14, 'e', 9, extra='fontStyle=2;')
    b += text('s1', f'{bf("1")} events arrive in time order, never change', X(0.05), Y(-0.265), 260, 14, 'sw', 9)
    b += edge('reta', ARROW + f'strokeColor={GREY};', X(1.85), Y(0.98), X(2.95), Y(0.98))
    b += text('retl', f'{bf("3")} retention<br>horizon', X(1.25), Y(1.03), 70, 26, 's', 9, GREY)
    # the attack chain: a time-respecting path through the stream
    chain = [(3.4, 0.42, 'implant<br>drop', True), (5.1, 0.62, 'privilege<br>escalation', False),
             (6.9, 0.42, 'C&amp;C<br>traffic', True), (8.6, 0.62, 'process<br>injection', False),
             (10.1, 0.50, 'alert', True)]
    for k, (x, y, lbl, up) in enumerate(chain):
        b += edge(f'drop{k}', LINE + f'strokeColor={SYSC};strokeWidth=0.7;dashed=1;dashPattern=1 2;',
                  X(x), Y(0.02), X(x), Y(y - 0.045))
    for k in range(len(chain) - 1):
        (x0, y0), (x1, y1) = chain[k][:2], chain[k + 1][:2]
        sx, sy, tx, ty = X(x0 + 0.13), Y(y0), X(x1 - 0.13), Y(y1)
        dx, dy = tx - sx, ty - sy
        wp = ((sx + tx) / 2 + 0.09 * dy, (sy + ty) / 2 - 0.09 * dx)    # arc3, rad=-0.18
        b += edge(f'c{k}', ARROW + f'strokeColor={SYSC};strokeWidth=1.2;curved=1;', sx, sy, tx, ty, [wp])
    for k, (x, y, lbl, up) in enumerate(chain):
        if lbl == 'alert':                              # the star marker
            b += text(f'm{k}', '★', X(x), Y(y), 20, 20, 'c', 16)
        else:
            b += cell(f'm{k}', '', f'ellipse;html=1;fillColor={SYSC};strokeColor=#FFFFFF;',
                      X(x) - 5, Y(y) - 5, 10, 10)
        b += text(f'l{k}', lbl, X(x), Y(y + 0.09) if up else Y(y - 0.09), 70,
                  14 if lbl == 'alert' else 24, 's' if up else 'n', 9, extra='fillColor=#FFFFFF;')
    # (2) the two questions: traversals along the time axis, both Q3
    b += edge('bwd', ARROW + f'strokeColor={G};strokeWidth=1.2;dashed=1;dashPattern=5 3;',
              X(9.9), Y(0.24), X(3.6), Y(0.24))
    b += text('bwdl', 'Q3 backward: which earlier events caused the alert?', X(6.7), Y(0.17), 260, 14, 'c', 9, G)
    b += edge('fwd', ARROW + f'strokeColor={G};strokeWidth=1.2;', X(3.6), Y(0.90), X(10.3), Y(0.90))
    b += text('fwdl', 'Q3 forward: what did the implant touch afterwards?', X(7.0), Y(0.99), 260, 14, 'c', 9, G)
    b += text('s2', f'{bf("2")} the questions walk the time axis; {bf("3")} retention deletes its oldest end',
              X(0.05), Y(-0.415), 380, 14, 'sw', 9, G)
    write('fig_motivation', 420, 230, b)

if __name__ == '__main__':
    overview(); arch(); runclaim(); motivation()
