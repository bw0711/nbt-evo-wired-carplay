/* IDEmu bench: stands for the CarPlay plug-in: loads an iAP2 adapter (argv[1]) and calls it as libconn_carplay.so does
 * (iAP2WiFiAdapterGet, initialize(config, ctx, send), setup). send() is the "AirPlay session": it only logs. */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
typedef int (*send_fn)(const unsigned char*, unsigned, void*);
static int air_send(const unsigned char* d, unsigned n, void* ctx)
{
    (void)d;
    printf("fakeplugin: %u bytes into the \"AirPlay session\" (ctx %p)\n", n, ctx);
    fflush(stdout);
    return (int)n;
}
int main(int argc, char** argv)
{
    void* lib;
    void* (*get)(void);
    void** ad;
    void** vt;
    if (argc < 2 || !(lib = dlopen(argv[1], RTLD_NOW))) {
        printf("fakeplugin: %s\n", argc < 2 ? "usage: fakeplugin ADAPTER.so" : dlerror());
        return 1;
    }
    get = (void* (*)(void))dlsym(lib, "iAP2WiFiAdapterGet");
    ad = get ? get() : 0;
    if (!ad)
        return 1;
    vt = *(void***)ad;
    ((int (*)(void*, void*, void*, send_fn))vt[0])(ad, 0, (void*)0xc0ffee, air_send);
    ((void (*)(void*))vt[1])(ad);
    printf("fakeplugin: adapter initialized and set up\n");
    fflush(stdout);
    for (;;)
        sleep(60);
}
