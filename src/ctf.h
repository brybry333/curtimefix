// curtimefix: shared declarations. No Source SDK headers here (only cmd.cpp uses them).
#pragma once
#include <cstddef>
#include <cstdint>

#define CTF_VERSION "1.2"

struct Range { uintptr_t lo, hi; };

// A loaded game module: its code ranges and its readable data ranges (rodata, data, relro).
struct ModInfo {
    uintptr_t base = 0, end = 0;
    Range text[8]; int ntext = 0;
    Range data[16]; int ndata = 0;
    bool in_text(uintptr_t a) const { for (int i = 0; i < ntext; i++) if (a >= text[i].lo && a < text[i].hi) return true; return false; }
};

// ---- platform (plat_linux.cpp / plat_win.cpp)
bool plat_module(const char *name, ModInfo *out);          // "client" / "engine"
void *plat_interface(const char *module, const char *iface);
void *plat_conmsg();                                       // tier0's printf-style console print
void *plat_swap_ptr(void **where, void *fn);               // swap a pointer in read-only data; old value or null
bool plat_write_code(void *addr, const void *bytes, size_t n);
void *plat_alloc_near(uintptr_t lo, uintptr_t hi, size_t size);   // RW memory within +-1.5 GB of [lo, hi)
bool plat_make_rx(void *p, size_t size);
bool plat_in_game();
bool plat_disabled();                                      // CURTIMEFIX_DISABLE=1
bool plat_claim();                                         // first copy in this process
bool plat_thread(void *(*fn)(void *));
void plat_sleep_ms(int ms);
double plat_now();

// ---- logging (fix.cpp)
void say(const char *fmt, ...);

// ---- finder (find.cpp)
enum { VT_INTERPOLATE, VT_NOTECHANGED };
int vt_slot(int which);                                    // per-ABI vtable slot

struct VarType {
    char name[96];          // RTTI name of one class using it
    int stride;             // bytes per history entry
    bool array;
    uintptr_t impl;         // Interpolate(float t, float amount)
};
struct VarVtable { uintptr_t vt; int type; };

struct Found {
    VarType types[8]; int ntypes = 0;
    VarVtable vts[32]; int nvts = 0;
    uintptr_t sites[32]; int site_type[32]; int nsites = 0;    // rel32 call/jmp into an impl
    uintptr_t clock = 0;                                       // the client's CGlobalVarsBase
    uintptr_t vm_vt[4]; int nvm = 0;                           // C_PredictedViewModel / C_BaseViewModel vtables
    int vm_slot = -1; uintptr_t vm_fn = 0;                     // C_BaseViewModel::Interpolate (gun animation cycle)
};
bool find_all(const ModInfo &client, const ModInfo &engine, void *enginetool, Found *f);

// ---- console command (cmd.cpp → fix.cpp)
bool cmd_register();
void ctf_command(int argc, const char **argv);
