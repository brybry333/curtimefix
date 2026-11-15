#!/usr/bin/env python3
"""within_tick.py rec.bin — inside one tick the local player's camera must move on a straight line in the
interpolation amount (render origin = prev + a * (cur - prev)). Per segment: residuals off that line."""
import math, struct, sys
from collections import defaultdict
FMT = '<dd3f3f3f3f4f4i2i'; SZ = struct.calcsize(FMT)
d = open(sys.argv[1], 'rb').read()
recs = [struct.unpack_from(FMT, d, i) for i in range(0, len(d) - SZ + 1, SZ)]
segs = sorted({r[22] for r in recs})
for s in segs:
    rs = [r for r in recs if r[22] == s and r[23] & 1]
    groups = defaultdict(list)
    for r in rs:
        groups[r[21]].append((r[16], r[2:5]))   # final predicted tick -> (a, view origin)
    rel = []; absr = []; nt = 0
    for k, g in groups.items():
        if len(g) < 5: continue
        a = [x[0] for x in g]
        if max(a) - min(a) < 0.3: continue
        n = len(g); ma = sum(a) / n
        saa = sum((x - ma) ** 2 for x in a)
        if saa <= 0: continue
        res2 = [0.0] * n; slope_len = 0.0
        for c in range(3):
            y = [x[1][c] for x in g]; my = sum(y) / n
            b = sum((a[i] - ma) * (y[i] - my) for i in range(n)) / saa
            slope_len += b * b
            for i in range(n): res2[i] += (y[i] - (my + b * (a[i] - ma))) ** 2
        tickmove = math.sqrt(slope_len)            # units per whole tick
        if tickmove < 1.0: continue
        step = tickmove / n                        # typical per-frame step
        nt += 1
        for i in range(n):
            r_ = math.sqrt(res2[i]); absr.append(r_); rel.append(r_ / step)
    if not rel:
        print(f'segment {s}: no usable ticks'); continue
    rel.sort(); absr.sort()
    q = lambda xs, p: xs[min(len(xs) - 1, int(p / 100 * (len(xs) - 1)))]
    print(f'segment {s}: {nt} moving ticks, {len(rel)} frames; residual off the in-tick line, as a share of a frame step: '
          f'median {100*q(rel,50):.2f}%  p90 {100*q(rel,90):.2f}%  p99 {100*q(rel,99):.2f}%  max {100*rel[-1]:.0f}%; '
          f'frames > 25%: {100*sum(1 for x in rel if x > .25)/len(rel):.2f}%; abs p99 {q(absr,99):.4f} u')
