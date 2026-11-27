/* nbtcp_iap2_adapter.so: the CarPlay plug-in's iAP2 adapter (carplay.ini [iAP2] LibPath) for wired CarPlay.
 *
 * The head unit's CarPlay plug-in (libconn_carplay.so) loads the adapter named in [iAP2] LibPath and calls
 * iAP2WiFiAdapterGet() -> adapter->initialize(config, ctx, send). The adapter owns /dev/carplayiap, the node the factory
 * iAP2 driver (mm-ipod, OSCAR package CONN_CARPLAY_WLAN_DRIVER) talks through; whatever mm-ipod writes there goes to
 * send() (into the AirPlay session: wireless CarPlay carries iAP2 inside AirPlay) and the iPhone's answers come back
 * through messageReceived(). Over the cable the iPhone keeps iAP2 on USB (with nbt_carplay's bridge), so the factory
 * gets no "now playing", calls, location... (wired-carplay/iap2-factory-re-2026-11-27.md).
 *
 * This library stands in front of the adapter it replaces (the next one: the factory libconn_iap2_wifi_qnx.so, or a
 * third-party adapter already in LibPath) and passes every call on to it unchanged, with one difference: the send()
 * the next adapter gets is ours. While nbt_carplay's bridge is connected to 127.0.0.1:47600 (wired CarPlay), what
 * mm-ipod writes goes to the bridge, and what the bridge sends goes into the node through the next adapter's
 * messageReceived(); the bridge answers mm-ipod in the iPhone's place. Without the bridge, send() goes into the AirPlay
 * session exactly as before (wireless CarPlay unchanged).
 *
 * The next adapter: the path in /var/opt/nbt-cp/iap2-next (one line, written by the installer from the LibPath it
 * replaced), else the factory one. Frames to and from the bridge: type (1 = iAP2 bytes), length (2 bytes, big-endian),
 * the bytes.
 *
 * Wired CarPlay as a service (wired-carplay/car/autostart/): when AUTOSTART exists, initialize() starts SERVICE (cpd.sh,
 * which runs nbt_carplay --auto again and again) once per boot: spawned with only /dev/null as its files, in a process
 * group of its own, with no signal blocked or caught, without a zombie when it ends; not again while the pid it wrote
 * to SERVICE_PID lives (Connectivity restarted). Nothing of it runs inside Connectivity; cpd.sh is its own watchdog.
 *
 * Safety: this runs inside Connectivity, where a crash means a core dump and a head unit restart. The plug-in makes
 * the adapter once per start of Connectivity (CCarPlayServiceCore::start -> readConfig). If two starts in a row did not
 * last HEALTHY_S seconds (STARTS counts them), or DISABLE exists, iAP2WiFiAdapterGet hands the plug-in the next
 * adapter's own object: none of our code runs in between, as before the install (only wired iAP2 is off). A start
 * that lasts HEALTHY_S seconds, or ends with a clean dispose, clears the count; a safe start resets it to 0, so the
 * start after it tries again. Two short writes to STATE_DIR per start.
 *
 * The plug-in sees a C++ object: the first word is a pointer to a table of member functions (vtable of the factory's
 * CiAP2WiFiAdapterQNX: initialize, setup, teardown, messageReceived, complete and deleting destructor); ARM EABI passes
 * "this" first, so plain C functions fill the table.
 */
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#ifndef STATE_DIR
#define STATE_DIR "/var/opt/nbt-cp"
#endif
#define NEXT_CONF STATE_DIR "/iap2-next"
#define STARTS STATE_DIR "/starts"     /* starts of Connectivity that did not last HEALTHY_S yet */
#define DISABLE STATE_DIR "/disable"   /* exists: do nothing at all (by hand: touch it over SSH) */
#define AUTOSTART STATE_DIR "/autostart" /* exists: start SERVICE (wired CarPlay without the laptop) */
#define SERVICE STATE_DIR "/cpd.sh"
#ifndef SERVICE_PID
#define SERVICE_PID "/dev/shmem/nbt-cpd.pid"   /* written by cpd.sh */
#endif
#ifndef HEALTHY_S
#define HEALTHY_S 120
#endif
#ifndef LOG_PATH
#define LOG_PATH "/dev/shmem/nbtcp-iap2.log"   /* RAM; started anew when over 64 KB */
#endif
#define NEXT_DEFAULT "/opt/conn/lib/carplay/devctrl/libconn_iap2_wifi_qnx.so"
#ifndef BRIDGE_PORT
#define BRIDGE_PORT 47600
#endif
#define F_IAP2 1

typedef int (*send_fn)(const unsigned char*, unsigned, void*);

struct cxx {                      /* a C++ adapter object: its first word points at its member functions */
    void** vptr;
};
enum { V_INIT, V_SETUP, V_TEARDOWN, V_RECEIVED, V_DTOR, V_DELETE };
typedef int (*init_fn)(struct cxx*, void*, void*, send_fn);
typedef void (*void_fn)(struct cxx*);
typedef void (*recv_fn)(struct cxx*, const unsigned char*, unsigned);

struct adapter {
    void** vptr;                  /* first: the plug-in calls through it */
    pthread_t health;
    int health_on;
    struct cxx* next;             /* the adapter we stand in front of */
    void* next_lib;
    void (*next_dispose)(void*);
    send_fn up_send;              /* the plug-in's send: into the AirPlay session */
    int ls, cs;                   /* listening socket, the bridge's connection (-1: none) */
    pthread_t thr;
    int thr_on;
    pthread_mutex_t mu;           /* cs and writes to it */
    volatile int stop;
    unsigned long frames_up, frames_down;
};

static struct adapter* g_ad;      /* the plug-in makes one; send() reaches it through here */
static void* g_pass;              /* safe mode: the next adapter's object, handed to the plug-in as is */
static void (*g_pass_dispose)(void*);

static void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char* fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    {
        struct stat st;
        char line[320];
        int fd, n;
        const int trunc = stat(LOG_PATH, &st) == 0 && st.st_size > 65536;
        n = snprintf(line, sizeof(line), "%ld nbtcp-iap2: %s\n", (long)time(0), b);
        fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | (trunc ? O_TRUNC : 0), 0644);
        if (fd >= 0) {
            if (write(fd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1)) < 0)
                n = 0;
            close(fd);
        }
#ifdef ADAPTER_TEST
        fprintf(stderr, "%s", line);
#endif
    }
}

static int full_write(int fd, const unsigned char* p, unsigned n)
{
    while (n) {
        const ssize_t k = write(fd, p, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += k;
        n -= (unsigned)k;
    }
    return 0;
}

static int full_read(int fd, unsigned char* p, unsigned n)
{
    while (n) {
        const ssize_t k = read(fd, p, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += k;
        n -= (unsigned)k;
    }
    return 0;
}

/* the send() the next adapter gets: to the bridge while it is there, else into the AirPlay session */
static int our_send(const unsigned char* data, unsigned len, void* ctx)
{
    struct adapter* a = g_ad;
    int to_bridge = 0, rc = (int)len;
    if (a) {
        pthread_mutex_lock(&a->mu);
        if (a->cs >= 0) {
            static unsigned char f[3 + 0xffff];   /* one write per frame (under mu) */
            to_bridge = 1;
            f[0] = F_IAP2;
            f[1] = (unsigned char)(len >> 8);
            f[2] = (unsigned char)len;
            if (len <= 0xffff)
                memcpy(f + 3, data, len);
            if (len > 0xffff || full_write(a->cs, f, 3 + len)) {
                logf_("write to the bridge failed (%s); back to the AirPlay session", strerror(errno));
                close(a->cs);
                a->cs = -1;
                rc = 9;   /* the factory send's failure value */
            } else
                a->frames_up++;
        }
        pthread_mutex_unlock(&a->mu);
        if (!to_bridge && a->up_send)
            return a->up_send(data, len, ctx);
        return rc;
    }
    return -1;
}

/* one loop for both sockets: a second bridge is refused at once, and the bridge's frames go into the node */
static void bridge_drop(struct adapter* a, int c, const char* why)
{
    pthread_mutex_lock(&a->mu);
    if (a->cs == c) {
        close(c);
        a->cs = -1;
    }
    pthread_mutex_unlock(&a->mu);
    logf_("bridge gone (%s): back to the AirPlay session (%lu frames up, %lu down)", why, a->frames_up, a->frames_down);
}

static void* serve(void* arg)
{
    struct adapter* a = arg;
    static unsigned char buf[0x10000];
    while (!a->stop) {
        fd_set r;
        struct timeval tv = {0, 300000};
        int c = a->cs, mx = a->ls;
        FD_ZERO(&r);
        FD_SET(a->ls, &r);
        if (c >= 0) {
            FD_SET(c, &r);
            if (c > mx)
                mx = c;
        }
        if (select(mx + 1, &r, 0, 0, &tv) <= 0)
            continue;
        if (FD_ISSET(a->ls, &r)) {
            const int n = accept(a->ls, 0, 0);
            if (n >= 0) {
                struct timeval st = {1, 0};   /* a stuck bridge must not hold mm-ipod's writer (or us) for long */
                const int one = 1;
                setsockopt(n, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
                setsockopt(n, SOL_SOCKET, SO_RCVTIMEO, &st, sizeof(st));
                setsockopt(n, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                pthread_mutex_lock(&a->mu);
                if (a->cs >= 0) {
                    pthread_mutex_unlock(&a->mu);
                    close(n);             /* one bridge at a time */
                } else {
                    a->cs = n;
                    pthread_mutex_unlock(&a->mu);
                    logf_("bridge connected: mm-ipod talks to it (wired CarPlay)");
                }
            }
        }
        if (c >= 0 && FD_ISSET(c, &r)) {
            unsigned char h[3];
            unsigned n;
            if (full_read(c, h, 3)) {
                bridge_drop(a, c, "closed");
                continue;
            }
            n = (unsigned)h[1] << 8 | h[2];
            if (full_read(c, buf, n)) {
                bridge_drop(a, c, "cut in a frame");
                continue;
            }
            if (h[0] == F_IAP2 && n && a->next)
                ((recv_fn)a->next->vptr[V_RECEIVED])(a->next, buf, n);
            a->frames_down++;
        }
    }
    return 0;
}

static int listen_local(void)
{
    struct sockaddr_in sa;
    const int one = 1;
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(BRIDGE_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) || listen(s, 1)) {
        close(s);
        return -1;
    }
    return s;
}

#ifdef ADAPTER_TEST
__attribute__((visibility("default"))) struct cxx* (*test_next_get)(void);   /* the host test's fake next adapter */
#endif

/* loads the next adapter and makes its object (0 if it cannot); the library stays loaded */
static struct cxx* open_next(void (**dispose)(void*))
{
    char path[256] = NEXT_DEFAULT;
    FILE* f;
    void* lib;
    struct cxx* (*get)(void);
    struct cxx* n;
    *dispose = 0;
#ifdef ADAPTER_TEST
    if (test_next_get)
        return test_next_get();
#endif
    f = fopen(NEXT_CONF, "r");
    if (f) {
        if (fgets(path, sizeof(path), f))
            path[strcspn(path, "\r\n")] = 0;
        fclose(f);
    }
    lib = dlopen(path, RTLD_LAZY);   /* as the plug-in loads LibPath (libconn_carplay readConfig: dlopen(path, 1)) */
    if (!lib) {
        logf_("cannot load the next adapter %s: %s", path, dlerror());
        return 0;
    }
    get = (struct cxx * (*)(void)) dlsym(lib, "iAP2WiFiAdapterGet");
    *dispose = (void (*)(void*))dlsym(lib, "iAP2WiFiAdapterDispose");
    n = get ? get() : 0;
    logf_("next adapter %s: %s", path, n ? "loaded" : "no iAP2WiFiAdapterGet");
    return n;
}

static void load_next(struct adapter* a)
{
    a->next = open_next(&a->next_dispose);
}

static int read_count(void)
{
    char b[16] = "";
    FILE* f = fopen(STARTS, "r");
    if (!f)
        return 0;
    if (!fgets(b, sizeof(b), f))
        b[0] = 0;
    fclose(f);
    return atoi(b);
}

static void write_count(int n)
{
    FILE* f = fopen(STARTS, "w");
    if (f) {
        fprintf(f, "%d\n", n);
        fclose(f);
    }
}

static void* healthy(void* arg)
{
    struct adapter* a = arg;
    int i;
    for (i = 0; i < HEALTHY_S * 10 && !a->stop; ++i)
        usleep(100000);
    if (!a->stop) {
        write_count(0);
        logf_("running for %d s: start count cleared", HEALTHY_S);
    }
    return 0;
}

/* --- the member functions the plug-in calls --- */

#ifndef __QNXNTO__
extern char** environ;
#endif

/* AUTOSTART: cpd.sh, once (see the header) */
static void start_service(void)
{
    char b[32] = "";
    char* argv[3];
    FILE* f;
    pid_t pid = 0;
    if (access(AUTOSTART, F_OK) != 0)
        return;
    if ((f = fopen(SERVICE_PID, "r")) != 0) {
        if (fgets(b, sizeof(b), f))
            pid = (pid_t)atoi(b);
        fclose(f);
        if (pid > 0 && kill(pid, 0) == 0) {
            logf_("wired CarPlay service already running (pid %d)", (int)pid);
            return;
        }
    }
    if (access(SERVICE, R_OK) != 0) {
        logf_("no %s: wired CarPlay service not started", SERVICE);
        return;
    }
    argv[0] = (char*)"sh";
    argv[1] = (char*)SERVICE;
    argv[2] = 0;
#ifdef __QNXNTO__
    {
        struct inheritance inh;
        const int null = open("/dev/null", O_RDWR);
        int map[3];
        memset(&inh, 0, sizeof(inh));
        inh.flags = SPAWN_SETGROUP | SPAWN_SETSIGMASK | SPAWN_SETSIGDEF | SPAWN_NOZOMBIE;
        inh.pgroup = SPAWN_NEWPGROUP;
        sigemptyset(&inh.sigmask);
        sigfillset(&inh.sigdefault);
        map[0] = map[1] = map[2] = null;
        pid = null < 0 ? -1 : spawn("/bin/sh", 3, map, &inh, argv, environ);
        if (null >= 0)
            close(null);
    }
#else
    {   /* the host test */
        posix_spawn_file_actions_t fa;
        posix_spawnattr_t at;
        sigset_t none, all;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDWR, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_RDWR, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_RDWR, 0);
        posix_spawnattr_init(&at);
        sigemptyset(&none);
        sigfillset(&all);
        posix_spawnattr_setsigmask(&at, &none);
        posix_spawnattr_setsigdefault(&at, &all);
        posix_spawnattr_setpgroup(&at, 0);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        if (posix_spawn(&pid, "/bin/sh", &fa, &at, argv, environ))
            pid = -1;
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
    }
#endif
    if (pid > 0)
        logf_("wired CarPlay service started (pid %d)", (int)pid);
    else
        logf_("wired CarPlay service could not be started (%s)", strerror(errno));
}

static int m_initialize(struct adapter* a, void* config, void* ctx, send_fn send)
{
    a->up_send = send;
    if (pthread_create(&a->health, 0, healthy, a) == 0)
        a->health_on = 1;
    load_next(a);
    if (a->next)   /* the next keeps the plug-in's ctx (it may use it), but sends through us */
        ((init_fn)a->next->vptr[V_INIT])(a->next, config, ctx, our_send);
    a->ls = listen_local();
    if (a->ls < 0)
        logf_("cannot listen on 127.0.0.1:%d (%s): wired iAP2 off, wireless unchanged", BRIDGE_PORT, strerror(errno));
    else if (pthread_create(&a->thr, 0, serve, a) == 0)
        a->thr_on = 1;
    logf_("initialized");
    start_service();
    return 0;
}

static void m_setup(struct adapter* a)
{
    if (a->next)
        ((void_fn)a->next->vptr[V_SETUP])(a->next);
}

static void m_teardown(struct adapter* a)
{
    if (a->next)
        ((void_fn)a->next->vptr[V_TEARDOWN])(a->next);
}

static void m_received(struct adapter* a, const unsigned char* p, unsigned n)
{
    if (a->next)
        ((recv_fn)a->next->vptr[V_RECEIVED])(a->next, p, n);
}

static struct adapter* m_dtor(struct adapter* a)
{
    a->stop = 1;
    if (a->thr_on)
        pthread_join(a->thr, 0);
    if (a->health_on)
        pthread_join(a->health, 0);
    pthread_mutex_lock(&a->mu);
    if (a->cs >= 0)
        close(a->cs);
    a->cs = -1;
    pthread_mutex_unlock(&a->mu);
    if (a->ls >= 0)
        close(a->ls);
    a->ls = -1;
    if (a->next) {
        if (a->next_dispose)
            a->next_dispose(a->next);
        else
            ((void_fn)a->next->vptr[V_DELETE])(a->next);
    }
    a->next = 0;
    if (g_ad == a)
        g_ad = 0;
    if (read_count() != 0)
        write_count(0);   /* a clean end: no crash */
    logf_("disposed (bridge frames up %lu, down %lu)", a->frames_up, a->frames_down);
    return a;
}

static struct adapter* m_delete(struct adapter* a)
{
    m_dtor(a);
    free(a);
    return a;
}

/* offset-to-top, type info, then the member functions (the vtable pointer points at the first function) */
static void* g_vtable[] = {0, 0, (void*)m_initialize, (void*)m_setup, (void*)m_teardown, (void*)m_received,
                           (void*)m_dtor, (void*)m_delete};

__attribute__((visibility("default"))) void* iAP2WiFiAdapterGet(void)
{
    const int starts = read_count(), disabled = access(DISABLE, F_OK) == 0;
    struct adapter* a;
    if (disabled || starts >= 2) {
        logf_("SAFE MODE (%s): the next adapter alone, as before the install; car-chip-test.sh iap2-uninstall puts "
              "it back for good", disabled ? DISABLE " exists" : "two starts in a row did not last");
        if (!disabled)
            write_count(0);   /* the next start tries again */
        g_pass = open_next(&g_pass_dispose);
        return g_pass;        /* 0 is fine: the plug-in then runs without an iAP2 adapter, as with a missing LibPath */
    }
    write_count(starts + 1);
    a = calloc(1, sizeof(*a));
    if (!a)
        return 0;
    a->vptr = &g_vtable[2];
    a->ls = a->cs = -1;
    pthread_mutex_init(&a->mu, 0);
    g_ad = a;
    return a;
}

__attribute__((visibility("default"))) void iAP2WiFiAdapterDispose(void* p)
{
    if (!p)
        return;
    if (p == g_pass) {   /* safe mode: the next adapter's own object */
        g_pass = 0;
        if (g_pass_dispose)
            g_pass_dispose(p);
        else
            ((void_fn)((struct cxx*)p)->vptr[V_DELETE])(p);
        return;
    }
    m_delete(p);
}
