#!/usr/bin/env python3
"""yaw.py rec.bin seg... — mouse-turn evenness: yaw per frame in mouse counts (sensitivity*m_yaw), during turns.
Per frame and per pair of frames (one 540 Hz refresh at 1080 fps): share of frames whose rotation is off its
local average (21-frame window) by more than 40%, and frames with no rotation in the middle of a turn."""
import struct, sys
FMT = '<dd3f3f3f3f4f4i2i'; SZ = struct.calcsize(FMT)
UNIT = 1.2 * 0.0355
d = open(sys.argv[1], 'rb').read()
recs = [struct.unpack_from(FMT, d, i) for i in range(0, len(d) - SZ + 1, SZ)]
def wrap(a):
    while a > 180: a -= 360
    while a < -180: a += 360
    return a
for seg in [int(x) for x in sys.argv[2:]]:
    rs = [r for r in recs if r[22] == seg]
    c = [wrap(rs[i][6] - rs[i - 1][6]) / UNIT for i in range(1, len(rs))]
    for name, xs in (('per frame', c), ('per refresh (2 frames)', [c[i] + c[i + 1] for i in range(0, len(c) - 1, 2)])):
        W = 10; n = 0; off = 0; zero = 0
        for i in range(W, len(xs) - W):
            win = xs[i - W:i + W + 1]
            m = sum(win) / len(win)
            if abs(m) < 2.0 or any((x > 0) != (m > 0) for x in win if abs(x) > 0.5):   # steady one-way turns only
                continue
            n += 1
            if abs(xs[i] - m) > 0.4 * abs(m): off += 1
            if abs(xs[i]) < 0.5: zero += 1
        if n:
            print(f'segment {seg} {name}: {n} turning samples, off their local average by >40%: {100*off/n:.1f}%, no rotation: {100*zero/n:.1f}%')
