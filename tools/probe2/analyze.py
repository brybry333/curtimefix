#!/usr/bin/env python3
"""analyze.py rec.bin [segment ...] — camera smoothness per probe2 segment.

For every frame: the camera this frame was drawn with (GetPlayerView), the render time the local player was
interpolated at (T = (final predicted tick - 1 + interpolation amount) * tick interval) and wall time.

Per segment it prints
  fps and frame-time spread (wall time between FRAME_RENDER_END calls),
  camera speed per frame over render time and over wall time, frame-to-frame change of it (the meter's number),
  "steps": frames whose movement is off from the smooth path by more than 25% of a normal frame's movement,
  yaw rate evenness, and frames where render time stood still or went backwards.
Standard library only.
"""
import math
import struct
import sys
from collections import defaultdict

FMT = '<dd3f3f3f3f4f4i2i'
SZ = struct.calcsize(FMT)
assert SZ == 104


def load(path):
    d = open(path, 'rb').read()
    out = []
    for i in range(0, len(d) - SZ + 1, SZ):
        v = struct.unpack_from(FMT, d, i)
        r = {
            't5': v[0], 't6': v[1], 'vo': v[2:5], 'va': v[5:8], 'ea': v[8:11], 'ao': v[11:14],
            'curtime': v[14], 'frametime': v[15], 'interp': v[16], 'absft': v[17],
            'tick': v[18], 'frame': v[19], 'tickbase': v[20], 'final': v[21], 'seg': v[22], 'flags': v[23],
        }
        out.append(r)
    return out


def pct(xs, p):
    if not xs:
        return float('nan')
    s = sorted(xs)
    k = min(len(s) - 1, max(0, int(round(p / 100 * (len(s) - 1)))))
    return s[k]


def mean(xs):
    return sum(xs) / len(xs) if xs else float('nan')


def sd(xs):
    if len(xs) < 2:
        return float('nan')
    m = mean(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / len(xs))


def wrap(a):
    while a > 180:
        a -= 360
    while a < -180:
        a += 360
    return a


def analyze(recs, seg, interval=0.01):
    rs = [r for r in recs if r['seg'] == seg and r['flags'] & 1]
    if len(rs) < 50:
        print(f'segment {seg}: {len(rs)} frames, too few')
        return
    # render time of the predicted local player
    T = [((r['final'] - 1) + r['interp']) * interval for r in rs]
    t = [r['t6'] for r in rs]
    dt = [t[i] - t[i - 1] for i in range(1, len(rs))]
    dT = [T[i] - T[i - 1] for i in range(1, len(rs))]
    pos = [r['vo'] for r in rs]
    d = [math.dist(pos[i], pos[i - 1]) for i in range(1, len(rs))]
    dur = t[-1] - t[0]
    med_dt = pct(dt, 50)
    print(f'segment {seg}: {len(rs)} frames over {dur:.1f} s = {len(rs) / dur:.0f} fps; frame time median {med_dt * 1e3:.3f} ms, '
          f'p99 {pct(dt, 99) * 1e3:.3f}, max {max(dt) * 1e3:.2f}, frames > 2x median: {sum(1 for x in dt if x > 2 * med_dt)}')
    back = sum(1 for x in dT if x < -1e-6)
    still = sum(1 for x in dT if abs(x) <= 1e-6)
    print(f'  render time: still {still} frames, backwards {back} frames; mean step {mean(dT) * 1e3:.3f} ms vs wall {mean(dt) * 1e3:.3f} ms')
    # moving frames only
    idx = [i for i in range(len(d)) if dT[i] > 1e-6 and d[i] / dT[i] > 50]
    if len(idx) < 50:
        print('  camera: not moving enough')
    else:
        vr = [d[i] / dT[i] for i in idx]
        vw = [d[i] / dt[i] for i in idx if dt[i] > 0]
        f2f = [abs(vr[k] - vr[k - 1]) for k in range(1, len(vr)) if idx[k] == idx[k - 1] + 1]
        f2fw = []
        for k in range(1, len(idx)):
            i, j = idx[k], idx[k - 1]
            if i == j + 1 and dt[i] > 0 and dt[j] > 0:
                f2fw.append(abs(d[i] / dt[i] - d[j] / dt[j]))
        print(f'  camera speed over render time: mean {mean(vr):.1f} u/s, spread {100 * sd(vr) / mean(vr):.2f}%, '
              f'frame-to-frame {100 * mean(f2f) / mean(vr):.2f}%')
        print(f'  camera speed over wall time:   mean {mean(vw):.1f} u/s, spread {100 * sd(vw) / mean(vw):.2f}%, '
              f'frame-to-frame {100 * mean(f2fw) / mean(vw):.2f}%')
    # steps: compare each frame's position to the midpoint of its neighbours, weighted by render time
    steps = []
    for i in range(1, len(rs) - 1):
        T0, T1, T2 = T[i - 1], T[i], T[i + 1]
        if not (T2 - T0 > 1e-6 and T0 < T1 < T2):
            continue
        w = (T1 - T0) / (T2 - T0)
        exp = [pos[i - 1][k] + w * (pos[i + 1][k] - pos[i - 1][k]) for k in range(3)]
        err = math.dist(exp, pos[i])
        step = math.dist(pos[i + 1], pos[i - 1]) / 2
        if step > 0.05:
            steps.append((err / step, i, err))
    big = [s for s in steps if s[0] > 0.25]
    print(f'  path: {len(steps)} frames checked, off the smooth path by > 25% of a frame: {len(big)} '
          f'({100 * len(big) / max(1, len(steps)):.2f}%), worst {max((s[2] for s in steps), default=0):.3f} units')
    if big:
        ticks = defaultdict(int)
        for _, i, _ in big:
            ticks[rs[i]['tick']] += 1
        top = sorted(big, reverse=True)[:8]
        print('   worst at client ticks: ' + ', '.join(f"{rs[i]['tick']} ({e:.2f}u, {100 * r:.0f}%)" for r, i, e in top))
    # yaw
    yaw = [wrap(rs[i]['va'][1] - rs[i - 1]['va'][1]) for i in range(1, len(rs))]
    yr = [yaw[i] / dt[i] for i in range(len(yaw)) if dt[i] > 0 and abs(yaw[i]) > 1e-4]
    if len(yr) > 50:
        yf = [abs(yr[k] - yr[k - 1]) for k in range(1, len(yr))]
        print(f'  yaw: {len(yr)} turning frames, mean {mean(yr):.1f} deg/s, spread {100 * sd(yr) / abs(mean(yr)):.2f}%, '
              f'frame-to-frame {100 * mean(yf) / abs(mean(yr)):.2f}%')


def main():
    recs = load(sys.argv[1])
    segs = [int(s) for s in sys.argv[2:]] or sorted({r['seg'] for r in recs})
    print(f'{len(recs)} records')
    for s in segs:
        analyze(recs, s)


if __name__ == '__main__':
    main()
