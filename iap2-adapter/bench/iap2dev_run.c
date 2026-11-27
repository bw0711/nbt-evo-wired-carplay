/* IDEmu bench: cp_iap2dev.c on its own (no USB, no iPhone): answers mm-ipod through the adapter; once mm-ipod asks
 * for now playing, it gets a made-up NowPlayingUpdate (title, artist, album, playing). What mm-ipod asks of the
 * "iPhone" is logged. Run: iap2dev_run [noauth] */
#include <stdarg.h>
#include <time.h>
#include "../../nbt-carplay-evo/src/cp_iap2dev.c"
struct cp_options g_opt;
volatile int g_stop;
void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); fflush(stdout); }
long long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
void put32(unsigned char* p, unsigned v) { put16(p, v >> 16); put16(p + 2, v & 0xffff); }
unsigned get16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
unsigned get32(const unsigned char* p) { return get16(p) << 16 | get16(p + 2); }
static int phone(unsigned id, const unsigned char* p, unsigned n)
{
    (void)p;
    say("bench: mm-ipod asks the iPhone %04x (%u bytes of parameters)", id, n);
    return 0;
}
int main(int argc, char** argv)
{
    int sent = 0;
    iap2dev_start(!(argc > 1 && !strcmp(argv[1], "noauth")), "Bench iPhone");
    for (;;) {
        usleep(100000);
        iap2dev_drain(phone);
        if (g_d.sub_np && !sent) {
            unsigned char g[256], m[300];
            unsigned gn = 0, at = 0, k;
            const unsigned char playing = 1;
            gn = dprm(g, gn, 1, "Bench Title", 12);
            gn = dprm(g, gn, 12, "Bench Artist", 13);
            gn = dprm(g, gn, 6, "Bench Album", 12);
            at = dprm(m, at, 0, g, gn);
            k = dprm(g, 0, 0, &playing, 1);
            at = dprm(m, at, 1, g, k);
            iap2dev_from_phone(0x5001, m, at);
            say("bench: NowPlayingUpdate sent to mm-ipod (Bench Title / Bench Artist, playing)");
            sent = 1;
        }
    }
}
