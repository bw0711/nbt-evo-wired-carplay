/* Mac test of cp_auto.c (--auto): the head unit's search for the iPhone, read from its log. GPL-3.0-only
 * From nbt-carplay-evo/:
 *   clang -std=gnu99 -g -fsanitize=address,undefined -Wall -Wextra -Isrc test-host/auto_test.c \
 *     -o test-host/auto_test -lpthread && ./test-host/auto_test
 * 1. the plug-in's time stamps (AM/PM, January/February, 12 AM / 12 PM);
 * 2. the gate, with the timings the car showed on 27.11: Wi-Fi off = a search of a line a second for 58 s (opens after
 *    6 s); Wi-Fi on = wireless up 1-3 s into it (never opens); old lines (sloginfo prints its buffer first) and a
 *    search that stopped do not count;
 * 3. the session's end after the takeover: exit code 3, or 4 and the "no w1" flag when it ended within 15 s with iAP2;
 * 4. the reader: a real child process writing lines with the current time through a pipe (as `sloginfo -w`). */
#include <assert.h>
#include <stdarg.h>
#define NOW1_FLAG "/tmp/nbtcp-now1-test"
#define PF_FILE "/tmp/nbtcp-tap0-test.pf"
#include "../src/cp_auto.c"

struct cp_options g_opt;
volatile int g_stop;
void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("  [auto] "); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
void flag_set(const char* path, const char* text)
{
    FILE* f = fopen(path, "w");
    if (f) { fputs(text ? text : "", f); fclose(f); }
}

/* a DevCtrl line with the plug-in's own stamp for wall time t (ms) */
static const char* line_at(long long t, const char* what)
{
    static char b[400];
    const time_t s = (time_t)(t / 1000);
    struct tm tm;
    int h;
    gmtime_r(&s, &tm);
    h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    snprintf(b, sizeof(b), "Jan 01 00:03:08    1     6     1 %04d-%02d-%02d %02d:%02d:%02d.%06d %s [CarPlay][DevCtrl] %s",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, h, tm.tm_min, tm.tm_sec, (int)(t % 1000) * 1000,
             tm.tm_hour < 12 ? "AM" : "PM", what);
    return b;
}
#define NF "Device with BT Address=AA:BB:CC:DD:EE:FF is not found in available device list"
#define UP "CarPlay Session is UP and RUNNING on Transport: ?"
#define END "CarPlay Session terminated"

static void reset(void)
{
    pthread_mutex_lock(&g_a.m);
    g_a.streak = g_a.look = g_a.up = g_a.end = g_a.taken = 0;
    g_a.factory = g_a.code = 0;
    pthread_mutex_unlock(&g_a.m);
}

int main(void)
{
    long long t0, t;
    int i;
    /* 1. time stamps */
    assert(auto_stamp("x 2026-11-27 12:04:03.709813 PM [CarPlay][DevCtrl] y") == 1795781043000LL + 709);
    assert(auto_stamp("x 2026-11-27 12:15:00.000000 AM [CarPlay][DevCtrl] y") == 1795738500000LL);
    assert(auto_stamp("x 2026-02-28 11:59:59.999999 PM [CarPlay][AirPlay] y") == 1772323199000LL + 999);
    assert(auto_stamp("x 2026-01-01 12:00:00.000000 AM [CarPlay][DevCtrl] y") == 1767225600000LL);
    assert(auto_stamp("x 2028-03-10 01:02:03.000000 AM [CarPlay][DevCtrl] y") == 1836262923000LL);
    assert(auto_stamp("Jan 01 00:03:08 nbt-cpadapter: syn") == -1);
    assert(auto_stamp("2026-11-27 13:04:03.709813 PM [CarPlay][DevCtrl]") == -1);   /* no 13 PM */
    assert(auto_stamp(" PM [CarPlay][x") == -1);

    /* 2a. Wi-Fi off: a line a second; opens once the search has run 6 s, closes when it stops */
    t0 = auto_wall_ms();
    reset();
    for (i = 0; i <= 5; ++i) {
        t = t0 + i * 1000;
        auto_line(line_at(t, NF), t);
        assert(!auto_gate_open(t));
    }
    t = t0 + 6000;
    auto_line(line_at(t, NF), t);
    assert(auto_gate_open(t));
    assert(auto_gate_open(t + 2400));
    assert(!auto_gate_open(t + 2600));   /* the search stopped */
    /* 2b. Wi-Fi on: wireless up 2 s into the search */
    reset();
    for (i = 0; i <= 2; ++i) {
        t = t0 + i * 1000;
        auto_line(line_at(t, NF), t);
    }
    auto_line(line_at(t0 + 2500, UP), t0 + 2500);
    for (i = 0; i < 20; ++i)
        assert(!auto_gate_open(t0 + 2500 + i * 500));
    /* 2c. old lines (the buffer sloginfo prints first) do not count */
    reset();
    for (i = 0; i < 59; ++i)
        auto_line(line_at(t0 - 3600000 + i * 1000, NF), t0);
    assert(!auto_gate_open(t0));
    /* 2d. a gap of more than 2.5 s starts a new search */
    reset();
    for (i = 0; i < 4; ++i)
        auto_line(line_at(t0 + i * 1000, NF), t0 + i * 1000);
    for (i = 0; i < 5; ++i)
        auto_line(line_at(t0 + 7000 + i * 1000, NF), t0 + 7000 + i * 1000);
    assert(!auto_gate_open(t0 + 11000));   /* 4 s into the new one */
    auto_line(line_at(t0 + 13000, NF), t0 + 13000);
    assert(auto_gate_open(t0 + 13000));    /* 6 s */
    /* 2e. other lines of the plug-in and of other programs change nothing */
    auto_line("Jan 01 00:03:47    5 10000     0 nbt-cpadapter: syn hu max_outstanding=10", t0 + 13000);
    auto_line(line_at(t0 + 13100, "Setting Night Mode to OFF"), t0 + 13100);
    assert(auto_gate_open(t0 + 13200));

    /* 3. after the takeover */
    reset();
    g_a.taken = t0;
    g_a.factory = 1;
    assert(!auto_ended());
    auto_line(line_at(t0 + 30000, UP), t0 + 30000);
    assert(!auto_ended());
    unlink(NOW1_FLAG);
    auto_line(line_at(t0 + 40002, END), t0 + 40002);   /* 10 s: the car on 27.11 without mm-ipod connected */
    assert(auto_ended() && auto_code() == 4);
    assert(access(NOW1_FLAG, F_OK) == 0);
    unlink(NOW1_FLAG);
    reset();
    g_a.taken = t0;
    g_a.factory = 1;
    auto_line(line_at(t0 + 30000, UP), t0 + 30000);
    auto_line(line_at(t0 + 300000, END), t0 + 300000);
    assert(auto_ended() && auto_code() == 3 && access(NOW1_FLAG, F_OK) != 0);
    reset();   /* an older session (before the takeover) does not count */
    auto_line(line_at(t0 - 20000, UP), t0 - 20000);
    auto_line(line_at(t0 - 10000, END), t0 - 10000);
    g_a.taken = t0;
    assert(!auto_ended());

    /* 4. the reader: lines with the current time from a child process, one a second; the gate opens after ~6 s */
    reset();
    setenv("NBT_CP_SLOG_CMD",
           "python3 -c \"import time,sys\nfor i in range(12):\n t=time.time(); g=time.gmtime(t); h=g.tm_hour%12 or 12\n"
           " print('Jan 01 00:00:00    1     6     1 %s %02d:%s.%06d %s [CarPlay][DevCtrl] Device with BT Address=X is "
           "not found in available device list' % (time.strftime('%Y-%m-%d', g), h, time.strftime('%M:%S', g), "
           "int(t%1*1e6), 'AM' if g.tm_hour<12 else 'PM'), flush=True); time.sleep(1)\nsys.stdout.flush(); "
           "time.sleep(30)\"", 1);
    assert(auto_start() == 0);
    {
        const long long s = auto_wall_ms();
        while (!auto_gate_open(auto_wall_ms()) && auto_wall_ms() - s < 12000)
            usleep(100000);
        printf("  gate opened after %.1f s of lines\n", (auto_wall_ms() - s) / 1000.0);
        assert(auto_gate_open(auto_wall_ms()));
        assert(auto_wall_ms() - s >= 5500);
    }
    g_stop = 1;
    auto_finish();   /* ends the child */
    usleep(300000);
    printf("auto test: all checks passed\n");
    return 0;
}
