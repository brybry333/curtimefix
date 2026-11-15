#!/usr/bin/env python3
"""check_build.py — after a CS:S update, check that everything curtimefix looks for is still where it looks.

  check_build.py                    the installed Linux game (Steam's default library)
  check_build.py <client> <engine>  any client.so/engine.so or client.dll/engine.dll pair (64-bit)

Does the same discovery as src/find.cpp, on the files instead of the running game: CInterpolatedVar RTTI names →
vtables → the Interpolate thunk → implementations, every rel32 call/jmp into them, the NoteChanged slot, the gun's
vtables, and the IEngineTool clock accessors. Prints what it found; exit code 1 if the mod would stay off.
Standard library only.
"""
import os
import re
import struct
import sys


class Image:
    """A module laid out at its preferred addresses, relocations applied (base = 0 for ELF, ImageBase for PE)."""

    def __init__(self, path):
        self.path = path
        d = open(path, 'rb').read()
        self.win = d[:2] == b'MZ'
        (self._pe if self.win else self._elf)(d)

    def _elf(self, d):
        phoff, = struct.unpack_from('<Q', d, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
        segs = []
        for i in range(phnum):
            typ, flags, off, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
            if typ == 1:
                segs.append((vaddr, memsz, off, filesz, flags))
        size = max(v + m for v, m, *_ in segs)
        self.mem = bytearray(size)
        self.base = 0
        self.text, self.data = [], []
        for v, m, o, fs, fl in segs:
            self.mem[v:v + fs] = d[o:o + fs]
            (self.text if fl & 1 else self.data).append((v, v + m))
        shoff, = struct.unpack_from('<Q', d, 0x28)
        shentsize, shnum, _ = struct.unpack_from('<HHH', d, 0x3a)
        for i in range(shnum):
            _, typ, _, _, off, size, *_ = struct.unpack_from('<IIQQQQ', d, shoff + i * shentsize)
            if typ == 4:   # RELA
                for k in range(off, off + size, 24):
                    r_off, info, add = struct.unpack_from('<QQq', d, k)
                    if info & 0xffffffff == 8:   # R_X86_64_RELATIVE
                        struct.pack_into('<q', self.mem, r_off, add)

    def _pe(self, d):
        pe, = struct.unpack_from('<I', d, 0x3c)
        nsec, = struct.unpack_from('<H', d, pe + 6)
        optsz, = struct.unpack_from('<H', d, pe + 20)
        opt = pe + 24
        self.base, = struct.unpack_from('<Q', d, opt + 24)
        imgsize, = struct.unpack_from('<I', d, opt + 56)
        self.mem = bytearray(imgsize)
        self.text, self.data = [], []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            vsz, va, rsz, rp = struct.unpack_from('<IIII', d, o + 8)
            ch, = struct.unpack_from('<I', d, o + 36)
            self.mem[va:va + min(rsz, vsz)] = d[rp:rp + min(rsz, vsz)]
            if ch & 0x40000000:   # readable
                (self.text if ch & 0x20000000 else self.data).append((self.base + va, self.base + va + max(vsz, rsz)))

    # addresses are virtual addresses (ELF: from 0, PE: from ImageBase)
    def q(self, a): return struct.unpack_from('<Q', self.mem, a - self.base)[0]
    def d32(self, a): return struct.unpack_from('<I', self.mem, a - self.base)[0]
    def s32(self, a): return struct.unpack_from('<i', self.mem, a - self.base)[0]
    def b(self, a, n): return bytes(self.mem[a - self.base:a - self.base + n])
    def in_text(self, a): return any(lo <= a < hi for lo, hi in self.text)

    def each_q(self, v):
        pat = struct.pack('<Q', v)
        for lo, hi in self.data:
            blob = self.mem[lo - self.base:hi - self.base]
            for m in re.finditer(re.escape(pat), blob):
                if (lo + m.start()) % 8 == 0:
                    yield lo + m.start()

    def each_d(self, v):
        pat = struct.pack('<I', v)
        for lo, hi in self.data:
            blob = self.mem[lo - self.base:hi - self.base]
            for m in re.finditer(re.escape(pat), blob):
                if (lo + m.start()) % 4 == 0:
                    yield lo + m.start()

    def strings(self, rx):
        for lo, hi in self.data:
            blob = self.mem[lo - self.base:hi - self.base]
            for m in re.finditer(rb'(?<=\x00)' + rx + rb'(?=\x00)', blob):
                yield lo + m.start(), m.group(0).decode()

    def vtables(self, name_addr):
        out = []
        if not self.win:
            for at in self.each_q(name_addr):
                for at2 in self.each_q(at - 8):
                    if self.q(at2 - 8) == 0 and self.in_text(self.q(at2 + 8)):
                        out.append(at2 + 8)
        else:
            td = name_addr - 16
            for at in self.each_d(td - self.base):
                col = at - 12
                if self.d32(col) == 1 and self.d32(col + 4) == 0 and self.d32(col + 20) == col - self.base:
                    out += [at2 + 8 for at2 in self.each_q(col) if self.in_text(self.q(at2 + 8))]
        return out


def classify(n, win):
    if not win:
        arr = 'VarArrayI' in n or 'Lb1E' in n
        non = 'CInterpolatedVarI' in n or 'Lb0E' in n
    else:
        arr = 'VarArray@' in n or '$00@' in n
        non = n.startswith('.?AV?$CInterpolatedVar@') or '$0A@' in n
    if arr == non:
        return None
    if arr:
        return 16, True
    if ('VVector@@' if win else '6Vector') in n or ('VQAngle@@' if win else '6QAngle') in n:
        return 16, False
    if ('VC_AnimationLayer@@' if win else '16C_AnimationLayer') in n:
        return 48, False
    if (('@M@' in n or '@M$' in n) if win else ('IfE' in n or 'IfL' in n)):
        return 8, False
    return None


def check_client(c):
    ok = True
    slot_i, slot_nc = (6, 4) if c.win else (7, 5)
    thunk = bytes.fromhex('f30f105138' if c.win else 'f30f104f38')
    rx = rb'\.\?AV\?\$CInterpolatedVar[^\x00]*' if c.win else rb'[1-9][0-9]*CInterpolatedVar[^\x00]*'
    types = {}   # impl -> [stride, array, names, nc slots]
    for addr, name in c.strings(rx):
        cl = classify(name, c.win)
        if not cl:
            print(f'  ? {name}: unknown element type')
            continue
        for vt in c.vtables(addr):
            fn = c.q(vt + 8 * slot_i)
            if c.b(fn, 5) != thunk or c.b(fn + 5, 1) != b'\xe9':
                print(f'  ! {name}: Interpolate slot {slot_i} is not the thunk ({c.b(fn, 8).hex()})')
                ok = False
                continue
            impl = fn + 10 + c.s32(fn + 6)
            t = types.setdefault(impl, [cl[0], cl[1], [], set()])
            if (t[0], t[1]) != cl:
                print(f'  ! {name}: shares impl {impl:#x} with a different layout')
                ok = False
            t[2].append(name)
            t[3].add(c.q(vt + 8 * slot_nc))
    sites = {i: [] for i in types}
    for lo, hi in c.text:
        blob = c.mem[lo - c.base:hi - c.base]
        for m in re.finditer(rb'[\xe8\xe9]', blob):
            a = lo + m.start()
            if a + 5 > hi:
                continue
            tgt = a + 5 + struct.unpack_from('<i', blob, m.start() + 1)[0]
            if tgt in sites:
                sites[tgt].append(('call' if blob[m.start()] == 0xe8 else 'jmp', a))
    for impl, (stride, arr, names, ncs) in sorted(types.items()):
        s = sites[impl]
        calls = [f'{k} {a:#x}' for k, a in s]
        print(f'  impl {impl:#x} {min(names, key=len)}: {stride}-byte samples{" (array)" if arr else ""}, '
              f'{len(names)} vtables, NoteChanged {", ".join(hex(x) for x in sorted(ncs))}, sites: {", ".join(calls)}')
        if not s:
            ok = False
    print(f'  {len(types)} implementations, {sum(len(v) for v in sites.values())} call sites')
    if len(types) != 7:
        print('  ! expected 7 implementations')
        ok = False
    for nm in ([r'\.\?AVC_PredictedViewModel@@', r'\.\?AVC_BaseViewModel@@'] if c.win else [r'20C_PredictedViewModel', r'15C_BaseViewModel']):
        hits = [(a, n) for a, n in c.strings(nm.encode())]
        vts = [v for a, _ in hits for v in c.vtables(a)]
        print(f'  gun class {nm.replace(chr(92), "")}: {len(vts)} vtable(s)')
    # the gun's Interpolate: overridden by C_BaseEntity, C_BaseAnimating and C_BaseViewModel, the viewmodel one calls
    # the C_BaseAnimating one and computes interpolation_amount * interval
    def one_vt(name):
        for a, _ in c.strings(name.encode()):
            v = c.vtables(a)
            if len(v) == 1:
                return v[0]
    names = ([r'\.\?AVC_BaseEntity@@', r'\.\?AVC_BaseAnimating@@', r'\.\?AVC_BaseViewModel@@'] if c.win
             else ['12C_BaseEntity', '15C_BaseAnimating', '15C_BaseViewModel'])
    ent, anim, bvm = (one_vt(n) for n in names)
    hits = []
    for i in range(500):
        if not (ent and anim and bvm):
            break
        E, A, V = c.q(ent + 8 * i), c.q(anim + 8 * i), c.q(bvm + 8 * i)
        if not (c.in_text(E) and c.in_text(A) and c.in_text(V)):
            break
        if len({E, A, V}) < 3:
            continue
        b = c.b(V, 800)
        calls = any(b[k] == 0xe8 and V + k + 5 + struct.unpack_from('<i', b, k + 1)[0] == A for k in range(len(b) - 5))
        amt = any(b[k:k + 3] == b'\xf3\x0f\x10' and b[k + 3] & 0xc0 == 0x40 and b[k + 4] == 0x20 for k in range(len(b) - 5))
        ival = any(b[k:k + 3] == b'\xf3\x0f\x59' and b[k + 3] & 0xc0 == 0x40 and b[k + 4] == 0x1c for k in range(len(b) - 5))
        if calls and amt and ival:
            hits.append((i, V))
    if len(hits) == 1:
        print(f'  gun animation: C_BaseViewModel::Interpolate = slot {hits[0][0]} ({hits[0][1]:#x})')
    else:
        print(f'  ! gun animation slot: {len(hits)} candidates (the gun-animation part would stay off)')
    return ok


def decode_acc(e, fn):
    if not e.in_text(fn):
        return None
    b = e.b(fn, 16)
    if b[0] == 0x48 and b[1] in (0x8d, 0x8b) and b[2] == 0x05:
        base = fn + 7 + struct.unpack_from('<i', b, 3)[0]
        if b[1] == 0x8b:
            return None   # pointer: only known at run time
        c = b[7:]
        if c[:4] == b'\xf3\x0f\x10\x40' and c[5] == 0xc3: return 1, base + c[4]
        if c[:2] == b'\x8b\x40' and c[3] == 0xc3: return 2, base + c[2]
        if c[:4] == b'\xf3\x0f\x11\x40' and c[5] == 0xc3: return 3, base + c[4]
        return None
    if b[:3] == b'\xf3\x0f\x10' and b[3] & 0xc7 == 5 and b[8] == 0xc3: return 1, fn + 8 + struct.unpack_from('<i', b, 4)[0]
    if b[0] == 0x8b and b[1] & 0xc7 == 5 and b[6] == 0xc3: return 2, fn + 6 + struct.unpack_from('<i', b, 2)[0]
    if b[:3] == b'\xf3\x0f\x11' and b[3] & 0xc7 == 5 and b[8] == 0xc3: return 3, fn + 8 + struct.unpack_from('<i', b, 4)[0]
    return None


def check_engine(e):
    name = rb'\.\?AVCEngineTool@@' if e.win else rb'11CEngineTool'
    found = []
    for addr, _ in e.strings(name):
        for vt in e.vtables(addr):
            acc = []
            for s in range(200):
                fn = e.q(vt + 8 * s)
                if not e.in_text(fn):
                    break
                acc.append(decode_acc(e, fn))
            for s in range(len(acc) - 3):
                a = acc[s]
                if not a or a[0] != 1:
                    continue
                A = a[1] - 0xc
                if acc[s + 1] == (1, A + 0x10) and acc[s + 2] == (2, A + 0x18) and acc[s + 3] == (3, A + 0x10):
                    found.append((vt, s, A))
    for vt, s, A in found:
        print(f'  clock struct {A:#x} (IEngineTool vtable {vt:#x}, slots {s}-{s + 3})')
    if len(found) != 1:
        print(f'  ! expected exactly one clock accessor quartet, found {len(found)}')
    return len(found) == 1


def main():
    if len(sys.argv) == 3:
        cp, ep = sys.argv[1:3]
    else:
        g = os.path.expanduser('~/.local/share/Steam/steamapps/common/Counter-Strike Source')
        cp, ep = f'{g}/cstrike/bin/linux64/client.so', f'{g}/bin/linux64/engine.so'
    print(f'client: {cp}')
    ok = check_client(Image(cp))
    print(f'engine: {ep}')
    ok = check_engine(Image(ep)) and ok
    print('OK: curtimefix should find everything' if ok else 'PROBLEM: curtimefix would stay off or skip parts')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
