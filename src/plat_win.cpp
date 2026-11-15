// curtimefix: Windows side of ctf.h (curtimefix.dll, loaded into cstrike_win64.exe by curtimefix.exe).
// Kept apart from the rest: windows.h and the Source SDK headers don't mix. Follows CS:TV's Windows platform code.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "ctf.h"

double plat_now()
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

bool plat_module(const char *name, ModInfo *out)
{
    char dll[64];
    snprintf(dll, sizeof dll, "%s.dll", name);
    HMODULE h = GetModuleHandleA(dll);
    if (!h) return false;
    uintptr_t base = (uintptr_t)h;
    auto dos = (const IMAGE_DOS_HEADER *)base;
    auto nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    ModInfo &m = *out;
    m = ModInfo();
    m.base = base;
    m.end = base + nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD ch = sec[i].Characteristics;
        if (!(ch & IMAGE_SCN_MEM_READ)) continue;
        DWORD size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        Range r = {base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + size};
        if (ch & IMAGE_SCN_MEM_EXECUTE) { if (m.ntext < 8) m.text[m.ntext++] = r; }
        else if (m.ndata < 16) m.data[m.ndata++] = r;
    }
    return m.ntext > 0;
}

typedef void *(*CreateInterfaceFn_t)(const char *name, int *ret);

void *plat_interface(const char *module, const char *iface)
{
    char name[64];
    snprintf(name, sizeof name, "%s.dll", module);
    HMODULE h = GetModuleHandleA(name);
    if (!h) return nullptr;
    auto ci = (CreateInterfaceFn_t)(void *)GetProcAddress(h, "CreateInterface");
    return ci ? ci(iface, nullptr) : nullptr;
}

// tier0's Msg (extern "C" on Windows, unlike the C++ ConMsg): printf-style, goes to the console
void *plat_conmsg()
{
    HMODULE h = GetModuleHandleA("tier0.dll");
    return h ? (void *)GetProcAddress(h, "Msg") : nullptr;
}

void *plat_swap_ptr(void **where, void *fn)
{
    DWORD old;
    if (!VirtualProtect(where, sizeof(void *), PAGE_READWRITE, &old)) return nullptr;
    void *orig = InterlockedExchangePointer(where, fn);
    DWORD tmp;
    VirtualProtect(where, sizeof(void *), old, &tmp);
    return orig;
}

bool plat_write_code(void *addr, const void *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, bytes, n);
    DWORD tmp;
    VirtualProtect(addr, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    return true;
}

void *plat_alloc_near(uintptr_t lo, uintptr_t hi, size_t size)
{
    const uintptr_t step = 0x10000, reach = 1536ull << 20;
    // walk free regions outward from the code, on both sides
    for (int side = 0; side < 2; side++) {
        uintptr_t a = side ? (hi + step) & ~(step - 1) : ((lo > reach ? lo - reach : step) + step - 1) & ~(step - 1);
        uintptr_t end = side ? lo + reach : lo;
        while (a + size < end) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) break;
            uintptr_t rb = (uintptr_t)mbi.BaseAddress, re = rb + mbi.RegionSize;
            if (mbi.State == MEM_FREE) {
                uintptr_t cand = (rb + step - 1) & ~(step - 1);
                if (cand + size <= re) {
                    uintptr_t b = cand + size;
                    uintptr_t dlo = b > lo ? b - lo : lo - cand, dhi = hi > cand ? hi - cand : b - hi;
                    if (dlo < reach && dhi < reach) {
                        void *p = VirtualAlloc((void *)cand, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                        if (p) return p;
                    }
                }
            }
            a = (re + step - 1) & ~(step - 1);
        }
    }
    return nullptr;
}

bool plat_make_rx(void *p, size_t size)
{
    DWORD old;
    bool ok = VirtualProtect(p, size, PAGE_EXECUTE_READ, &old) != 0;
    FlushInstructionCache(GetCurrentProcess(), p, size);
    return ok;
}

bool plat_in_game()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    const wchar_t *base = wcsrchr(path, L'\\');
    return base && !_wcsicmp(base + 1, L"cstrike_win64.exe");
}

bool plat_disabled()
{
    char v[8];
    return GetEnvironmentVariableA("CURTIMEFIX_DISABLE", v, sizeof v) && !strcmp(v, "1");
}

bool plat_claim()
{
    char v[8];
    if (GetEnvironmentVariableA("CURTIMEFIX_LOADED", v, sizeof v)) return false;
    SetEnvironmentVariableA("CURTIMEFIX_LOADED", "1");
    return true;
}

struct ThreadArg { void *(*fn)(void *); };
static DWORD WINAPI thread_main(void *p)
{
    auto fn = ((ThreadArg *)p)->fn;
    delete (ThreadArg *)p;
    fn(nullptr);
    return 0;
}

bool plat_thread(void *(*fn)(void *))
{
    auto *a = new ThreadArg{fn};
    HANDLE h = CreateThread(nullptr, 0, thread_main, a, 0, nullptr);
    if (!h) { delete a; return false; }
    CloseHandle(h);
    return true;
}

void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }

void plat_echo_console() {}
#endif
