// curtimefix: Linux side of ctf.h (preloaded into cstrike_linux64). Interface lookup and slot swapping follow
// CS:TV's client (same author).
#ifndef _WIN32
#include "ctf.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

double plat_now()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct ModQuery { const char *suffix; ModInfo *out; bool found; };

static int phdr_cb(dl_phdr_info *info, size_t, void *arg)
{
    ModQuery *q = (ModQuery *)arg;
    const char *n = info->dlpi_name;
    size_t nl = n ? strlen(n) : 0, sl = strlen(q->suffix);
    if (q->found || nl < sl || strcmp(n + nl - sl, q->suffix)) return 0;
    ModInfo &m = *q->out;
    m = ModInfo();
    m.base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) &p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD || !(p.p_flags & PF_R)) continue;
        Range r = {info->dlpi_addr + p.p_vaddr, info->dlpi_addr + p.p_vaddr + p.p_memsz};
        if (r.hi > m.end) m.end = r.hi;
        if (p.p_flags & PF_X) { if (m.ntext < 8) m.text[m.ntext++] = r; }
        else if (m.ndata < 16) m.data[m.ndata++] = r;
    }
    q->found = m.ntext > 0;
    return q->found;
}

bool plat_module(const char *name, ModInfo *out)
{
    std::string suffix = std::string("/") + name + ".so";
    ModQuery q = {suffix.c_str(), out, false};
    dl_iterate_phdr(phdr_cb, &q);
    return q.found;
}

// full path of a loaded module whose path ends with `suffix`, from /proc/self/maps
static std::string module_path(const char *suffix)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return "";
    char line[1024];
    std::string found;
    size_t sl = strlen(suffix);
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, '/');
        if (!p) continue;
        p[strcspn(p, "\n")] = 0;
        size_t n = strlen(p);
        if (n >= sl && !strcmp(p + n - sl, suffix)) { found = p; break; }
    }
    fclose(f);
    return found;
}

typedef void *(*CreateInterfaceFn_t)(const char *name, int *ret);

void *plat_interface(const char *module, const char *iface)
{
    std::string path = module_path((std::string("/") + module + ".so").c_str());
    if (path.empty()) return nullptr;
    void *h = dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY);   // the game loads these privately
    if (!h) return nullptr;
    auto ci = (CreateInterfaceFn_t)dlsym(h, "CreateInterface");
    void *r = ci ? ci(iface, nullptr) : nullptr;
    dlclose(h);
    return r;
}

void *plat_conmsg()
{
    std::string tier0 = module_path("/libtier0.so");
    void *h = tier0.empty() ? nullptr : dlopen(tier0.c_str(), RTLD_NOLOAD | RTLD_LAZY);
    if (!h) return nullptr;
    void *f = dlsym(h, "_Z6ConMsgPKcz");
    dlclose(h);
    return f;
}

// the protection a page has now, so it can be put back exactly
static int page_prot(uintptr_t addr)
{
    int prot = PROT_READ;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return prot;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        uintptr_t s, e;
        char perms[5] = {0};
        if (sscanf(line, "%lx-%lx %4s", &s, &e, perms) < 3 || addr < s || addr >= e) continue;
        prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
        break;
    }
    fclose(f);
    return prot;
}

static bool with_writable(uintptr_t a, size_t n, void (*fn)(void *), void *arg)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t start = a & ~(uintptr_t)(pg - 1), end = (a + n + pg - 1) & ~(uintptr_t)(pg - 1);
    int p1 = page_prot(start), p2 = page_prot(end - pg);
    if (mprotect((void *)start, end - start, p1 | p2 | PROT_READ | PROT_WRITE)) return false;
    fn(arg);
    mprotect((void *)start, pg, p1);
    if (end - start > (uintptr_t)pg) mprotect((void *)(start + pg), end - start - pg, p2);
    return true;
}

struct SwapArg { void **where; void *fn, *old; };
static void do_swap(void *p) { SwapArg *s = (SwapArg *)p; s->old = __atomic_exchange_n(s->where, s->fn, __ATOMIC_SEQ_CST); }

void *plat_swap_ptr(void **where, void *fn)
{
    SwapArg s = {where, fn, nullptr};
    return with_writable((uintptr_t)where, sizeof(void *), do_swap, &s) ? s.old : nullptr;
}

struct CodeArg { void *addr; const void *bytes; size_t n; };
static void do_code(void *p) { CodeArg *c = (CodeArg *)p; memcpy(c->addr, c->bytes, c->n); }

bool plat_write_code(void *addr, const void *bytes, size_t n)
{
    CodeArg c = {addr, bytes, n};
    return with_writable((uintptr_t)addr, n, do_code, &c);
}

void *plat_alloc_near(uintptr_t lo, uintptr_t hi, size_t size)
{
    const uintptr_t step = 64ull << 20, reach = 1536ull << 20;
    for (uintptr_t d = step; d < reach; d += step) {
        uintptr_t tries[2] = {hi + d, lo > d + size ? lo - d - size : 0};
        for (uintptr_t want : tries) {
            if (!want) continue;
            want &= ~(uintptr_t)0xFFFF;
            void *p = mmap((void *)want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
            uintptr_t a = (uintptr_t)p, b = a + size;
            uintptr_t dlo = b > lo ? b - lo : lo - a, dhi = hi > a ? hi - a : b - hi;   // farthest call site either way
            if (dlo < reach && dhi < reach) return p;
            munmap(p, size);
        }
    }
    return nullptr;
}

bool plat_make_rx(void *p, size_t size) { return mprotect(p, size, PROT_READ | PROT_EXEC) == 0; }

bool plat_in_game()
{
    char comm[64] = {0};
    if (FILE *f = fopen("/proc/self/comm", "r")) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
    return strstr(comm, "cstrike_linux64") != nullptr;
}

bool plat_disabled()
{
    const char *e = getenv("CURTIMEFIX_DISABLE");
    return e && !strcmp(e, "1");
}

bool plat_claim()
{
    if (getenv("CURTIMEFIX_LOADED")) return false;
    setenv("CURTIMEFIX_LOADED", "1", 1);
    return true;
}

bool plat_thread(void *(*fn)(void *))
{
    // detached from the start (pthread_detach is a glibc 2.34 symbol; the Steam runtime may be older)
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    bool ok = pthread_create(&th, &at, fn, nullptr) == 0;
    pthread_attr_destroy(&at);
    return ok;
}

void plat_sleep_ms(int ms) { usleep(ms * 1000); }

// CURTIMEFIX_ECHO_CONSOLE=1: copy the game console to stderr (headless test runs)
typedef int (*Spew_t)(int type, const char *msg);
static Spew_t g_old_spew;
static int echo_spew(int type, const char *msg)
{
    fputs(msg, stderr);
    return g_old_spew ? g_old_spew(type, msg) : 0;
}

void plat_echo_console()
{
    const char *e = getenv("CURTIMEFIX_ECHO_CONSOLE");
    if (!e || strcmp(e, "1")) return;
    std::string tier0 = module_path("/libtier0.so");
    void *hd = tier0.empty() ? nullptr : dlopen(tier0.c_str(), RTLD_NOLOAD | RTLD_LAZY);
    if (!hd) return;
    auto get = (Spew_t (*)())dlsym(hd, "GetSpewOutputFunc");
    auto set = (void (*)(Spew_t))dlsym(hd, "SpewOutputFunc");
    if (get && set && get() != echo_spew) { g_old_spew = get(); set(echo_spew); }
    dlclose(hd);
}
#endif
