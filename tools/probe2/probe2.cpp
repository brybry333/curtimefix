// probe2: per-frame camera recorder + console-command channel for the 64-bit CS:S Linux client (LD_PRELOAD).
// For finding out where camera jitter comes from. Does nothing unless PROBE2_DIR is set.
//
//   PROBE2_DIR=<dir> (no spaces)
//     <dir>/ctl        commands, one per line, appended by the tester (read by offset, never modified)
//                        !log <n>   start recording segment n (records go to <dir>/rec.bin)
//                        !stop      stop recording (flushes)
//                        !mark ...  write a line to probe.log
//                        anything else: IVEngineClient::ClientCmd_Unrestricted
//     <dir>/rec.bin    records (struct Rec below, little endian), appended
//     <dir>/probe.log  status
//
// One record per rendered frame, written at FRAME_RENDER_END:
//   the camera this frame was drawn with (IBaseClientDLL::GetPlayerView: origin + angles, smoothing included),
//   the engine's view angles and the local player's interpolated origin at FRAME_RENDER_START,
//   the client clock (CGlobalVarsBase via IEngineTool's ClientTime accessor), m_nTickBase and the final predicted
//   tick, and monotonic wall time at both stages.
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

typedef void *(*CreateInterfaceFn_t)(const char *, int *);
typedef void (*FSN_t)(void *, int);
typedef int (*GetLocalPlayer_t)(void *);
typedef bool (*IsInGame_t)(void *);
typedef void (*ClientCmdU_t)(void *, const char *);
typedef void *(*GetClientEntity_t)(void *, int);
typedef const float *(*GetAbsOrigin_t)(void *);
typedef void (*GetViewAngles_t)(void *, float *);
typedef bool (*GetPlayerView_t)(void *, void *);

static void *g_client, *g_engine, *g_entlist, *g_tool;
static FSN_t o_FSN;
static FILE *g_log;
static char g_dir[400], g_ctlPath[440], g_recPath[440];
static int g_recFd = -1;
static long g_ctlOff;
static int g_segment;
static double g_lastCtl;
static const uint8_t *g_clock;   // CGlobalVarsBase
static int g_offTickBase = -1;
static bool g_viewOk, g_viewChecked;

template <typename F> static F vfn(void *obj, int slot) { return (F)(*(void ***)obj)[slot]; }

static double mono()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void say(const char *fmt, ...)
{
    if (!g_log) return;
    fprintf(g_log, "%.3f ", mono());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static int parse_int(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void *iface(const char *module, const char *name)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return nullptr;
    char line[1024], path[1024] = "";
    char want[64];
    snprintf(want, sizeof want, "/%s.so", module);
    size_t wl = strlen(want);
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, '/');
        if (!p) continue;
        p[strcspn(p, "\n")] = 0;
        size_t n = strlen(p);
        if (n >= wl && !strcmp(p + n - wl, want)) { snprintf(path, sizeof path, "%s", p); break; }
    }
    fclose(f);
    if (!path[0]) return nullptr;
    void *h = dlopen(path, RTLD_NOLOAD | RTLD_LAZY);
    if (!h) return nullptr;
    auto ci = (CreateInterfaceFn_t)dlsym(h, "CreateInterface");
    void *r = ci ? ci(name, nullptr) : nullptr;
    dlclose(h);
    return r;
}

static int page_prot(uintptr_t addr)
{
    int prot = PROT_READ;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return prot;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned long s = 0, e = 0;
        char *p = line;
        s = strtoul(p, &p, 16);
        if (*p == '-') p++;
        e = strtoul(p, &p, 16);
        while (*p == ' ') p++;
        if (addr < s || addr >= e) continue;
        prot = (p[0] == 'r' ? PROT_READ : 0) | (p[1] == 'w' ? PROT_WRITE : 0) | (p[2] == 'x' ? PROT_EXEC : 0);
        break;
    }
    fclose(f);
    return prot;
}

static void *swap_slot(void *obj, int slot, void *fn)
{
    void **vt = *(void ***)obj;
    uintptr_t a = (uintptr_t)&vt[slot];
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t start = a & ~(uintptr_t)(pg - 1);
    int prot = page_prot(a);
    if (mprotect((void *)start, pg, prot | PROT_WRITE)) return nullptr;
    void *orig = __atomic_exchange_n(&vt[slot], fn, __ATOMIC_SEQ_CST);
    mprotect((void *)start, pg, prot);
    return orig;
}

// IEngineTool slot 46 = ClientTime: "lea/mov rax,[rip+d]; movss xmm0,[rax+0x0c]; ret" (curtimefix's find.cpp)
static const uint8_t *find_clock()
{
    if (!g_tool) return nullptr;
    const uint8_t *b = (const uint8_t *)(*(void ***)g_tool)[46];
    if (b[0] == 0x48 && (b[1] == 0x8D || b[1] == 0x8B) && b[2] == 0x05) {
        int32_t d;
        memcpy(&d, b + 3, 4);
        uintptr_t base = (uintptr_t)b + 7 + d;
        if (b[1] == 0x8B) base = *(uintptr_t *)base;
        const uint8_t *c = b + 7;
        if (c[0] == 0xF3 && c[1] == 0x0F && c[2] == 0x10 && c[3] == 0x40 && c[4] == 0x0c && c[5] == 0xC3) return (const uint8_t *)base;
    }
    if (b[0] == 0xF3 && b[1] == 0x0F && b[2] == 0x10 && (b[3] & 0xC7) == 0x05 && b[8] == 0xC3) {
        int32_t d;
        memcpy(&d, b + 4, 4);
        return (const uint8_t *)((uintptr_t)b + 8 + d - 0x0c);
    }
    return nullptr;
}

// netvars (same RecvTable walk as curtimefix)
struct RecvTable_;
struct RecvProp_ {
    const char *name; int type, flags, strsize; bool inarray; const void *extra; void *arrayprop, *alproxy, *proxy, *dtproxy;
    RecvTable_ *table; int offset, stride, nelems; const char *parent;
};
struct RecvTable_ { RecvProp_ *props; int nprops; void *decoder; const char *name; };
struct ClientClass_ { void *create, *create_ev; const char *name; RecvTable_ *table; ClientClass_ *next; int id; };
static_assert(sizeof(RecvProp_) == 96, "RecvProp layout");

static int netvar_in(const RecvTable_ *t, const char *var, int depth)
{
    if (!t || depth > 10) return -1;
    for (int i = 0; i < t->nprops; i++) {
        const RecvProp_ &p = t->props[i];
        if (p.name && !strcmp(p.name, var)) return p.offset;
        if (p.type == 6 && p.table) { int r = netvar_in(p.table, var, depth + 1); if (r >= 0) return r + p.offset; }
    }
    return -1;
}

static int netvar(const char *netclass, const char *var)
{
    typedef ClientClass_ *(*GetAllClasses_t)(void *);
    ClientClass_ *cc = vfn<GetAllClasses_t>(g_client, 8)(g_client);
    for (int n = 0; cc && n < 4096; cc = cc->next, n++)
        if (cc->name && !strcmp(cc->name, netclass)) return netvar_in(cc->table, var, 0);
    return -1;
}

#pragma pack(push, 1)
struct Rec {
    double t5, t6;               // monotonic s at FRAME_RENDER_START (after the game's own work) / FRAME_RENDER_END
    float vo[3], va[3];          // camera this frame was drawn with
    float ea[3];                 // engine view angles at RENDER_START
    float ao[3];                 // local player's abs origin at RENDER_START (interpolated)
    float curtime, frametime, interp, absframetime;
    int32_t tickcount, framecount, tickbase, finalpred;
    int32_t segment, flags;      // flags: 1 = camera valid
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 104, "record size");

static char g_buf[1 << 20];
static size_t g_bufLen;
static Rec g_cur;
static bool g_have5;

static void flush_rec()
{
    size_t off = 0;
    while (g_recFd >= 0 && off < g_bufLen) {
        ssize_t w = write(g_recFd, g_buf + off, g_bufLen - off);
        if (w <= 0) { say("record write failed: %s", strerror(errno)); break; }
        off += (size_t)w;
    }
    g_bufLen = 0;
}

static void run_ctl()
{
    FILE *f = fopen(g_ctlPath, "r");
    if (!f) return;
    if (fseek(f, g_ctlOff, SEEK_SET)) { fclose(f); return; }
    char line[600];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (!n || line[n - 1] != '\n') break;   // partial line: wait for the rest
        g_ctlOff += (long)n;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        if (!strncmp(line, "!log ", 5)) {
            flush_rec();
            g_segment = parse_int(line + 5);
            say("segment %d", g_segment);
        } else if (!strcmp(line, "!stop")) {
            flush_rec();
            say("segment %d stopped", g_segment);
            g_segment = 0;
        } else if (!strncmp(line, "!mark", 5)) {
            say("mark%s", line + 5);
        } else {
            char cmd[620];
            snprintf(cmd, sizeof cmd, "%s\n", line);
            vfn<ClientCmdU_t>(g_engine, 106)(g_engine, cmd);
            say("cmd: %s", line);
        }
    }
    fclose(f);
}

static void *local_player()
{
    if (!vfn<IsInGame_t>(g_engine, 26)(g_engine)) return nullptr;
    int idx = vfn<GetLocalPlayer_t>(g_engine, 12)(g_engine);
    return vfn<GetClientEntity_t>(g_entlist, 3)(g_entlist, idx);
}

static void h_FSN(void *self, int stage)
{
    o_FSN(self, stage);
    if (stage == 5) {
        const double now = mono();
        if (now - g_lastCtl > 0.05) { g_lastCtl = now; run_ctl(); }
        g_have5 = false;
        if (!g_segment) return;
        void *lp = local_player();
        if (!lp || !g_clock) return;
        Rec &r = g_cur;
        memset(&r, 0, sizeof r);
        r.t5 = now;
        const float *o = vfn<GetAbsOrigin_t>(lp, 11)(lp);
        memcpy(r.ao, o, 12);
        vfn<GetViewAngles_t>(g_engine, 19)(g_engine, r.ea);
        memcpy(&r.curtime, g_clock + 0x0c, 4);
        memcpy(&r.frametime, g_clock + 0x10, 4);
        memcpy(&r.interp, g_clock + 0x20, 4);
        memcpy(&r.absframetime, g_clock + 0x08, 4);
        memcpy(&r.tickcount, g_clock + 0x18, 4);
        memcpy(&r.framecount, g_clock + 0x04, 4);
        if (g_offTickBase > 0) {
            memcpy(&r.tickbase, (const uint8_t *)lp + g_offTickBase, 4);
            memcpy(&r.finalpred, (const uint8_t *)lp + g_offTickBase + 4, 4);
        }
        r.segment = g_segment;
        g_have5 = true;
    } else if (stage == 6) {
        if (!g_have5 || !g_segment) return;
        g_have5 = false;
        Rec &r = g_cur;
        r.t6 = mono();
        if (g_viewOk || !g_viewChecked) {
            alignas(16) uint8_t vs[1024];
            memset(vs, 0, sizeof vs);
            vfn<GetPlayerView_t>(g_client, 59)(g_client, vs);
            float o[3], a[3];
            memcpy(o, vs + 64, 12);
            memcpy(a, vs + 76, 12);
            if (!g_viewChecked) {
                // CViewSetup: origin at 64, angles at 76 (SDK 2013 layout). Trust it only if it agrees with the
                // engine's view angles (yaw) and sits near the player.
                float dy = a[1] - r.ea[1];
                while (dy > 180) dy -= 360;
                while (dy < -180) dy += 360;
                float dz = o[2] - r.ao[2];
                float dx = o[0] - r.ao[0], dyy = o[1] - r.ao[1];
                g_viewOk = dy > -5 && dy < 5 && dz > -10 && dz < 90 && dx * dx + dyy * dyy < 900;
                g_viewChecked = true;
                say("GetPlayerView check: origin %.1f %.1f %.1f angles %.2f %.2f %.2f vs player %.1f %.1f %.1f eng %.2f %.2f: %s",
                    o[0], o[1], o[2], a[0], a[1], a[2], r.ao[0], r.ao[1], r.ao[2], r.ea[0], r.ea[1], g_viewOk ? "ok" : "NOT ok");
            }
            if (g_viewOk) { memcpy(r.vo, o, 12); memcpy(r.va, a, 12); r.flags |= 1; }
        }
        if (g_bufLen + sizeof r > sizeof g_buf) flush_rec();
        memcpy(g_buf + g_bufLen, &r, sizeof r);
        g_bufLen += sizeof r;
    }
}

static void *hook_thread(void *)
{
    for (int i = 0; i < 6000; i++) {
        g_client = iface("client", "VClient017");
        g_engine = iface("engine", "VEngineClient014");
        g_entlist = iface("client", "VClientEntityList003");
        g_tool = iface("engine", "VENGINETOOL003");
        if (g_client && g_engine && g_entlist && g_tool) {
            g_clock = find_clock();
            g_offTickBase = netvar("CCSPlayer", "m_nTickBase");
            say("clock %p, m_nTickBase %#x", (const void *)g_clock, g_offTickBase);
            void *was = swap_slot(g_client, 35, (void *)h_FSN);
            if (was) o_FSN = (FSN_t)was;
            say("hooked FrameStageNotify: %s", was ? "ok" : "FAILED");
            return nullptr;
        }
        usleep(100000);
    }
    say("interfaces never appeared");
    return nullptr;
}

extern char *program_invocation_short_name;

__attribute__((constructor)) static void probe_ctor()
{
    if (!program_invocation_short_name || strcmp(program_invocation_short_name, "cstrike_linux64")) return;
    const char *dir = getenv("PROBE2_DIR");
    if (!dir || !dir[0]) return;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    snprintf(g_ctlPath, sizeof g_ctlPath, "%s/ctl", g_dir);
    snprintf(g_recPath, sizeof g_recPath, "%s/rec.bin", g_dir);
    char lp[440];
    snprintf(lp, sizeof lp, "%s/probe.log", g_dir);
    g_log = fopen(lp, "a");
    g_recFd = open(g_recPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    FILE *c = fopen(g_ctlPath, "r");   // skip what is already there
    if (c) { fseek(c, 0, SEEK_END); g_ctlOff = ftell(c); fclose(c); }
    say("probe2 loaded in pid %d (rec fd %d)", (int)getpid(), g_recFd);
    pthread_t th;
    pthread_create(&th, nullptr, hook_thread, nullptr);
    (void)th;   // not detached: pthread_detach is GLIBC_2.34 when built here, the runtime has 2.31
}
