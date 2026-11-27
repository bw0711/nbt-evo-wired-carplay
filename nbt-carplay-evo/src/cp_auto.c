/* nbt_carplay, NBT Evo port: --auto, the bridge as a service (wired-carplay/car/autostart/).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The head unit's own CarPlay looks for the iPhone it knows by Bluetooth (iDrive mode "Apple CarPlay") when the phone
 * connects and 2-3 s after every session: its plug-in logs "[CarPlay][DevCtrl] Device with BT Address=... is not found
 * in available device list" (or "Connection failed with the device ...") once a second, for 58 s (car 2026-11-27).
 * With the iPhone's Wi-Fi on, wireless CarPlay comes up 1-3 s into that search; with it off the search runs its 58 s.
 * A session that reaches the head unit outside a search stays inactive (no iDrive entry, 26.11). So in --auto the
 * bridge leaves a plugged-in iPhone alone (charging, the car's iPod handlers, a wireless session) until a search has
 * run GATE_MS in a row, then takes it over; when the head unit ends the session, the bridge ends too and the service
 * starts it again (the head unit searches again right after).
 *
 * Read from `sloginfo -w` (NBT_CP_SLOG_CMD overrides it, for tests). A line counts by the wall-clock time the plug-in
 * writes into it ("2026-11-27 12:04:03.709813 PM", UTC like the head unit's clock), so the old lines sloginfo prints
 * first do not count.
 *
 * Before the takeover: wired iAP2 for the cluster (--iap2-factory) only when nbtcp_iap2_adapter listens and no session
 * of this boot ended within QUICK_END_MS of its start (/dev/shmem/nbtcp-now1); the w1 patch in Connectivity is applied
 * with it and reverted without it (Connectivity ends a wired session 10 s after its start when mm-ipod does not
 * connect, car 27.11). And a pass rule for tap0 in the pf anchor filter_USB (the main ruleset blocks every interface it
 * does not name), emptied again at the end.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __QNXNTO__
#include <spawn.h>
#else
#include <spawn.h>
extern char** environ;
#endif
#include "cp.h"

#define GATE_MS 6000        /* a search this long: the iPhone's Wi-Fi is off */
#define FRESH_MS 2500       /* the search goes on: its last line at most this old */
#define STALE_MS 5000       /* lines older (or newer) than this against our clock do not count */
#define QUICK_END_MS 15000  /* a wired iAP2 session that ends this soon: w1 / --iap2-factory off for this boot */
#ifndef NOW1_FLAG
#define NOW1_FLAG "/dev/shmem/nbtcp-now1"
#endif
#ifndef PF_FILE
#define PF_FILE "/dev/shmem/nbtcp-tap0.pf"
#endif
#ifndef ADAPTER_PORT
#define ADAPTER_PORT 47600
#endif

static struct {
    pthread_mutex_t m;
    long long streak, look;     /* wall ms: the current search's first and last line */
    long long up, end;          /* wall ms: the last "Session is UP" / "Session terminated" lines */
    long long taken;            /* wall ms: when we took the iPhone over (0: not yet) */
    int factory, pf, code;
    pid_t slog;
} g_a = {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0, 0, 0, 0, 0, -1};

long long auto_wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* the plug-in's own time stamp in a line ("... 2026-11-27 12:04:03.709813 PM [CarPlay][..."), ms; -1 if none */
long long auto_stamp(const char* line)
{
    const char* q = strstr(line, " [CarPlay][");
    int y, mo, d, h, mi, s, us;
    char ap;
    if (!q || q - line < 29 || q[-1] != 'M' || (q[-2] != 'A' && q[-2] != 'P'))
        return -1;
    ap = q[-2];
    if (sscanf(q - 29, "%4d-%2d-%2d %2d:%2d:%2d.%6d", &y, &mo, &d, &h, &mi, &s, &us) != 7 || mo < 1 || mo > 12 ||
        d < 1 || d > 31 || h < 1 || h > 12 || mi > 59 || s > 60)
        return -1;
    h %= 12;
    if (ap == 'P')
        h += 12;
    {   /* days since 1970-01-01 (H. Hinnant's days_from_civil) */
        const long long yy = mo <= 2 ? y - 1 : y;
        const long long era = (yy >= 0 ? yy : yy - 399) / 400;
        const unsigned yoe = (unsigned)(yy - era * 400);
        const unsigned doy = (153u * (unsigned)(mo > 2 ? mo - 3 : mo + 9) + 2) / 5 + (unsigned)d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const long long days = era * 146097 + (long long)doe - 719468;
        return ((days * 24 + h) * 60 + mi) * 60000LL + s * 1000LL + us / 1000;
    }
}

/* one line of the head unit's log, `now` = auto_wall_ms() when it was read */
void auto_line(const char* line, long long now)
{
    long long t;
    if (!strstr(line, "[CarPlay][DevCtrl]") || (t = auto_stamp(line)) < 0 || t < now - STALE_MS || t > now + STALE_MS)
        return;
    pthread_mutex_lock(&g_a.m);
    if (strstr(line, "is not found in available device list") || strstr(line, "Connection failed with the device")) {
        if (t - g_a.look > FRESH_MS)
            g_a.streak = t;
        g_a.look = t;
    } else if (strstr(line, "CarPlay Session is UP")) {
        g_a.up = t;
        g_a.look = g_a.streak = 0;   /* a session (wireless, or ours) ended the search */
    } else if (strstr(line, "CarPlay Session terminated"))
        g_a.end = t;
    pthread_mutex_unlock(&g_a.m);
}

/* the search for the iPhone has run GATE_MS and goes on */
int auto_gate_open(long long now)
{
    int open;
    pthread_mutex_lock(&g_a.m);
    open = g_a.look && now - g_a.look <= FRESH_MS && g_a.look - g_a.streak >= GATE_MS;
    pthread_mutex_unlock(&g_a.m);
    return open;
}

/* after the takeover: the head unit's session with us has ended (0: not / not yet) */
int auto_ended(void)
{
    int ended = 0;
    pthread_mutex_lock(&g_a.m);
    if (g_a.taken && g_a.up >= g_a.taken && g_a.end > g_a.up) {
        ended = 1;
        if (!g_a.code) {
            g_a.code = 3;
            if (g_a.factory && g_a.end - g_a.up < QUICK_END_MS) {
                g_a.code = 4;
                flag_set(NOW1_FLAG, "a wired session with iAP2 for the cluster ended within 15 s\n");
            }
        }
    }
    pthread_mutex_unlock(&g_a.m);
    return ended;
}

int auto_code(void)
{
    return g_a.code;
}

int auto_taken(void)
{
    return g_a.taken != 0;
}

/* --- reading the head unit's log --- */

static pid_t run_reader(int* fd)
{
    const char* cmd = getenv("NBT_CP_SLOG_CMD");
    char line[512];
    char* argv[4];
    int p[2];
    pid_t pid = -1;
    snprintf(line, sizeof(line), "exec %s", cmd && *cmd ? cmd : "sloginfo -w");
    argv[0] = (char*)"sh";
    argv[1] = (char*)"-c";
    argv[2] = line;
    argv[3] = 0;
    if (pipe(p))
        return -1;
#ifdef __QNXNTO__
    {
        struct inheritance inh;
        const int null = open("/dev/null", O_RDWR);
        int map[3];
        memset(&inh, 0, sizeof(inh));
        inh.flags = SPAWN_SETSIGMASK | SPAWN_SETSIGDEF | SPAWN_NOZOMBIE;
        sigemptyset(&inh.sigmask);
        sigfillset(&inh.sigdefault);
        map[0] = null;
        map[1] = p[1];
        map[2] = null;
        pid = spawn("/bin/sh", 3, map, &inh, argv, environ);
        if (null >= 0)
            close(null);
    }
#else
    {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, p[1], 1);
        posix_spawn_file_actions_addclose(&fa, p[0]);
        if (posix_spawn(&pid, "/bin/sh", &fa, 0, argv, environ))
            pid = -1;
        posix_spawn_file_actions_destroy(&fa);
    }
#endif
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    *fd = p[0];
    return pid;
}

static void* reader(void* arg)
{
    static char buf[8192];
    unsigned n = 0;
    (void)arg;
    while (!g_stop) {
        int fd = -1;
        const pid_t pid = run_reader(&fd);
        if (pid < 0) {
            say("auto: the head unit's log cannot be read (%s); trying again in 5 s", strerror(errno));
            sleep(5);
            continue;
        }
        pthread_mutex_lock(&g_a.m);
        g_a.slog = pid;
        pthread_mutex_unlock(&g_a.m);
        n = 0;
        for (;;) {
            const ssize_t r = read(fd, buf + n, sizeof(buf) - 1 - n);
            char *s, *e;
            if (r <= 0)
                break;
            n += (unsigned)r;
            buf[n] = 0;
            s = buf;
            while ((e = memchr(s, '\n', (size_t)(buf + n - s))) != 0) {
                *e = 0;
                auto_line(s, auto_wall_ms());
                s = e + 1;
            }
            n = (unsigned)(buf + n - s);
            memmove(buf, s, n);
            if (n == sizeof(buf) - 1)   /* a line longer than the buffer: dropped */
                n = 0;
        }
        close(fd);
        pthread_mutex_lock(&g_a.m);
        g_a.slog = -1;
        pthread_mutex_unlock(&g_a.m);
        kill(pid, SIGTERM);
#ifndef __QNXNTO__
        waitpid(pid, 0, 0);
#endif
        if (!g_stop) {
            say("auto: the head unit's log reader ended; starting it again");
            sleep(1);
        }
    }
    return 0;
}

int auto_start(void)
{
    static int started;
    pthread_t t;
    if (started)
        return 0;
    if (pthread_create(&t, 0, reader, 0))
        return -1;
    pthread_detach(t);
    started = 1;
    return 0;
}

/* --- before the takeover and at the end --- */

static int adapter_listens(void)
{
    struct sockaddr_in sa;
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    int ok;
    if (s < 0)
        return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ADAPTER_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = connect(s, (struct sockaddr*)&sa, sizeof(sa)) == 0;
    close(s);
    return ok;
}

static int run(const char* cmd)
{
    const int rc = system(cmd);
    return rc == -1 ? -1 : WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* the takeover is about to happen: decide wired iAP2, set w1 to match, open the firewall for tap0 */
void auto_prepare(void)
{
    const char* patch = getenv("NBT_CP_PATCH") ? getenv("NBT_CP_PATCH") : "/var/opt/nbt-cp/conn_patch";
    char cmd[400];
    int factory = g_opt.iap2_factory != 0;
    if (factory && access(NOW1_FLAG, F_OK) == 0) {
        say("auto: an earlier wired session of this boot ended within 15 s: no iAP2 for the cluster this time");
        factory = 0;
    }
    if (factory && !adapter_listens()) {
        say("auto: nbtcp_iap2_adapter does not listen on 127.0.0.1:%d: no iAP2 for the cluster", ADAPTER_PORT);
        factory = 0;
    }
    if (access(patch, X_OK) != 0) {
        if (factory)
            say("auto: no %s: no iAP2 for the cluster", patch);
        factory = 0;
    } else {
        snprintf(cmd, sizeof(cmd), "%s %s w1", patch, factory ? "apply" : "revert");
        if (run(cmd) != 0 && factory) {
            say("auto: w1 could not be applied: no iAP2 for the cluster");
            factory = 0;
            snprintf(cmd, sizeof(cmd), "%s revert w1", patch);
            run(cmd);
        }
    }
    g_opt.iap2_factory = factory ? g_opt.iap2_factory : 0;
    if (run("echo 'pass quick on tap0 all' > " PF_FILE " && pfctl -a filter_USB -f " PF_FILE) == 0)
        g_a.pf = 1;
    else
        say("auto: the pf rule for tap0 could not be set");
    pthread_mutex_lock(&g_a.m);
    g_a.factory = factory;
    g_a.taken = auto_wall_ms();
    pthread_mutex_unlock(&g_a.m);
    say("auto: taking the iPhone over (%s)", factory ? "with iAP2 for the cluster" : "without iAP2 for the cluster");
}

void auto_finish(void)
{
    pid_t pid;
    if (g_a.pf) {
        run("pfctl -a filter_USB -F rules; rm -f " PF_FILE);
        g_a.pf = 0;
    }
    pthread_mutex_lock(&g_a.m);
    pid = g_a.slog;
    pthread_mutex_unlock(&g_a.m);
    if (pid > 0)
        kill(pid, SIGTERM);
}
