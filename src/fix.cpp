// curtimefix — CS:S renders as if the map had just loaded, at any map age.
//
// The client keeps map time as float seconds since map load. Each frame it blends every moving thing between
// stored samples (CInterpolatedVar), with times of that size; past ~1 h the float step is a large part of a
// tick and the blend gets quantized. Here, around every Interpolate(float t, float amount) the game makes:
//   1. recognise t as this frame's render time ((tick + interpolation_amount) * interval, exactly), else leave
//      the call alone;
//   2. shift that time and every stored sample time by the same base so they are about 1 s, recovering each
//      sample's exact time (tick multiples; this frame's render time from a per-object record);
//   3. run the game's own Interpolate on those small exact numbers, then put every stored time back bit-exact.
// Plus NoteChanged: the game's "drop a sample with the same time" test (t + 0.0001f > new) stops working past
// 34 min; it is done here in exact time before the game's own runs.
#include "ctf.h"
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------- logging
typedef void (*ConMsg_t)(const char *fmt, ...);
static ConMsg_t g_ConMsg;

void say(const char *fmt, ...)
{
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_ConMsg) g_ConMsg("[curtimefix] %s\n", buf);
    fprintf(stderr, "[curtimefix] %s\n", buf);   // also the terminal / launcher log
}

// ---------------------------------------------------------------- game data
struct Clock {   // CGlobalVarsBase (the start of it)
    float realtime;
    int framecount;
    float absoluteframetime;
    float curtime;              // +0x0c
    float frametime;
    int maxClients;
    int tickcount;              // +0x18
    float interval_per_tick;    // +0x1c
    float interpolation_amount; // +0x20
};

// CInterpolatedVarArrayBase (same on both ABIs, checked in the binaries)
enum { IV_VALUE = 0x08, IV_ELEMS = 0x10, IV_MAX = 0x18, IV_FIRST = 0x1a, IV_COUNT = 0x1c };
static inline uint8_t *iv_elems(void *o) { return *(uint8_t **)((uint8_t *)o + IV_ELEMS); }
static inline uint16_t &iv_u16(void *o, int off) { return *(uint16_t *)((uint8_t *)o + off); }

typedef int (*Interp_t)(void *self, float t, float amount);
typedef bool (*NoteChanged_t)(void *self, float changetime, bool update_last_networked);

struct TypeRT {
    int stride;
    bool array, ok, angles;
    Interp_t orig;
    char name[96];
};
static TypeRT g_type[8];
static int g_ntypes;

struct VtRT { uintptr_t vt; int type; NoteChanged_t orig_nc; };
static VtRT g_vt[32];
static int g_nvt;

static Found g_found;
static void *g_client, *g_engine, *g_entlist;
static const Clock *g_clock;
static std::atomic<bool> g_on{true};
static bool g_installed;
static thread_local bool t_main;

// ---------------------------------------------------------------- stats (relaxed: bone-setup threads call too)
enum {
    S_CALLS, S_GATE_CUR, S_GATE_PRED, S_PASS, S_PASS_BADCLOCK, S_PASS_ORDER, S_PASS_BIG, S_PASS_ZERO,
    S_E_TICK, S_E_SHADOW, S_E_PLAIN, S_E_ZERO, S_FLUSH_EXTRA, S_NC_CALLS, S_SHADOW_REC, S_OFF, S_VM, S_VM_PASS, S_VM_CALLS,
    S_RET_KEPT, S_N
};
static std::atomic<uint64_t> g_st[S_N];
static inline void st(int i, uint64_t n = 1) { g_st[i].fetch_add(n, std::memory_order_relaxed); }

static inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double ulp(float f) { f = fabsf(f); return (double)nextafterf(f, INFINITY) - (double)f; }

// ---------------------------------------------------------------- exact render time
struct Gate { int kind; long long k; double I, T; float a; };   // kind 1 = curtime, 2 = predicted local player
static const long long g_K1 = 128;   // shift so the render time lands 128 ticks in: 1-2 s at any tickrate, never <= 0
static thread_local long long t_shift;   // ticks taken off the predicted tick while the gun's Interpolate runs (D)
static thread_local int t_win_bad;       // ... and how many calls in that window went to the game untouched

static bool gate(float t, Gate *g)
{
    const Clock *c = g_clock;
    float If = c->interval_per_tick, a = c->interpolation_amount, cur = c->curtime;
    if (!(If > 0.0009f && If < 0.2f) || !(a >= 0.0f && a <= 1.0f)) return false;
    double I = If;
    if (fbits(t) == fbits(cur)) {
        // render time: (k + the frame's fraction of a tick) * I for the client tick k curtime was built from. That is
        // not always gpGlobals->tickcount (on a dedicated server it was one tick behind in a long session and up to
        // two ahead after a map load), so k comes from curtime itself; trust it only if it reproduces curtime.
        double kd = nearbyint((double)cur / I - (double)a), tol = 1.5 * ulp(cur) + 1e-7;
        for (int d = 0; d < 3; d++) {
            double k = kd + (d == 0 ? 0 : d == 1 ? -1 : 1);
            if (k < 1 || k > 16777216.0) continue;
            double T = (k + (double)a) * I;
            if (fabs(T - (double)cur) <= tol) { *g = {1, (long long)k, I, T, a}; return true; }
        }
        st(S_PASS_BADCLOCK);
        return false;
    }
    // predicted local player: fl(fl(fl(N*I) - I) + fl(a*I)), the game's own float steps (BaseInterpolatePart1)
    double n0 = nearbyint((double)t / I - (double)a + 1.0);
    for (int d = 0; d < 3; d++) {
        double nd = n0 + (d == 0 ? 0 : d == 1 ? -1 : 1);
        if (nd < 1 || nd > 16777216.0) continue;
        volatile float f = (float)nd * If;
        f = f - If;
        volatile float ai = a * If;
        f = f + ai;
        if (fbits(f) == fbits(t)) {
            long long k = (long long)nd - 1 + t_shift;
            *g = {2, k, I, ((double)k + (double)a) * I, a};
            return true;
        }
    }
    return false;
}

// a stored time that is a tick multiple (within one float step: prediction resets store fl(fl(t*I) - I))
static inline bool tick_time(float ct, const Gate &g, double *ex)
{
    double jd = nearbyint((double)ct / g.I);
    if (!(fabs(jd) < 16777216.0)) return false;
    volatile float e = (float)jd * (float)g.I;
    if (e == ct || nextafterf(e, INFINITY) == ct || nextafterf(e, -INFINITY) == ct) { *ex = jd * g.I; return true; }
    return false;
}

// ---------------------------------------------------------------- shadow: exact times of render-time samples
// The gun sway history (and client-side animation) stores fl(curtime) of the frame it was latched in; at
// 1000 fps two frames can share one float. Main thread only.
struct Shadow {
    void *obj, *value;
    uint64_t used;
    int n, head;                 // ring, head = newest
    float ct[256];
    double T[256];
};
static Shadow g_sh[24];
static uint64_t g_sh_clock;

static Shadow *shadow_find(void *obj, void *value)
{
    for (Shadow &s : g_sh)
        if (s.obj == obj) {
            if (s.value != value) { s.n = 0; s.value = value; }
            s.used = ++g_sh_clock;
            return &s;
        }
    return nullptr;
}

static void shadow_record(void *obj, void *value, float ct, double T)
{
    Shadow *s = shadow_find(obj, value);
    if (!s) {
        s = &g_sh[0];
        for (Shadow &x : g_sh) if (x.used < s->used) s = &x;
        s->obj = obj; s->value = value; s->n = 0; s->head = 0; s->used = ++g_sh_clock;
    }
    if (s->n && fbits(s->ct[s->head]) == fbits(ct) && s->T[s->head] == T) return;   // same frame again
    s->head = (s->head + 1) & 255;
    s->ct[s->head] = ct;
    s->T[s->head] = T;
    if (s->n < 256) s->n++;
    st(S_SHADOW_REC);
}

// ---------------------------------------------------------------- trace (CURTIMEFIX_TRACE=1): one stderr line per frame
static bool g_trace;
static void *g_trace_value;            // the local player's m_vecOrigin: its history gets traced
static uint8_t *g_trace_lp;            // the local player (candidates must point into it)
static void *g_trace_cand[16]; static int g_trace_ncand;
struct TraceFrame { int calls, kind, count, ntick, nsh, nplain; double worst_ulps; float t; long long k; float ct[4]; };
static TraceFrame g_tf;

// ---------------------------------------------------------------- the Interpolate sandwich
enum { MAXH = 512 };

// self-test: run the untouched path too and compare (Vector/QAngle, main thread)
static std::atomic<bool> g_selftest{false};
static double g_sf_max, g_sf_sum;
static uint64_t g_sf_n, g_sf_retdiff, g_sf_over1, g_sf_over2;   // differences > 0.01 / > 0.1 units
static int g_sf_logged;                                         // detailed reports printed this run (max 15)
static uint64_t g_sf_float, g_sf_jump, g_sf_odd;                // over 0.01: float rounding / across a teleport / neither
static uint64_t g_sf_ret_early, g_sf_ret_late;                  // "no more changes" where the game says 0 / says 1
static void *local_player();

// the game's own Interpolate, untouched (inside the gun window that would mix a shifted time with real samples)
static inline int pass(TypeRT &ty, void *self, float t, float amount)
{
    if (t_shift) t_win_bad++;
    return ty.orig(self, t, amount);
}

static int sandwich(int ti, void *self, float t, float amount, const Gate &g)
{
    TypeRT &ty = g_type[ti];
    uint8_t *el = iv_elems(self);
    int maxe = iv_u16(self, IV_MAX), first = iv_u16(self, IV_FIRST), count = iv_u16(self, IV_COUNT);
    if (!el || count <= 0 || count > maxe || first >= maxe) return pass(ty, self, t, amount);
    if (count > MAXH) { st(S_PASS_BIG); return pass(ty, self, t, amount); }

    double B = ((double)(g.k - g_K1) + 0.5) * g.I;   // half a tick: no shifted tick time can be exactly 0.0
    float *ptr[MAXH];
    float saved[MAXH], nv[MAXH];
    Shadow *sh = t_main ? shadow_find(self, *(void **)((uint8_t *)self + IV_VALUE)) : nullptr;
    int r = sh ? sh->head : 0, rleft = sh ? sh->n : 0;
    double prev = INFINITY;
    int ntick = 0, nsh = 0, nplain = 0, nzero = 0;
    for (int i = 0; i < count; i++) {
        int slot = first + i;
        if (slot >= maxe) slot -= maxe;
        float *p = (float *)(el + (size_t)slot * ty.stride);
        float ct = *p;
        ptr[i] = p;
        saved[i] = ct;
        if (ct == 0.0f) { nv[i] = ct; nzero++; continue; }   // the game's "empty" marker stays
        double ex;
        bool have = false;
        if (sh) {   // newest record first, matched in order (entries are newest first too)
            for (int rr = r, left = rleft; left > 0; left--, rr = (rr - 1) & 255)
                if (fbits(sh->ct[rr]) == fbits(ct)) { ex = sh->T[rr]; have = true; r = (rr - 1) & 255; rleft = left - 1; break; }
            if (have) nsh++;
        }
        if (!have && tick_time(ct, g, &ex)) { have = true; ntick++; }
        if (!have) {
            ex = (double)ct; nplain++;
            if (g_trace && t_main && *(void **)((uint8_t *)self + IV_VALUE) == g_trace_value) {
                double jd = nearbyint((double)ct / g.I);
                double off = fabs((double)ct - (double)((float)jd * (float)g.I)) / ulp(ct);
                if (off > g_tf.worst_ulps) g_tf.worst_ulps = off;
            }
        }
        if (ex > prev + 2 * ulp(ct)) { st(S_PASS_ORDER); return pass(ty, self, t, amount); }   // not what we think it is
        prev = ex;
        nv[i] = (float)(ex - B);
        if (nv[i] == 0.0f) { st(S_PASS_ZERO); return pass(ty, self, t, amount); }
    }
    float t2 = (float)(g.T - B);
    if (g_trace && t_main && !g_trace_value && g.kind == 2 && ty.stride == 16 && !ty.array && !ty.angles && g_trace_lp) {
        uint8_t *v = *(uint8_t **)((uint8_t *)self + IV_VALUE);
        if (v >= g_trace_lp && v < g_trace_lp + 0x4000 && g_trace_ncand < 16) {
            bool dup = false;
            for (int i = 0; i < g_trace_ncand; i++) dup |= g_trace_cand[i] == v;
            if (!dup) g_trace_cand[g_trace_ncand++] = v;
        }
    }
    if (g_trace && t_main && *(void **)((uint8_t *)self + IV_VALUE) == g_trace_value) {
        g_tf.calls++; g_tf.kind = g.kind; g_tf.count = count; g_tf.ntick += ntick; g_tf.nsh += nsh; g_tf.nplain += nplain;
        g_tf.t = t; g_tf.k = g.k;
        for (int i = 0; i < 4; i++) g_tf.ct[i] = i < count ? saved[i] : 0;
    }

    // self-test: the game's own result first, then rewind value and count
    float A[3];
    int retA = 0;
    bool test = g_selftest.load(std::memory_order_relaxed) && t_main && !ty.array && ty.stride == 16 && !t_shift;
    if (test) {
        float *val = *(float **)((uint8_t *)self + IV_VALUE);
        float v0[3];
        memcpy(v0, val, 12);
        retA = ty.orig(self, t, amount);
        memcpy(A, val, 12);
        memcpy(val, v0, 12);
        iv_u16(self, IV_COUNT) = (uint16_t)count;
    }

    for (int i = 0; i < count; i++) *ptr[i] = nv[i];
    int ret = ty.orig(self, t2, amount);
    for (int i = 0; i < count; i++) *ptr[i] = saved[i];   // Interpolate only truncates: every slot is still ours
    // The return value is "no more changes": with it the game may take the entity off its interpolation list. The game
    // says 1 when the target time (t - amount) is at or past the newest sample, or when it lies between the newest two
    // samples and they (and the third for a spline) are identical. At a tick boundary the exact target and the game's
    // float target can land on different sides of a sample; say 1 only where the game's own target would too. Keeping
    // an entity on the list one more frame is harmless, dropping it early is not.
    if (ret == 1 && saved[0] != 0.0f) {
        volatile float tt_ours = t2 - amount, tt_game = t - amount;
        bool ours_past = !(nv[0] > tt_ours), game_past = !(saved[0] > tt_game);
        bool game_at_head = count >= 2 && saved[1] != 0.0f && !(saved[1] > tt_game);
        if (!game_past && (ours_past || !game_at_head)) { ret = 0; st(S_RET_KEPT); }
    }

    st(S_E_TICK, ntick); st(S_E_SHADOW, nsh); st(S_E_PLAIN, nplain); st(S_E_ZERO, nzero);
    if (test) {
        float *val = *(float **)((uint8_t *)self + IV_VALUE);
        double d = 0;
        for (int k = 0; k < 3; k++) {
            double x = (double)val[k] - (double)A[k];
            if (ty.angles) x -= 360.0 * nearbyint(x / 360.0);   // 359.99 and -0.01 are the same angle
            d = fmax(d, fabs(x));
        }
        g_sf_max = fmax(g_sf_max, d);
        if (d > 0.01) g_sf_over1++;
        if (d > 0.1) g_sf_over2++;
        g_sf_sum += d;
        g_sf_n++;
        if (ret != retA) { g_sf_retdiff++; if (ret) g_sf_ret_early++; else g_sf_ret_late++; }
        // Sort a difference: what the game's float times explain (the fastest change between neighbouring samples x
        // 3 float steps at t), a jump in the samples (a teleport: both blend across it, at slightly different times),
        // or neither. Only "neither" and an early "no more changes" would be our bug.
        double vmax = 0;
        bool jump = false;
        for (int i = 0; i + 1 < count; i++) {
            if (nv[i] == saved[i] || nv[i + 1] == saved[i + 1] || !(nv[i] > nv[i + 1])) continue;
            const float *e0 = ptr[i], *e1 = ptr[i + 1];
            double s = 0;
            for (int k = 0; k < 3; k++) {
                double x = (double)e0[1 + k] - (double)e1[1 + k];
                if (ty.angles) x -= 360.0 * nearbyint(x / 360.0);
                s += x * x;
            }
            if (sqrt(s) > 64.0) jump = true;
            vmax = fmax(vmax, sqrt(s) / ((double)nv[i] - (double)nv[i + 1]));
        }
        bool odd = false;
        if (d > 0.01) {
            if (jump) g_sf_jump++;
            else if (d <= 0.01 + vmax * 3.0 * ulp(t)) g_sf_float++;
            else { g_sf_odd++; odd = true; }
        }
        bool early = ret && !retA;
        if ((odd || early) && g_sf_logged < 15) {
            // what differed, with everything needed to replay it by hand
            int no = ++g_sf_logged;
            uint8_t *vp = *(uint8_t **)((uint8_t *)self + IV_VALUE), *lp = (uint8_t *)local_player();
            char where[48];
            if (lp && vp >= lp && vp < lp + 0x4000) snprintf(where, sizeof where, "local player+%#lx", (long)(vp - lp));
            else snprintf(where, sizeof where, "%p", (void *)vp);
            say("selftest #%d: %s at %s, kind %d k %lld a %.6f, t %.6f -> %.6f (B %.6f), amount %.6f, count %d (first %d max %d), "
                "tick/rec/plain/zero %d/%d/%d/%d, diff %.4f, ret game %d ours %d",
                no, ty.name, where, g.kind, g.k, g.a, t, t2, B, amount, count, first, maxe, ntick, nsh, nplain, nzero, d, retA, ret);
            say("  game %.4f %.4f %.4f  ours %.4f %.4f %.4f", A[0], A[1], A[2], val[0], val[1], val[2]);
            for (int i = 0; i < count && i < 8; i++) {
                const float *e = ptr[i];
                say("  [%d] time %.6f (bits %08x) exact %.6f -> %.6f  value %.4f %.4f %.4f", i, saved[i], fbits(saved[i]),
                    nv[i] == saved[i] ? (double)saved[i] : (double)nv[i] + B, nv[i], e[1], e[2], e[3]);
            }
        }
    }
    // this frame's render-time sample (the gun sway latches curtime right before interpolating)
    if (t_main && g.kind == 1 && iv_u16(self, IV_COUNT) > 0) {
        float head = *(float *)(el + (size_t)first * ty.stride);
        double dummy;
        if (fbits(head) == fbits(t) && !tick_time(head, g, &dummy)) shadow_record(self, *(void **)((uint8_t *)self + IV_VALUE), head, g.T);
    }
    return ret;
}

static double g_pred_T;    // this frame's exact predicted render time (meter), main thread
static long long g_pred_k;  // and its tick (k = final predicted tick - 1), for checking the gun fix's field
static int g_pred_frame = -1;

static int interp_common(int ti, void *self, float t, float amount)
{
    TypeRT &ty = g_type[ti];
    st(S_CALLS);
    Gate g;
    if (!ty.ok || !gate(t, &g)) { st(S_PASS); return pass(ty, self, t, amount); }
    if (g.kind == 2 && t_main) { g_pred_T = g.T; g_pred_k = g.k; g_pred_frame = g_clock->framecount; }
    if (!g_on.load(std::memory_order_relaxed)) { st(S_OFF); return pass(ty, self, t, amount); }
    st(g.kind == 1 ? S_GATE_CUR : S_GATE_PRED);
    return sandwich(ti, self, t, amount, g);
}

template <int I> static int h_interp(void *self, float t, float amount) { return interp_common(I, self, t, amount); }
static void *const g_hooks[8] = {(void *)h_interp<0>, (void *)h_interp<1>, (void *)h_interp<2>, (void *)h_interp<3>,
                                 (void *)h_interp<4>, (void *)h_interp<5>, (void *)h_interp<6>, (void *)h_interp<7>};

// ---------------------------------------------------------------- NoteChanged: the same-time flush, in exact time
static double exact_or_float(float ct, const Gate *g)
{
    double ex;
    if (g && tick_time(ct, *g, &ex)) return ex;
    return (double)ct;
}

static bool h_notechanged(void *self, float changetime, bool upd)
{
    uintptr_t vt = *(uintptr_t *)self;
    int i = 0;
    for (; i < g_nvt && g_vt[i].vt != vt; i++) {}
    if (i == g_nvt) return false;   // can't happen: only our vtables point here
    VtRT &v = g_vt[i];
    st(S_NC_CALLS);
    TypeRT &ty = g_type[v.type];
    if (ty.ok && g_on.load(std::memory_order_relaxed)) {
        const Clock *c = g_clock;
        Gate g = {0, 0, (double)c->interval_per_tick, 0, 0};
        const Gate *gp = (c->interval_per_tick > 0.0009f && c->interval_per_tick < 0.2f) ? &g : nullptr;
        double nw = exact_or_float(changetime, gp);
        uint8_t *el = iv_elems(self);
        int maxe = iv_u16(self, IV_MAX);
        while (el && iv_u16(self, IV_COUNT) > 0) {
            int first = iv_u16(self, IV_FIRST);
            if (first >= maxe) break;
            float head = *(float *)(el + (size_t)first * ty.stride);
            if (!(exact_or_float(head, gp) + 0.0001 > nw)) break;
            if (!(head + 0.0001f > changetime)) st(S_FLUSH_EXTRA);   // the game itself would have kept it
            // CSimpleRingBuffer::RemoveAtHead
            iv_u16(self, IV_FIRST) = (uint16_t)(first + 1 >= maxe ? first + 1 - maxe : first + 1);
            iv_u16(self, IV_COUNT) = (uint16_t)(iv_u16(self, IV_COUNT) - 1);
        }
    }
    return v.orig_nc(self, changetime, upd);
}

// ---------------------------------------------------------------- netvars (RecvTable offsets, same on both ABIs)
struct RecvTable_;
struct RecvProp_ {
    const char *name; int type, flags, strsize; bool inarray; const void *extra; void *arrayprop, *alproxy, *proxy, *dtproxy;
    RecvTable_ *table; int offset, stride, nelems; const char *parent;
};
struct RecvTable_ { RecvProp_ *props; int nprops; void *decoder; const char *name; };
struct ClientClass_ { void *create, *create_ev; const char *name; RecvTable_ *table; ClientClass_ *next; int id; };
static_assert(sizeof(RecvProp_) == 96, "RecvProp layout");
enum { DPT_Array = 5, DPT_DataTable = 6 };

static int netvar_in(const RecvTable_ *t, const char *var, int depth)
{
    if (!t || depth > 10) return -1;
    for (int i = 0; i < t->nprops; i++) {
        const RecvProp_ &p = t->props[i];
        if (p.name && getenv("CURTIMEFIX_NETVAR_DEBUG") && strstr(p.name, var))
            say("netvar %s in %s: '%s' type %d offset %#x table %p arrayprop %p", var, t->name ? t->name : "?", p.name, p.type,
                p.offset, (void *)p.table, p.arrayprop);
        if (p.name && !strcmp(p.name, var)) {
            // RecvPropArray3: a table at offset 0 whose elements "000", "001" ... carry the real offsets
            if (p.type == DPT_DataTable && p.table && p.table->nprops > 0) return p.offset + p.table->props[0].offset;
            // RecvPropArray: a DPT_Array prop at offset 0; its element template (m_pArrayProp) has the offset
            if (p.type == DPT_Array && p.arrayprop) return ((const RecvProp_ *)p.arrayprop)->offset;
            return p.offset;
        }
        if (p.type == DPT_DataTable && p.table) { int r = netvar_in(p.table, var, depth + 1); if (r >= 0) return r + p.offset; }
    }
    return -1;
}

static int netvar(const char *netclass, const char *var)   // e.g. ("CCSPlayer", "m_nTickBase")
{
    typedef ClientClass_ *(*GetAllClasses_t)(void *);
    ClientClass_ *cc = (*(GetAllClasses_t **)g_client)[8](g_client);
    for (int n = 0; cc && n < 4096; cc = cc->next, n++)
        if (cc->name && !strcmp(cc->name, netclass)) return netvar_in(cc->table, var, 0);
    return -1;
}

// ---------------------------------------------------------------- D: the gun's animation cycle
// C_BaseViewModel::Interpolate sets the cycle from elapsed = fl(N*I) - m_flAnimTime + a*I (N = the local player's
// final predicted tick): two big floats subtracted, off by up to a float step per tick. While it runs, both are
// moved down to small exact values (the animation's start lands 128 ticks in, like a fresh map), and the predicted
// times its own interpolation produces are recognised with that shift; everything is put back afterwards.
typedef bool (*EntInterp_t)(void *self, float t);
static EntInterp_t o_vm_interp;
static int g_off_animtime = -1, g_off_tickbase = -1, g_off_cycle = -1;
static int g_vm_state;            // 0 = checking the predicted-tick field, 1 = on, 2 = off
static int g_vm_match, g_vm_miss;
static int g_vm_seen_N, g_vm_seen_frame = -1;   // field value the gun saw, checked at the end of that frame
typedef int (*GetLocalPlayer_t)(void *);
typedef void *(*GetClientEntity_t)(void *, int);

static void *local_player()
{
    int i = (*(GetLocalPlayer_t **)g_engine)[12](g_engine);
    return (*(GetClientEntity_t **)g_entlist)[3](g_entlist, i);
}

static bool h_vm_interp(void *self, float t)
{
    st(S_VM_CALLS);
    if (!g_on.load(std::memory_order_relaxed) || !t_main || g_vm_state == 2) return o_vm_interp(self, t);
    void *lp = local_player();
    if (!lp) return o_vm_interp(self, t);
    int *pN = (int *)((uint8_t *)lp + g_off_tickbase + 4);   // m_nFinalPredictedTick follows m_nTickBase
    int N = *pN;
    const Clock *c = g_clock;
    if (g_vm_state == 0) {   // not trusted yet: note what the field holds, vm_check_frame() compares it
        g_vm_seen_N = N;
        g_vm_seen_frame = c->framecount;
        return o_vm_interp(self, t);
    }
    float If = c->interval_per_tick;
    if (!(If > 0.0009f && If < 0.2f)) return o_vm_interp(self, t);
    float *pA = (float *)((uint8_t *)self + g_off_animtime);
    float anim = *pA;
    Gate g = {0, 0, (double)If, 0, 0};
    double ex;
    if (!tick_time(anim, g, &ex)) { st(S_VM_PASS); return o_vm_interp(self, t); }
    long long j = llround(ex / g.I), K = j - g_K1, N2 = (long long)N - K;
    if (N2 < 1 || N2 > 16777216) { st(S_VM_PASS); return o_vm_interp(self, t); }
    volatile float a2 = (float)g_K1 * If;
    *pN = (int)N2;
    *pA = a2;
    t_shift = K;
    t_win_bad = 0;
    bool r = o_vm_interp(self, t);
    t_shift = 0;
    *pN = N;
    float now = *pA;
    if (fbits(now) == fbits(a2)) *pA = anim;   // untouched: back to the real start time
    else {
        // UpdateAnimationParity restarted the animation at GetFinalPredictedTime(): the real one is fl(N*I)
        volatile float fin2 = (float)N2 * If;
        if (fbits(now) == fbits(fin2)) { volatile float fin = (float)N * If; *pA = fin; }
    }
    if (t_win_bad) {
        // something in there wasn't recognised: redo the frame the game's way and leave this part off
        g_vm_state = 2;
        say("gun animation part off: %d interpolations in its window weren't recognised", t_win_bad);
        return o_vm_interp(self, t);
    }
    st(S_VM);
    return r;
}

// end of a frame's interpolation: the field must equal the predicted tick (k + 1) the local player used this frame
static void vm_check_frame()
{
    if (g_vm_state != 0 || g_vm_seen_frame != g_clock->framecount || g_pred_frame != g_clock->framecount) return;
    if (g_vm_seen_N == g_pred_k + 1) g_vm_match++; else g_vm_miss++;
    g_vm_seen_frame = -1;
    if (g_vm_miss > 20) { g_vm_state = 2; say("gun animation part off: the predicted-tick field didn't check out (%d/%d)", g_vm_match, g_vm_miss); }
    else if (g_vm_match >= 30 && g_vm_miss == 0) { g_vm_state = 1; say("gun animation part on (field checked %d times)", g_vm_match); }
    else if (g_vm_match >= 30) { g_vm_state = 2; say("gun animation part off: the predicted-tick field matched only %d of %d times", g_vm_match, g_vm_match + g_vm_miss); }
}

// ---------------------------------------------------------------- meter: how evenly the camera and gun move
typedef bool (*IsInGame_t)(void *);
typedef const float *(*GetAbs_t)(void *);
template <typename F> static F vfn(void *obj, int slot) { return (F)(*(void ***)obj)[slot]; }
#ifdef _WIN32
enum { SLOT_GetAbsOrigin = 9 };
#else
enum { SLOT_GetAbsOrigin = 11 };
#endif
enum { SLOT_GetLocalPlayer = 12, SLOT_IsInGame = 26, SLOT_GetClientEntity = 3, SLOT_FrameStageNotify = 35 };

struct Meter {
    bool on;
    bool have;
    double Tprev, pos[3], relmag, vprev, moving_since, last_moving;
    void *vm;
    double vm_check;
    // camera: speed per frame over the exact predicted render time the camera follows; gun: frame-to-frame change
    // of the gun's distance from the player
    double n, s, ss, frozen, jit;
    double gn, gs, gss;
    double cyc_prev, cn, cs, css, cjit, crprev, cfrozen;   // gun animation: cycle advance per second of game time
    int canim;
};
static Meter g_m;

static int g_off_vmhandle = -1;   // m_hViewModel[0] in the player

static void *find_viewmodel(void *local)
{
    if (g_off_vmhandle < 0) return nullptr;
    uint32_t h = *(const uint32_t *)((const uint8_t *)local + g_off_vmhandle);
    if (h == 0xFFFFFFFFu) return nullptr;
    void *e = vfn<GetClientEntity_t>(g_entlist, SLOT_GetClientEntity)(g_entlist, (int)(h & 0x1FFF));   // entity index bits
    if (!e) return nullptr;
    for (int k = 0; k < g_found.nvm; k++) if (*(uintptr_t *)e == g_found.vm_vt[k]) return e;
    return nullptr;
}

static void meter_frame()
{
    Meter &m = g_m;
    if (!m.on || !vfn<IsInGame_t>(g_engine, SLOT_IsInGame)(g_engine)) { m.have = false; return; }
    void *lp = vfn<GetClientEntity_t>(g_entlist, SLOT_GetClientEntity)(g_entlist, vfn<GetLocalPlayer_t>(g_engine, SLOT_GetLocalPlayer)(g_engine));
    if (!lp) { m.have = false; return; }
    double T = g_pred_T;
    if (T <= 0) { m.have = false; return; }
    const float *o = vfn<GetAbs_t>(lp, SLOT_GetAbsOrigin)(lp);
    double now = plat_now();
    if (!m.vm || now > m.vm_check) { m.vm = find_viewmodel(lp); m.vm_check = now + 1.0; }
    // the gun was placed while rendering the previous frame: pair it with the player's previous origin
    double relmag = -1;
    if (m.have && m.vm) {
        bool ok = false;
        for (int k = 0; k < g_found.nvm; k++) if (*(uintptr_t *)m.vm == g_found.vm_vt[k]) ok = true;
        if (ok) {
            const float *v = vfn<GetAbs_t>(m.vm, SLOT_GetAbsOrigin)(m.vm);
            double r[3];
            for (int k = 0; k < 3; k++) r[k] = (double)v[k] - m.pos[k];
            relmag = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        } else m.vm = nullptr;
    }
    if (m.have && T > m.Tprev) {
        double dT = T - m.Tprev;
        double d = sqrt(pow(o[0] - m.pos[0], 2) + pow(o[1] - m.pos[1], 2) + pow(o[2] - m.pos[2], 2));
        double v = d / dT;
        // a flight = movement with no 0.1 s pause; its first 0.5 s (speeding up) is skipped. Frozen frames inside a
        // flight are counted: they are what the meter is for.
        bool moving = v > 50 && v < 20000;
        if (moving) { if (!m.moving_since) m.moving_since = T; m.last_moving = T; }
        else if (T - m.last_moving > 0.1) m.moving_since = 0;
        if (m.moving_since && T - m.moving_since > 0.5 && v < 20000) {
            m.n++; m.s += v; m.ss += v * v;
            double mean = m.s / m.n;
            if (m.n > 20 && v < 0.1 * mean) m.frozen++;
            if (m.vprev > 0) m.jit += fabs(v - m.vprev);
            if (relmag >= 0 && m.relmag >= 0) {
                double gv = fabs(relmag - m.relmag) / dT;
                m.gn++; m.gs += gv; m.gss += gv * gv;
            }
        }
        m.vprev = moving ? v : 0;
    }
    // gun animation: how evenly the cycle advances (looping animations; a wrap or a new sequence is skipped)
    if (m.vm && g_off_cycle >= 0) {
        double cyc = *(const float *)((const uint8_t *)m.vm + g_off_cycle);
        if (m.have && T > m.Tprev) {
            double dc = cyc - m.cyc_prev, rate = dc / (T - m.Tprev);
            // inside an animation (it advanced recently): count every frame, a frame that didn't advance as frozen
            if (dc > 0 && dc < 0.2 && rate < 50) m.canim = 8;
            else if (dc != 0) m.canim = 0;   // wrapped / new sequence
            if (m.canim > 0 && dc >= 0 && dc < 0.2 && rate < 50) {
                if (dc == 0) { m.canim--; m.cfrozen++; }
                m.cn++; m.cs += rate; m.css += rate * rate;
                if (m.crprev >= 0) m.cjit += fabs(rate - m.crprev);
                m.crprev = rate;
            } else m.crprev = -1;
        }
        m.cyc_prev = cyc;
    }
    m.have = true;
    m.Tprev = T;
    for (int k = 0; k < 3; k++) m.pos[k] = o[k];
    m.relmag = relmag;
}

static void meter_print()
{
    Meter &m = g_m;
    if (m.n < 30) say("meter: camera: not enough moving frames (%.0f); fly or run in a straight line", m.n);
    else {
        double mean = m.s / m.n, sd = sqrt(fmax(0, m.ss / m.n - mean * mean));
        say("meter: camera %.0f frames, speed %.1f u/s, spread %.2f%%, frame-to-frame %.2f%%, frozen %.2f%%", m.n, mean,
            100 * sd / mean, 100 * m.jit / (m.n * mean), 100 * m.frozen / m.n);
    }
    if (m.gn > 30) {
        double gm = m.gs / m.gn, gsd = sqrt(fmax(0, m.gss / m.gn - gm * gm));
        say("meter: gun offset change %.3f u/s mean, %.3f sd (smaller = steadier gun; compare 0 vs 1 at the same map age)", gm, gsd);
    } else if (m.n >= 30) say("meter: gun not seen (r_drawviewmodel 0, or no weapon)");
    if (m.cn > 30) {
        double cm = m.cs / m.cn, csd = sqrt(fmax(0, m.css / m.cn - cm * cm));
        (void)csd;
        say("meter: gun animation %.0f frames, %.4f cycles/s, frame-to-frame %.3f%%, frozen %.2f%%", m.cn, cm,
            100 * m.cjit / (m.cn * cm), 100 * m.cfrozen / m.cn);
    }
}

// ---------------------------------------------------------------- install (main thread, between frames)
static uint8_t *g_stubs;

static bool install()
{
    // stubs within rel32 reach of every call site: jmp [rip+0]; dq hook
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (int i = 0; i < g_found.nsites; i++) { if (g_found.sites[i] < lo) lo = g_found.sites[i]; if (g_found.sites[i] > hi) hi = g_found.sites[i]; }
    g_stubs = (uint8_t *)plat_alloc_near(lo, hi, 4096);
    if (!g_stubs) { say("no memory near client code; staying off"); return false; }
    for (int t = 0; t < g_ntypes; t++) {
        uint8_t *s = g_stubs + 16 * t;
        s[0] = 0xFF; s[1] = 0x25; memset(s + 2, 0, 4);
        void *h = g_hooks[t];
        memcpy(s + 6, &h, 8);
    }
    if (!plat_make_rx(g_stubs, 4096)) { say("can't make the stub page executable; staying off"); return false; }
    int patched = 0;
    for (int i = 0; i < g_found.nsites; i++) {
        int t = g_found.site_type[i];
        if (!g_type[t].ok) continue;
        uintptr_t site = g_found.sites[i];
        long long rel = (long long)((uintptr_t)(g_stubs + 16 * t)) - (long long)(site + 5);
        if (rel != (int32_t)rel) { say("call site %d out of reach", i); continue; }
        int32_t r32 = (int32_t)rel;
        if (plat_write_code((void *)(site + 1), &r32, 4)) patched++;
    }
    int slots = 0;
    for (int i = 0; i < g_nvt; i++) {
        if (!g_type[g_vt[i].type].ok) continue;
        void **slot = (void **)(g_vt[i].vt + 8 * vt_slot(VT_NOTECHANGED));
        g_vt[i].orig_nc = (NoteChanged_t)*slot;
        void *was = plat_swap_ptr(slot, (void *)h_notechanged);
        if (was) { g_vt[i].orig_nc = (NoteChanged_t)was; slots++; }
        else g_vt[i].vt = 0;
    }
    say("v%s on: %d of %d call sites redirected, NoteChanged hooked in %d vtables", CTF_VERSION, patched, g_found.nsites, slots);
    int vms = 0;
    if (g_found.vm_slot >= 0 && g_off_animtime >= 0 && g_off_tickbase >= 0) {
        o_vm_interp = (EntInterp_t)g_found.vm_fn;
        for (int i = 0; i < g_found.nvm; i++) {
            void **slot = (void **)(g_found.vm_vt[i] + 8 * g_found.vm_slot);
            if ((uintptr_t)*slot == g_found.vm_fn && plat_swap_ptr(slot, (void *)h_vm_interp)) vms++;
        }
    }
    if (vms) say("gun animation: slot %d hooked in %d vtables (checking its predicted-tick field first)", g_found.vm_slot, vms);
    else say("gun animation part not found (slot %d, m_flAnimTime %d, m_nTickBase %d); the rest works", g_found.vm_slot, g_off_animtime, g_off_tickbase);
    return patched > 0;
}

typedef float (*GetLastTimeStamp_t)(void *);
static void trace_frame()
{
    void *lp = vfn<IsInGame_t>(g_engine, SLOT_IsInGame)(g_engine) ? local_player() : nullptr;
    if (!lp) { g_trace_value = nullptr; return; }
    const Clock *c = g_clock;
    const float *o = vfn<GetAbs_t>(lp, SLOT_GetAbsOrigin)(lp);
    int N = g_off_tickbase >= 0 ? *(const int *)((const uint8_t *)lp + g_off_tickbase + 4) : -1;
    float ts = vfn<GetLastTimeStamp_t>(g_engine, 15)(g_engine);
    fprintf(stderr, "[ctftrace] %.6f tick %d a %.6f N %d k %lld kind %d calls %d hist %d tick %d rec %d plain %d worst %.2f "
            "pos %.3f %.3f %.3f ts %.4f t %.6f ct %.6f %.6f %.6f %.6f\n",
            plat_now(), c->tickcount, c->interpolation_amount, N, g_tf.k, g_tf.kind, g_tf.calls, g_tf.count, g_tf.ntick,
            g_tf.nsh, g_tf.nplain, g_tf.worst_ulps, o[0], o[1], o[2], ts, g_tf.t, g_tf.ct[0], g_tf.ct[1], g_tf.ct[2], g_tf.ct[3]);
    memset(&g_tf, 0, sizeof g_tf);
    if ((uint8_t *)lp != g_trace_lp) { g_trace_lp = (uint8_t *)lp; g_trace_value = nullptr; g_trace_ncand = 0; }
    if (!g_trace_value)   // the origin history: the one whose value is where the camera is
        for (int i = 0; i < g_trace_ncand; i++) {
            const float *v = (const float *)g_trace_cand[i];
            if (v[0] == o[0] && v[1] == o[1] && v[2] == o[2]) {
                g_trace_value = g_trace_cand[i];
                fprintf(stderr, "[ctftrace] origin history value at player+%#lx\n", (long)((uint8_t *)g_trace_value - g_trace_lp));
            }
        }
}

// ---------------------------------------------------------------- FrameStageNotify
typedef void (*FSN_t)(void *, int);
static FSN_t o_FSN;
enum { FRAME_RENDER_START = 5 };

static void h_FSN(void *self, int stage)
{
    t_main = true;
    if (!g_installed) {
        g_installed = true;
        plat_echo_console();
        if (!install()) g_on = false;
        if (!cmd_register()) say("console command not registered (the fix still works)");   // main thread: the cvar list isn't thread-safe
    }
    o_FSN(self, stage);
    if (stage == FRAME_RENDER_START && g_trace) trace_frame();
    if (stage == FRAME_RENDER_START) {
        vm_check_frame();
        if (g_m.on) meter_frame();
    }
}

// ---------------------------------------------------------------- commands
static void print_status()
{
    const Clock *c = g_clock;
    say("%s. map time %.1f min, float step there %.3f ms, tick %.4f s", g_on ? "ON" : "OFF (passing through)",
        c->curtime / 60.0, ulp(c->curtime) * 1000.0, c->interval_per_tick);
    uint64_t calls = g_st[S_CALLS];
    say("interpolations %llu: render time %llu, predicted %llu, switched off %llu, left alone %llu (clock mismatch %llu, order %llu, other %llu)",
        (unsigned long long)calls, (unsigned long long)g_st[S_GATE_CUR], (unsigned long long)g_st[S_GATE_PRED], (unsigned long long)g_st[S_OFF],
        (unsigned long long)g_st[S_PASS], (unsigned long long)g_st[S_PASS_BADCLOCK], (unsigned long long)g_st[S_PASS_ORDER],
        (unsigned long long)(g_st[S_PASS_BIG] + g_st[S_PASS_ZERO]));
    say("sample times: %llu tick-exact, %llu from frame records, %llu as stored, %llu empty; %llu frame records",
        (unsigned long long)g_st[S_E_TICK], (unsigned long long)g_st[S_E_SHADOW], (unsigned long long)g_st[S_E_PLAIN],
        (unsigned long long)g_st[S_E_ZERO], (unsigned long long)g_st[S_SHADOW_REC]);
    say("NoteChanged %llu, same-time samples dropped that the game would have kept: %llu; \"no more changes\" held back %llu",
        (unsigned long long)g_st[S_NC_CALLS], (unsigned long long)g_st[S_FLUSH_EXTRA], (unsigned long long)g_st[S_RET_KEPT]);
    say("gun animation: %s (field check %d/%d), %llu calls, %llu fixed, %llu left alone; predicted frame %d, now %d",
        g_vm_state == 1 ? "on" : g_vm_state == 2 ? "off" : "checking", g_vm_match, g_vm_miss,
        (unsigned long long)g_st[S_VM_CALLS], (unsigned long long)g_st[S_VM], (unsigned long long)g_st[S_VM_PASS],
        g_pred_frame, g_clock->framecount);
    if (g_sf_n) {
        say("selftest: %llu comparisons, difference max %.6f mean %.6f units, %llu over 0.01, %llu over 0.1",
            (unsigned long long)g_sf_n, g_sf_max, g_sf_sum / g_sf_n, (unsigned long long)g_sf_over1, (unsigned long long)g_sf_over2);
        say("selftest: over 0.01 = %llu the game's float times, %llu across a teleport in the samples, %llu UNEXPLAINED",
            (unsigned long long)g_sf_float, (unsigned long long)g_sf_jump, (unsigned long long)g_sf_odd);
        say("selftest: \"no more changes\" differs %llu times: %llu ours only (EARLY, would be a bug), %llu the game's only (harmless)",
            (unsigned long long)g_sf_retdiff, (unsigned long long)g_sf_ret_early, (unsigned long long)g_sf_ret_late);
    }
}

void ctf_command(int argc, const char **argv)
{
    if (!g_clock) { say("not active (see the messages at game start)"); return; }
    const char *a = argc > 1 ? argv[1] : "";
    if (!strcmp(a, "0") || !strcmp(a, "1")) {
        g_on = a[0] == '1';
        say("%s", g_on ? "ON" : "OFF: the game's own interpolation (for A/B)");
    } else if (!strcmp(a, "meter")) {
        if (g_m.on) { meter_print(); memset(&g_m, 0, sizeof g_m); say("meter off"); }
        else { memset(&g_m, 0, sizeof g_m); g_m.on = true; say("meter on: move steadily (noclip or run straight; turn with +right for the gun), then `curtimefix meter` again"); }
    } else if (!strcmp(a, "selftest")) {
        bool on = !g_selftest;
        if (on) { g_sf_max = g_sf_sum = 0; g_sf_n = g_sf_retdiff = g_sf_over1 = g_sf_over2 = g_sf_odd = g_sf_float = g_sf_jump = 0;
                 g_sf_ret_early = g_sf_ret_late = 0; g_sf_logged = 0; }
        else print_status();
        g_selftest = on;
        say("selftest %s", on ? "on: each frame also runs the game's own path and compares" : "off");
    } else if (!strcmp(a, "reset")) {
        for (auto &s : g_st) s = 0;
        g_sf_max = g_sf_sum = 0; g_sf_n = g_sf_retdiff = g_sf_over1 = g_sf_over2 = g_sf_odd = g_sf_float = g_sf_jump = 0;
        g_sf_ret_early = g_sf_ret_late = 0; g_sf_logged = 0;
        say("counters reset");
    } else if (!*a || !strcmp(a, "status")) {
        print_status();
    } else say("usage: curtimefix [0|1|status|meter|selftest|reset]");
}

// ---------------------------------------------------------------- start
static bool setup()
{
    g_client = plat_interface("client", "VClient017");
    g_engine = plat_interface("engine", "VEngineClient014");
    g_entlist = plat_interface("client", "VClientEntityList003");
    void *tool = plat_interface("engine", "VENGINETOOL003");
    if (!g_client || !g_engine || !g_entlist || !tool) return false;
    g_ConMsg = (ConMsg_t)plat_conmsg();
    ModInfo c, e;
    if (!plat_module("client", &c) || !plat_module("engine", &e)) return false;
    if (!find_all(c, e, tool, &g_found)) {
        say("game layout not recognised (types %d, clock %s); staying off. A game update may have moved things.",
            g_found.ntypes, g_found.clock ? "found" : "not found");
        return true;   // don't retry
    }
    g_clock = (const Clock *)g_found.clock;
    g_off_tickbase = netvar("CCSPlayer", "m_nTickBase");
    // C_BaseEntity / C_BaseAnimating members, at the same offset in every entity; the viewmodel doesn't network them
    g_off_animtime = netvar("CCSPlayer", "m_flAnimTime");
    g_off_cycle = netvar("CCSPlayer", "m_flCycle");
    g_off_vmhandle = netvar("CCSPlayer", "m_hViewModel[0]");
    if (const char *tr = getenv("CURTIMEFIX_TRACE")) g_trace = !strcmp(tr, "1");
    say("netvars: m_nTickBase %#x, m_flAnimTime %#x, m_flCycle %#x, m_hViewModel %#x", g_off_tickbase, g_off_animtime, g_off_cycle, g_off_vmhandle);
    g_ntypes = g_found.ntypes;
    for (int t = 0; t < g_ntypes; t++) {
        VarType &v = g_found.types[t];
        g_type[t].stride = v.stride;
        g_type[t].array = v.array;
        g_type[t].orig = (Interp_t)v.impl;
        g_type[t].angles = strstr(v.name, "QAngle") != nullptr;
        snprintf(g_type[t].name, sizeof g_type[t].name, "%s", v.name);
        int sites = 0;
        for (int i = 0; i < g_found.nsites; i++) if (g_found.site_type[i] == t) sites++;
        g_type[t].ok = v.stride > 0 && sites > 0;
        say("type %d: %s, %d-byte samples%s, %d call sites%s", t, v.name, v.stride, v.array ? " (array)" : "", sites, g_type[t].ok ? "" : " — NOT fixed");
    }
    g_nvt = 0;
    for (int i = 0; i < g_found.nvts; i++) g_vt[g_nvt++] = {g_found.vts[i].vt, g_found.vts[i].type, nullptr};
    if (const char *off = getenv("CURTIMEFIX_START_OFF")) if (!strcmp(off, "1")) g_on = false;
    // FrameStageNotify: installs the rest on the main thread at the next frame
    void **slot = &(*(void ***)g_client)[SLOT_FrameStageNotify];
    o_FSN = (FSN_t)*slot;
    void *was = plat_swap_ptr(slot, (void *)h_FSN);
    if (!was) { say("FrameStageNotify hook failed; staying off"); g_clock = nullptr; return true; }
    o_FSN = (FSN_t)was;
    return true;
}

static void *init_thread(void *)
{
    for (int i = 0; i < 3000; i++) {   // up to 300 s while the game loads
        if (setup()) return nullptr;
        plat_sleep_ms(100);
    }
    g_ConMsg = (ConMsg_t)plat_conmsg();
    say("game interfaces not found; staying off");
    return nullptr;
}

static bool ctf_start()
{
    if (!plat_in_game() || plat_disabled() || !plat_claim()) return false;
    return plat_thread(init_thread);
}

#ifndef _WIN32
__attribute__((constructor)) static void ctf_ctor() { ctf_start(); }
#else
extern "C" int __stdcall DllMain(void *, unsigned long reason, void *)
{
    if (reason == 1) ctf_start();
    return 1;
}
#endif
