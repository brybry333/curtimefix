// test_ticktime: tick_time() over map ages, against the two forms the game stores. `make test` in linux/.
#include "../src/fix.cpp"
#include <initializer_list>

bool plat_module(const char *, ModInfo *) { return false; }
void *plat_interface(const char *, const char *) { return nullptr; }
void *plat_conmsg() { return nullptr; }
void *plat_swap_ptr(void **, void *) { return nullptr; }
bool plat_write_code(void *, const void *, size_t) { return false; }
void *plat_alloc_near(uintptr_t, uintptr_t, size_t) { return nullptr; }
bool plat_make_rx(void *, size_t) { return false; }
bool plat_in_game() { return false; }
bool plat_disabled() { return true; }
bool plat_claim() { return false; }
bool plat_thread(void *(*)(void *)) { return false; }
void plat_sleep_ms(int) {}
double plat_now() { return 0; }
int vt_slot(int) { return 0; }
bool find_all(const ModInfo &, const ModInfo &, void *, Found *) { return false; }
bool cmd_register() { return false; }

int main()
{
    int bad = 0;
    for (float If : {0.015f, 0.01f}) {   // 66.67 and 100 tick
        for (double age : {60.0, 2048.0, 8192.0, 32768.0, 65536.0, 85000.0, 120000.0, 140000.0}) {
            long long n0 = (long long)(age / If), wrong = 0, missed = 0;
            for (long long N = n0; N < n0 + 20000; N++) {
                volatile float plain = (float)N * If, reset = plain - If;   // tick N, and tick N-1 after a prediction reset
                for (int k = 0; k < 2; k++) {
                    double ex;
                    Gate g = {0, 0, (double)If, 0, 0};
                    if (!tick_time(k ? reset : plain, g, &ex)) missed++;
                    else if (ex != (double)(k ? N - 1 : N) * (double)If) wrong++;
                }
            }
            bool fail = wrong || (age < 131072 && missed);   // past 2^17 s two ticks fit: left as stored is right there
            printf("%5.1f tick %7.0f s: wrong %lld, not recognised %lld%s\n", 1 / If, age, wrong, missed, fail ? "  FAIL" : "");
            bad += fail;
        }
    }
    return bad != 0;
}
