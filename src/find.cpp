// curtimefix: everything is located at runtime from names and code shapes, never from fixed addresses.
//   - CInterpolatedVar* classes by their RTTI names → vtables → the Interpolate thunk → the 7 implementations
//   - every rel32 call/jmp into an implementation (the thunks and direct callers such as the gun sway)
//   - the client's CGlobalVarsBase from the engine's IEngineTool ClientTime/ClientFrameTime/ClientTick/
//     SetClientFrameTime accessors (four consecutive tiny functions on one struct)
#include "ctf.h"
#include <cstring>
#include <cstdio>
#include <vector>

#ifdef _WIN32
static const bool WIN = true;
#else
static const bool WIN = false;
#endif

int vt_slot(int which)
{
    // IInterpolatedVar: dtor, Setup, SetInterpolationAmount, NoteLastNetworkedValue, NoteChanged, Reset, Interpolate ...
    // Itanium has two destructor slots, MSVC one.
    if (which == VT_INTERPOLATE) return WIN ? 6 : 7;
    return WIN ? 4 : 5;
}

static uint64_t rd64(uintptr_t a) { uint64_t v; memcpy(&v, (const void *)a, 8); return v; }
static uint32_t rd32(uintptr_t a) { uint32_t v; memcpy(&v, (const void *)a, 4); return v; }
static int32_t rds32(uintptr_t a) { int32_t v; memcpy(&v, (const void *)a, 4); return v; }

static bool in_data(const ModInfo &m, uintptr_t a, size_t n)
{
    for (int i = 0; i < m.ndata; i++) if (a >= m.data[i].lo && a + n <= m.data[i].hi) return true;
    for (int i = 0; i < m.ntext; i++) if (a >= m.text[i].lo && a + n <= m.text[i].hi) return true;
    return false;
}

// every 8-aligned qword in the data ranges equal to v
template <typename F> static void each_q(const ModInfo &m, uint64_t v, F fn)
{
    for (int r = 0; r < m.ndata; r++)
        for (uintptr_t a = (m.data[r].lo + 7) & ~(uintptr_t)7; a + 8 <= m.data[r].hi; a += 8)
            if (*(const uint64_t *)a == v) fn(a);
}

template <typename F> static void each_d(const ModInfo &m, uint32_t v, F fn)
{
    for (int r = 0; r < m.ndata; r++)
        for (uintptr_t a = (m.data[r].lo + 3) & ~(uintptr_t)3; a + 4 <= m.data[r].hi; a += 4)
            if (*(const uint32_t *)a == v) fn(a);
}

// start addresses of NUL-terminated strings in the data ranges that contain `needle` and satisfy ok(start)
static std::vector<uintptr_t> strings_with(const ModInfo &m, const char *needle)
{
    std::vector<uintptr_t> out;
    size_t nl = strlen(needle);
    for (int r = 0; r < m.ndata; r++) {
        const char *lo = (const char *)m.data[r].lo, *hi = (const char *)m.data[r].hi;
        for (const char *p = lo; p + nl <= hi;) {
            const char *q = (const char *)memchr(p, needle[0], hi - p);
            if (!q || q + nl > hi) break;
            if (!memcmp(q, needle, nl)) {
                const char *s = q;
                while (s > lo && s[-1] != 0 && s - q > -128) s--;
                if (s > lo && s[-1] == 0) {
                    bool dup = false;
                    for (uintptr_t o : out) if (o == (uintptr_t)s) dup = true;
                    if (!dup) out.push_back((uintptr_t)s);
                }
            }
            p = q + 1;
        }
    }
    return out;
}

// all primary vtables of the class whose RTTI name string starts at `name`
static int vtables_of(const ModInfo &m, uintptr_t name, uintptr_t *out, int max)
{
    int n = 0;
    if (!WIN) {
        // typeinfo = { vptr, const char *name, ... }; vtable = { offset_to_top (0), &typeinfo, slot0 ... }
        each_q(m, name, [&](uintptr_t at) {
            uintptr_t ti = at - 8;
            each_q(m, ti, [&](uintptr_t at2) {
                if (n >= max || rd64(at2 - 8) != 0 || !m.in_text((uintptr_t)rd64(at2 + 8))) return;
                out[n++] = at2 + 8;
            });
        });
    } else {
        // TypeDescriptor = { pVFTable, spare, name[] }; COL = { 1, offset, cdOffset, TD rva, CHD rva, self rva };
        // vtable[-1] = &COL
        uintptr_t td = name - 16;
        each_d(m, (uint32_t)(td - m.base), [&](uintptr_t at) {
            uintptr_t col = at - 12;
            if (!in_data(m, col, 24) || rd32(col) != 1 || rd32(col + 4) != 0 || rd32(col + 20) != (uint32_t)(col - m.base)) return;
            each_q(m, col, [&](uintptr_t at2) {
                if (n >= max || !m.in_text((uintptr_t)rd64(at2 + 8))) return;
                out[n++] = at2 + 8;
            });
        });
    }
    return n;
}

static uintptr_t find_name(const ModInfo &m, const char *exact)
{
    for (uintptr_t s : strings_with(m, exact))
        if (!strcmp((const char *)s, exact)) return s;
    return 0;
}

// element type → bytes per history entry. Arrays store { float changetime; int count; T *value } = 16.
static bool classify(const char *n, int *stride, bool *array)
{
    bool arr, non;
    if (!WIN) {
        arr = strstr(n, "VarArrayI") || strstr(n, "Lb1E");
        non = (strstr(n, "CInterpolatedVarI") != nullptr) || strstr(n, "Lb0E");
    } else {
        arr = strstr(n, "VarArray@") || strstr(n, "$00@");
        non = !strncmp(n, ".?AV?$CInterpolatedVar@", 23) || strstr(n, "$0A@");
    }
    if (arr == non) return false;
    *array = arr;
    if (arr) { *stride = 16; return true; }
    if (strstr(n, WIN ? "VVector@@" : "6Vector") || strstr(n, WIN ? "VQAngle@@" : "6QAngle")) *stride = 16;
    else if (strstr(n, WIN ? "VC_AnimationLayer@@" : "16C_AnimationLayer")) *stride = 48;
    else if (WIN ? (strstr(n, "@M@") || strstr(n, "@M$")) : (strstr(n, "IfE") || strstr(n, "IfL"))) *stride = 8;
    else return false;
    return true;
}

// the Interpolate slot: movss xmm,[this+0x38] (m_InterpolationAmount); jmp impl
static uintptr_t thunk_target(const ModInfo &m, uintptr_t fn)
{
    if (!m.in_text(fn) || !m.in_text(fn + 9)) return 0;
    const uint8_t *b = (const uint8_t *)fn;
    static const uint8_t lin[5] = {0xF3, 0x0F, 0x10, 0x4F, 0x38};   // movss xmm1, [rdi+0x38]
    static const uint8_t win[5] = {0xF3, 0x0F, 0x10, 0x51, 0x38};   // movss xmm2, [rcx+0x38]
    if (memcmp(b, WIN ? win : lin, 5) || b[5] != 0xE9) return 0;
    uintptr_t t = fn + 10 + rds32(fn + 6);
    return m.in_text(t) ? t : 0;
}

// IEngineTool accessor shapes. kind 1 = float load, 2 = int load, 3 = float store; ea = the field's address
struct Acc { int kind; uintptr_t ea; };
static Acc decode_acc(const ModInfo &e, uintptr_t fn)
{
    Acc a = {0, 0};
    if (!e.in_text(fn) || !e.in_text(fn + 14)) return a;
    const uint8_t *b = (const uint8_t *)fn;
    if (b[0] == 0x48 && (b[1] == 0x8D || b[1] == 0x8B) && b[2] == 0x05) {   // lea/mov rax, [rip+d]; op [rax+d8]; ret
        uintptr_t base = fn + 7 + rds32(fn + 3);
        if (b[1] == 0x8B) { if (!in_data(e, base, 8)) return a; base = (uintptr_t)rd64(base); }
        const uint8_t *c = b + 7;
        if (c[0] == 0xF3 && c[1] == 0x0F && c[2] == 0x10 && c[3] == 0x40 && c[5] == 0xC3) a = {1, base + c[4]};
        else if (c[0] == 0x8B && c[1] == 0x40 && c[3] == 0xC3) a = {2, base + c[2]};
        else if (c[0] == 0xF3 && c[1] == 0x0F && c[2] == 0x11 && c[3] == 0x40 && c[5] == 0xC3) a = {3, base + c[4]};
        return a;
    }
    // op [rip+d32]; ret
    if (b[0] == 0xF3 && b[1] == 0x0F && b[2] == 0x10 && (b[3] & 0xC7) == 0x05 && b[8] == 0xC3) a = {1, fn + 8 + rds32(fn + 4)};
    else if (b[0] == 0x8B && (b[1] & 0xC7) == 0x05 && b[6] == 0xC3) a = {2, fn + 6 + rds32(fn + 2)};
    else if (b[0] == 0xF3 && b[1] == 0x0F && b[2] == 0x11 && (b[3] & 0xC7) == 0x05 && b[8] == 0xC3) a = {3, fn + 8 + rds32(fn + 4)};
    return a;
}

static uintptr_t find_clock(const ModInfo &e, void *enginetool)
{
    if (!enginetool) return 0;
    uintptr_t *vt = *(uintptr_t **)enginetool;
    Acc acc[200];
    int n = 0;
    for (; n < 200 && in_data(e, (uintptr_t)&vt[n], 8) && e.in_text(vt[n]); n++) acc[n] = decode_acc(e, vt[n]);
    uintptr_t found = 0;
    int hits = 0;
    for (int s = 0; s + 3 < n; s++) {
        if (acc[s].kind != 1) continue;
        uintptr_t A = acc[s].ea - 0x0c;
        if (acc[s + 1].kind == 1 && acc[s + 1].ea == A + 0x10 && acc[s + 2].kind == 2 && acc[s + 2].ea == A + 0x18
            && acc[s + 3].kind == 3 && acc[s + 3].ea == A + 0x10) {
            found = A;
            hits++;
            say("clock: IEngineTool slots %d-%d", s, s + 3);
        }
    }
    return hits == 1 && in_data(e, found, 0x24) ? found : 0;
}

bool find_all(const ModInfo &c, const ModInfo &e, void *enginetool, Found *f)
{
    // 1. CInterpolatedVar classes → vtables → implementations
    for (uintptr_t s : strings_with(c, WIN ? ".?AV?$CInterpolatedVar" : "CInterpolatedVar")) {
        const char *name = (const char *)s;
        if (WIN ? strncmp(name, ".?AV?$CInterpolatedVar", 22) : !(name[0] >= '1' && name[0] <= '9')) continue;
        int stride;
        bool array;
        if (!classify(name, &stride, &array)) { say("skipping %s (unknown element type)", name); continue; }
        uintptr_t vts[4];
        int nv = vtables_of(c, s, vts, 4);
        for (int i = 0; i < nv; i++) {
            uintptr_t impl = thunk_target(c, rd64(vts[i] + 8 * vt_slot(VT_INTERPOLATE)));
            if (!impl) { say("%s: Interpolate slot has an unexpected shape, skipped", name); continue; }
            int t = 0;
            for (; t < f->ntypes && f->types[t].impl != impl; t++) {}
            if (t == f->ntypes) {
                if (f->ntypes == 8) continue;
                VarType &vt = f->types[f->ntypes++];
                snprintf(vt.name, sizeof vt.name, "%s", name);
                vt.stride = stride;
                vt.array = array;
                vt.impl = impl;
            } else if (f->types[t].stride != stride || f->types[t].array != array) {
                say("%s and %s share an implementation but not a layout; that type stays unfixed", name, f->types[t].name);
                f->types[t].stride = -1;
            }
            if (f->nvts < 32) f->vts[f->nvts++] = {vts[i], t};
        }
    }
    // 2. every rel32 call/jmp into an implementation
    for (int r = 0; r < c.ntext; r++)
        for (uintptr_t a = c.text[r].lo; a + 5 <= c.text[r].hi; a++) {
            uint8_t op = *(const uint8_t *)a;
            if (op != 0xE8 && op != 0xE9) continue;
            uintptr_t tgt = a + 5 + rds32(a + 1);
            for (int t = 0; t < f->ntypes; t++)
                if (tgt == f->types[t].impl && f->nsites < 32) { f->site_type[f->nsites] = t; f->sites[f->nsites++] = a; }
        }
    // 3. the gun's classes (meter)
    const char *vmn[2] = {WIN ? ".?AVC_PredictedViewModel@@" : "20C_PredictedViewModel", WIN ? ".?AVC_BaseViewModel@@" : "15C_BaseViewModel"};
    for (const char *nm : vmn)
        if (uintptr_t s = find_name(c, nm)) f->nvm += vtables_of(c, s, f->vm_vt + f->nvm, 4 - f->nvm);
    // 4. C_BaseViewModel::Interpolate: the one slot overridden by C_BaseEntity, C_BaseAnimating and C_BaseViewModel
    //    alike whose viewmodel version calls the C_BaseAnimating one and computes interpolation_amount * interval
    //    (globals +0x20 times +0x1c) for the cycle
    uintptr_t ent = 0, anim = 0, bvm = 0, tmp[4];
    if (uintptr_t s = find_name(c, WIN ? ".?AVC_BaseEntity@@" : "12C_BaseEntity")) if (vtables_of(c, s, tmp, 4) == 1) ent = tmp[0];
    if (uintptr_t s = find_name(c, WIN ? ".?AVC_BaseAnimating@@" : "15C_BaseAnimating")) if (vtables_of(c, s, tmp, 4) == 1) anim = tmp[0];
    if (uintptr_t s = find_name(c, WIN ? ".?AVC_BaseViewModel@@" : "15C_BaseViewModel")) if (vtables_of(c, s, tmp, 4) == 1) bvm = tmp[0];
    int hits = 0;
    for (int i = 0; ent && anim && bvm && i < 500; i++) {
        uintptr_t E = rd64(ent + 8 * i), A = rd64(anim + 8 * i), V = rd64(bvm + 8 * i);
        if (!c.in_text(E) || !c.in_text(A) || !c.in_text(V)) break;
        if (E == A || A == V || E == V || !c.in_text(V + 800)) continue;
        const uint8_t *b = (const uint8_t *)V;
        bool calls = false, amt = false, ival = false;
        for (int k = 0; k + 5 < 800; k++) {
            if (b[k] == 0xE8 && V + k + 5 + rds32(V + k + 1) == A) calls = true;
            if (b[k] == 0xF3 && b[k + 1] == 0x0F && (b[k + 3] & 0xC0) == 0x40) {
                if (b[k + 2] == 0x10 && b[k + 4] == 0x20) amt = true;
                if (b[k + 2] == 0x59 && b[k + 4] == 0x1C) ival = true;
            }
        }
        if (calls && amt && ival) { f->vm_slot = i; f->vm_fn = V; hits++; }
    }
    if (hits != 1) { f->vm_slot = -1; f->vm_fn = 0; }
    // 5. the clock
    f->clock = find_clock(e, enginetool);
    return f->ntypes > 0 && f->clock;
}
