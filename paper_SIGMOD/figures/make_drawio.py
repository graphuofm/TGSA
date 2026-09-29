#!/usr/bin/env python3
"""Emit editable .drawio sources for the four hand-drawn figures.

The data charts in this directory are generated from CSVs and have .py
sources; a drawio file would be the wrong thing for them. The four
figures below are drawings, so each gets a same-named .drawio that
opens in diagrams.net and can be rearranged and re-exported to PDF.

The LaTeX/TikZ originals (../fig_arch.tex, ../fig_format.tex,
../fig_runclaim.tex) remain what the paper compiles; when you change a
drawio and want it in the paper, export to PDF here and swap the
\\input for an \\includegraphics, or mirror the change back into the
TikZ.

Run: python3 make_drawio.py
"""
import html

# palette, matching the paper's macros
QWIN, QNBR, WINS = '#0072B2', '#009E73', '#D55E00'
PANEL, EDGEC, HDRC, REFC = '#FFF6EC', '#E6F2E6', '#DCE8FA', '#FFF3D0'

def cell(cid, value, style, x, y, w, h, parent='1'):
    return (f'        <mxCell id="{cid}" value="{html.escape(value)}" '
            f'style="{style}" vertex="1" parent="{parent}">\n'
            f'          <mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/>\n'
            f'        </mxCell>\n')

def edge(cid, style, sx, sy, tx, ty):
    return (f'        <mxCell id="{cid}" style="{style}" edge="1" parent="1">\n'
            f'          <mxGeometry relative="1" as="geometry">\n'
            f'            <mxPoint x="{sx}" y="{sy}" as="sourcePoint"/>\n'
            f'            <mxPoint x="{tx}" y="{ty}" as="targetPoint"/>\n'
            f'          </mxGeometry>\n'
            f'        </mxCell>\n')

BOX   = 'rounded=0;whiteSpace=wrap;html=1;fontSize=9;'
TEXT  = 'text;html=1;align=left;verticalAlign=middle;fontSize=9;'
TITLE = 'text;html=1;align=left;verticalAlign=middle;fontSize=11;fontStyle=1;'
ARROW = 'endArrow=block;endFill=1;html=1;rounded=0;'

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
    open(name + '.drawio', 'w').write(xml)
    print('wrote', name + '.drawio')

# ===================================================== fig_runclaim
def runclaim():
    b = ''
    b += cell('t1', 'Per-edge claim', TITLE, 20, 10, 160, 20)
    b += cell('t1s', 'one fetch_add per event', TEXT + 'fontColor=#666666;', 20, 30, 200, 18)
    b += cell('t2', 'Run claim', TITLE, 420, 10, 160, 20)
    b += cell('t2s', 'one fetch_add per run', TEXT + 'fontColor=#666666;', 420, 30, 200, 18)
    b += cell('div', '', 'line;direction=north;strokeColor=#BBBBBB;', 395, 10, 10, 320)
    for k, y in enumerate((60, 110, 160)):
        b += cell(f'wl{k}', f'w{k+1}', TEXT, 20, y, 26, 24)
        for i in range(3):
            b += cell(f'e{k}{i}', '', BOX + f'fillColor={EDGEC};', 55 + i*60, y, 52, 24)
            b += edge(f'a{k}{i}', ARROW + f'strokeColor={WINS};strokeWidth=1;',
                      81 + i*60, y + 24, 175, 250)
        b += cell(f'rl{k}', f'w{k+1}', TEXT, 420, y, 26, 24)
        b += cell(f'run{k}', '', 'rounded=1;whiteSpace=wrap;html=1;fillColor=none;'
                  f'strokeColor={QNBR};strokeWidth=2;', 450, y - 4, 188, 32)
        for i in range(3):
            b += cell(f'f{k}{i}', '', BOX + f'fillColor={EDGEC};', 455 + i*60, y, 52, 24)
        b += edge(f'ra{k}', ARROW + f'strokeColor={QNBR};strokeWidth=1.5;',
                  545, y + 28, 545, 250)
    b += cell('c1', 'count += 1', BOX + f'fillColor={HDRC};', 120, 250, 110, 28)
    b += cell('c2', 'count += 3', BOX + f'fillColor={HDRC};', 490, 250, 110, 28)
    b += cell('n1', '9 atomics on one cache line',
              TEXT + f'fontColor={WINS};align=center;', 95, 284, 160, 20)
    b += cell('n2', '3 atomics, same 9 events placed',
              TEXT + f'fontColor={QNBR};align=center;', 465, 284, 170, 20)
    b += cell('foot', 'wiki-talk, 8 writers:  6.1  →  73.2 M events/s',
              TEXT + 'align=center;fontStyle=1;', 230, 318, 300, 20)
    write('fig_runclaim', 700, 360, b)

# ===================================================== fig_format
def fmt():
    b = ''
    rows = [
        ('Edge, 20 B packed, arena-resident',
         [('src 4', EDGEC, 70), ('dst 4', EDGEC, 70),
          ('timestamp 8 (release-published)', '#FFE0CC', 190), ('w 4', EDGEC, 55)],
         'ts = 0 marks an unpublished slot (T5)'),
        ('EdgeRef, 24 B, vertex-index bucket entry',
         [('dst 4', REFC, 70), ('off 4', REFC, 70),
          ('block ptr 8', REFC, 110), ('ts cache 8', '#FFE0CC', 110)],
         'temporal predicates never dereference the arena (§4.3); the 16 B ablation build drops it'),
        ('Block: one 64 B header line, then edges',
         [('cnt 4', HDRC, 60), ('cap 4', HDRC, 60), ('next 8', HDRC, 70),
          ('flags 4', HDRC, 65), ('pad→64', '#EFEFEF', 70),
          ('Edge0', EDGEC, 60), ('...', EDGEC, 40), ('Edge cap-1', EDGEC, 80)],
         'flags: PUBLISHED cleared before every cnt bump, same protocol as the bucket sorted flag'),
    ]
    y = 20
    for n, (title, fields, note) in enumerate(rows):
        b += cell(f'h{n}', title, TITLE, 20, y, 420, 20)
        x = 20
        for i, (lbl, fill, w) in enumerate(fields):
            b += cell(f'r{n}f{i}', lbl, BOX + f'fillColor={fill};', x, y + 24, w, 26)
            x += w
        b += cell(f'n{n}', note, TEXT + 'fontColor=#666666;fontSize=8;', 20, y + 54, 620, 18)
        y += 100
    b += cell('sh', 'Property sidecar (dock module, §4.1)', TITLE, 20, y, 420, 20)
    b += cell('sf', 'prop(e)  =  P_base  +  floor((&e - A_base) / 20) × prop_bytes',
              BOX + 'fillColor=#F5F5F5;fontFamily=Courier New;', 20, y + 24, 420, 26)
    b += cell('sn', 'pure address arithmetic: demand-paged, prefix-evicted and '
              'replay-recovered with the arena itself; written before the timestamp '
              'store, so T5 covers it',
              TEXT + 'fontColor=#666666;fontSize=8;', 20, y + 54, 620, 30)
    write('fig_format', 700, y + 110, b)

# ===================================================== fig_arch
def arch():
    b = ''
    b += cell('inA', 'event stream, never ends\ne = (src, dst, t, w), immutable',
              BOX + 'fillColor=none;', 20, 10, 190, 38)
    b += cell('q1', 'Q1  "all events in [t1, t2]"\ntime-keyed: touches only the timeline',
              BOX + f'fillColor=none;strokeColor={QWIN};fontColor={QWIN};strokeWidth=2;',
              240, 10, 240, 38)
    b += cell('q2', 'Q2  "what did v3 touch in [t1, t2]"\nentity-keyed: index first, arena per hit',
              BOX + f'fillColor=none;strokeColor={QNBR};fontColor={QNBR};strokeWidth=2;',
              640, 10, 250, 38)
    # panel A
    b += cell('pA', '', BOX + f'fillColor={PANEL};strokeColor=#888888;', 20, 70, 590, 300)
    b += cell('pAt', 'Time-keyed storage (§4.1–§4.2):  Timeline = tmap<block ptr>  +  Arena',
              TITLE, 32, 78, 520, 20)
    b += cell('eqn', '❶  A(T) = A_base + ((T − T0) >> S) × P',
              TEXT + f'fontColor={WINS};fontStyle=1;', 32, 104, 330, 20)
    for i in range(12):
        fill = '#D9D9D9' if i < 3 else ('#FFE0CC' if i == 5 else ('#FFFFFF' if i in (4, 7, 9, 11) else '#F0F0F0'))
        val = '0' if i in (4, 7, 9, 11) else ''
        b += cell(f's{i}', val, BOX + f'fillColor={fill};fontSize=8;', 34 + i*44, 140, 42, 26)
    b += cell('slab', 'W0 .. W11, one slot per 2^S ticks', TEXT + 'fontSize=8;', 34, 172, 250, 16)
    b += cell('ev', 'evicted: madvise prefix (T3)', TEXT + 'fontSize=8;fontColor=#666666;', 34, 190, 180, 16)
    b += cell('t2n', '0 = never faulted, costs nothing (T2)', TEXT + 'fontSize=8;fontColor=#666666;', 300, 190, 230, 16)
    blocks = [('b1', 40, [('cnt 4\nflags', HDRC, 58), ('(v3,t17)', EDGEC, 62), ('(v9,t18)', EDGEC, 62), ('(v3,t21)', EDGEC, 62)]),
              ('b2', 290, [('cnt 4\nflags', HDRC, 58), ('(v1,t40)', EDGEC, 62), ('(v3,t41)', EDGEC, 62), ('(v8,t42)', '#FFE0CC', 62)])]
    for bid, bx, fields in blocks:
        x = bx
        for i, (lbl, fill, w) in enumerate(fields):
            b += cell(f'{bid}_{i}', lbl, BOX + f'fillColor={fill};fontSize=8;', x, 236, w, 30)
            x += w
    b += cell('b3_0', 'cnt 4\nflags', BOX + f'fillColor={HDRC};fontSize=8;', 540, 236, 58, 30)
    b += edge('ar1', ARROW + 'strokeColor=#777777;', 145, 166, 130, 236)
    b += edge('ar2', ARROW + 'strokeColor=#777777;', 275, 166, 380, 236)
    b += cell('st2', '❷ one fetch_add claims the run', TEXT + f'fontColor={WINS};fontSize=8;', 40, 276, 240, 16)
    b += cell('st3', '❸ payload first, t last (release, T5)', TEXT + f'fontColor={WINS};fontSize=8;', 40, 294, 260, 16)
    b += cell('arn', '20 B packed edges, never sorted on insert', TEXT + 'fontSize=8;fontColor=#666666;', 40, 340, 280, 16)
    b += cell('q1s', '❶ map t1,t2   ❷ walk W6..W9', TEXT + f'fontColor={QWIN};fontSize=8;', 240, 118, 220, 16)
    b += cell('q1t', '❸ interior blocks copied comparison-free', TEXT + f'fontColor={QWIN};fontSize=8;', 330, 208, 260, 16)
    # panel B
    b += cell('pB', '', BOX + f'fillColor={PANEL};strokeColor=#888888;', 630, 70, 330, 300)
    b += cell('pBt', 'Vertex index (§4.3): append chains,\nsorted only by compact()', TITLE, 642, 78, 300, 32)
    for i in range(4):
        fill = f'{QNBR}20' if i == 2 else '#FFFFFF'
        b += cell(f'h{i}', f'v{i+1}', BOX + ('fillColor=#CCEDE0;' if i == 2 else 'fillColor=#FFFFFF;') + 'fontSize=8;',
                  646 + i*48, 122, 44, 24)
    b += cell('hn', 'heads, one per vertex', TEXT + 'fontSize=8;', 846, 122, 120, 24)
    b += cell('k1', 'cnt 3\nsorted 1', BOX + f'fillColor={HDRC};fontSize=8;', 646, 170, 62, 30)
    for i, lbl in enumerate(['v9 | t18', 'v3 | t21', 'v1 | t40']):
        b += cell(f'r1{i}', lbl, BOX + f'fillColor={REFC};fontSize=8;', 708 + i*66, 170, 66, 30)
    b += cell('k2', 'cnt 2\nsorted 0', BOX + f'fillColor={HDRC};fontSize=8;', 646, 226, 62, 30)
    b += cell('r20', 'v3 | t44', BOX + f'fillColor={REFC};fontSize=8;', 708, 226, 66, 30)
    b += cell('r21', 'appending…', BOX + 'fillColor=#FFFBEA;fontSize=8;', 774, 226, 76, 30)
    b += edge('be1', ARROW + 'strokeColor=#777777;', 668, 146, 668, 170)
    b += edge('be2', ARROW + 'strokeColor=#777777;', 660, 200, 660, 226)
    b += cell('q2a', '❶ head of v3', TEXT + f'fontColor={QNBR};fontSize=8;', 862, 150, 100, 16)
    b += cell('q2b', '❷ binary-search on inline ts', TEXT + f'fontColor={QNBR};fontSize=8;', 862, 178, 100, 30)
    b += cell('q2c', '❸ arena only per hit', TEXT + f'fontColor={QNBR};fontSize=8;', 862, 226, 100, 30)
    b += cell('refn', '24 B EdgeRef: {dst, blk, off, ts inline}', TEXT + 'fontSize=8;fontFamily=Courier New;', 646, 280, 300, 16)
    b += cell('refn2', 'appends clear sorted BEFORE bumping cnt (Thm 4.4)', TEXT + 'fontSize=8;fontColor=#666666;', 646, 300, 300, 16)
    b += edge('cross', ARROW + f'strokeColor={QNBR};strokeWidth=2;', 700, 256, 220, 266)
    write('fig_arch', 1000, 400, b)

# ===================================================== fig_motivation
def motivation():
    b = ''
    b += cell('tl', '', 'line;strokeWidth=1.5;html=1;', 30, 250, 640, 10)
    b += edge('tla', ARROW + 'strokeWidth=1.5;', 30, 255, 680, 255)
    b += cell('tlab', 'time (11 days)', TEXT + 'fontStyle=2;align=right;', 560, 262, 120, 18)
    b += cell('ret', '', BOX + 'fillColor=#EDEDED;strokeColor=none;opacity=60;', 30, 60, 150, 190)
    b += cell('retl', 'retention horizon\n(evidence deleted)',
              TEXT + 'align=center;fontStyle=2;fontColor=#666666;', 40, 20, 130, 34)
    b += edge('reta', ARROW + 'strokeColor=#888888;', 120, 45, 190, 45)
    chain = [('implant\ndrop', 215, 190), ('privilege\nescalation', 315, 150),
             ('C&C\ntraffic', 415, 190), ('process\ninjection', 515, 150),
             ('alert', 615, 170)]
    for i, (lbl, x, y) in enumerate(chain):
        last = (i == len(chain) - 1)
        col = WINS if last else QWIN
        b += cell(f'n{i}', '', f'ellipse;html=1;fillColor={col};strokeColor=#FFFFFF;', x - 7, y - 7, 14, 14)
        b += cell(f'l{i}', lbl, TEXT + 'align=center;fontSize=8;', x - 45, y - 42, 90, 30)
        b += edge(f'd{i}', 'endArrow=none;dashed=1;html=1;strokeColor=#AACCEE;', x, y + 8, x, 250)
        if i:
            px, py = chain[i-1][1], chain[i-1][2]
            b += edge(f'c{i}', ARROW + f'strokeColor={QWIN};strokeWidth=1.5;curved=1;', px + 9, py, x - 9, y)
    b += edge('fwd', ARROW + f'strokeColor={WINS};dashed=1;strokeWidth=1.5;', 215, 95, 640, 95)
    b += cell('fwdl', 'forward: what did the implant touch?',
              TEXT + f'fontColor={WINS};align=center;', 280, 72, 300, 18)
    b += edge('bwd', ARROW + f'strokeColor={QNBR};dashed=1;strokeWidth=1.5;', 615, 225, 215, 225)
    b += cell('bwdl', 'backward: what caused the alert?',
              TEXT + f'fontColor={QNBR};align=center;', 290, 228, 280, 18)
    write('fig_motivation', 720, 300, b)

if __name__ == '__main__':
    runclaim(); fmt(); arch(); motivation()
