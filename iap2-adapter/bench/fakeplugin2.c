/* IDEmu bench: stands for the CarPlay plug-in (libconn_carplay.so) closely enough for the real adapters to run behind
 * ours: the factory's libconn_iap2_wifi_qnx.so and nbt-aa's libnbt-cpadapter.so. On the car Connectivity's libraries
 * give them CConfigReader and CUtilsMutex; here this program exports them (link with -Wl,-E):
 *   CConfigReader::getString answers [iAP2] DevicePath = /dev/carplayiap (as the car's carplay.ini), "" otherwise.
 *   It returns a Dinkum std::string (libcpp-ne, as the factory adapter builds them inline): allocator byte padded to 4,
 *   16-byte buffer or pointer, size, reserve; the caller passes the result's address in r0 and the object in r1
 *   (libconn_iap2_wifi_qnx.so 0x165c..0x1668; it reads the reserve at +24).
 *   CUtilsMutex: one recursive mutex for all.
 * Then as libconn_carplay.so: dlopen(argv[1], RTLD_LAZY) (readConfig), iAP2WiFiAdapterGet, initialize(cfg, ctx, send),
 * setup (a wired session start with w1); on SIGTERM or SIGINT teardown and iAP2WiFiAdapterDispose (a clean end).
 * send() is the "AirPlay session": it logs what reaches it. */
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct dstr {
    unsigned char alval[4];
    union {
        char buf[16];
        char* ptr;
    } bx;
    unsigned size, res;
};
static const char* dstr_c(const struct dstr* s) { return s->res >= 16 ? s->bx.ptr : s->bx.buf; }

void _ZN13CConfigReader9getStringERKSsS1_i(struct dstr* ret, void* self, const struct dstr* sec, const struct dstr* key,
                                           int def)
{
    const char* v = "";
    size_t n;
    (void)self;
    (void)def;
    if (!strcmp(dstr_c(sec), "iAP2") && !strcmp(dstr_c(key), "DevicePath"))
        v = "/dev/carplayiap";
    n = strlen(v);   /* < 16: in the buffer, nothing for the caller's destructor to free */
    memset(ret, 0, sizeof(*ret));
    memcpy(ret->bx.buf, v, n + 1);
    ret->size = (unsigned)n;
    ret->res = 15;
    printf("fakeplugin2: config [%s] %s -> \"%s\"\n", dstr_c(sec), dstr_c(key), v);
    fflush(stdout);
}

static pthread_mutex_t g_mu;
void* _ZN11CUtilsMutexC1Ev(void* self) { return self; }
void* _ZN11CUtilsMutexD1Ev(void* self) { return self; }
int _ZN11CUtilsMutex4lockEv(void* self) { (void)self; return pthread_mutex_lock(&g_mu); }
int _ZN11CUtilsMutex6unlockEv(void* self) { (void)self; return pthread_mutex_unlock(&g_mu); }

typedef int (*send_fn)(const unsigned char*, unsigned, void*);
static int air_send(const unsigned char* d, unsigned n, void* ctx)
{
    printf("fakeplugin2: %u bytes into the \"AirPlay session\" (ctx %p, first %02x %02x)\n", n, ctx, n ? d[0] : 0,
           n > 1 ? d[1] : 0);
    fflush(stdout);
    return (int)n;
}

static volatile sig_atomic_t g_end;
static void on_signal(int s) { (void)s; g_end = 1; }

int main(int argc, char** argv)
{
    static char cfg[256];   /* "CConfigReader": only passed back to getString */
    pthread_mutexattr_t ma;
    void* lib;
    void* (*get)(void);
    void (*dispose)(void*);
    void** ad;
    void** vt;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mu, &ma);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    if (argc < 2 || !(lib = dlopen(argv[1], RTLD_LAZY))) {
        printf("fakeplugin2: %s\n", argc < 2 ? "usage: fakeplugin2 ADAPTER.so" : dlerror());
        return 1;
    }
    get = (void* (*)(void))dlsym(lib, "iAP2WiFiAdapterGet");
    dispose = (void (*)(void*))dlsym(lib, "iAP2WiFiAdapterDispose");
    ad = get ? get() : 0;
    if (!ad) {
        printf("fakeplugin2: no adapter object\n");
        return 1;
    }
    vt = *(void***)ad;
    ((int (*)(void*, void*, void*, send_fn))vt[0])(ad, cfg, (void*)0xc0ffee, air_send);
    ((void (*)(void*))vt[1])(ad);
    printf("fakeplugin2: adapter initialized and set up\n");
    fflush(stdout);
    while (!g_end)
        usleep(200000);
    ((void (*)(void*))vt[2])(ad);
    if (dispose)
        dispose(ad);
    printf("fakeplugin2: torn down and disposed\n");
    fflush(stdout);
    return 0;
}
