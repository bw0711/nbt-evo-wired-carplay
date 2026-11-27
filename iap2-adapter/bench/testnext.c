/* IDEmu bench: a "next adapter" for nbtcp_iap2_adapter.so that makes the node like the factory libconn_iap2_wifi_qnx.so
 * (libmmipodtrresmgr.so.1: trresmgr_init({0x10000}), trresmgr_setup(h, {0x10000, path, cb, ctx}); cb(x, data, len,
 * ctx) -> send(data, len, ctx); messageReceived -> trresmgr_message_received). No CConfigReader: the path is fixed. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int (*send_fn)(const unsigned char*, unsigned, void*);
struct params {
    unsigned bufsize;
    char path[0x400];
    int (*cb)(void*, const unsigned char*, unsigned, void*);
    void* ctx;
};
struct tn {
    void** vptr;
    send_fn send;
    void* ctx;
    void* h;
    void* inst;
    int (*received)(void*, const unsigned char*, unsigned);
};
static int cb(void* x, const unsigned char* d, unsigned n, void* ctx)
{
    struct tn* t = ctx;
    (void)x;
    return t && t->send ? t->send(d, n, t->ctx) : -1;
}
static int t_init(struct tn* t, void* cfg, void* ctx, send_fn send)
{
    void* lib = dlopen("libmmipodtrresmgr.so.1", RTLD_NOW);
    void* (*init)(unsigned*);
    void* (*setup)(void*, struct params*);
    static unsigned opts = 0x10000;
    static struct params p;
    (void)cfg;
    t->send = send;
    t->ctx = ctx;
    if (!lib) {
        printf("testnext: %s\n", dlerror());
        return -1;
    }
    init = (void* (*)(unsigned*))dlsym(lib, "trresmgr_init");
    setup = (void* (*)(void*, struct params*))dlsym(lib, "trresmgr_setup");
    t->received = (int (*)(void*, const unsigned char*, unsigned))dlsym(lib, "trresmgr_message_received");
    t->h = init(&opts);
    p.bufsize = 0x10000;
    snprintf(p.path, sizeof(p.path), "/dev/carplayiap");
    p.cb = cb;
    p.ctx = t;
    t->inst = setup(t->h, &p);
    printf("testnext: node /dev/carplayiap %s\n", t->inst ? "made" : "NOT made");
    fflush(stdout);
    return 0;
}
static void t_nop(struct tn* t) { (void)t; }
static void t_recv(struct tn* t, const unsigned char* d, unsigned n)
{
    if (t->inst && t->received)
        t->received(t->inst, d, n);
}
static struct tn* t_dtor(struct tn* t) { return t; }
static void* vt[] = {(void*)t_init, (void*)t_nop, (void*)t_nop, (void*)t_recv, (void*)t_dtor, (void*)t_dtor};
void* iAP2WiFiAdapterGet(void)
{
    struct tn* t = calloc(1, sizeof(*t));
    t->vptr = vt;
    return t;
}
void iAP2WiFiAdapterDispose(void* p) { free(p); }
