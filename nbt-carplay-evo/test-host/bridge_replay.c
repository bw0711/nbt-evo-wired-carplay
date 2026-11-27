/* Mac test (not for the car): the bridge's frame handling under AddressSanitizer/UBSan. Frames from the car's
 * recordings (pcap) and mangled copies of them go through frame_in (iPhone -> tap0) and the tap0 -> iPhone path
 * (mDNS log, factory receiver scan, frame log, controller announcement), also after the bridge has ended.
 * Build and run from nbt-carplay-evo/:
 *   clang -std=gnu99 -g -O1 -fsanitize=address,undefined -w -Isrc -Isrc/lwip_port \
 *         -I.cache/lwip-STABLE-2_2_1_RELEASE/src/include test-host/bridge_replay.c -o test-host/bridge_replay
 *   ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./test-host/bridge_replay <car recording.pcap>...
 *   (writes test-host/announce.bin: the controller announcement, one frame)
 * SPDX-License-Identifier: GPL-3.0-only */
#include <stdarg.h>
#include <time.h>
#include "../src/cp_ncm.c"

/* lwIP is never started in the bridge: stubs that fail loudly if reached */
static void lwip_reached(const char* f) { printf("lwIP reached: %s\n", f); abort(); }
err_t ethip6_output(struct netif* n, struct pbuf* q, const ip6_addr_t* a) { (void)n; (void)q; (void)a; lwip_reached(__func__); return 0; }
char* ip6addr_ntoa(const ip6_addr_t* a) { (void)a; lwip_reached(__func__); return 0; }
sys_mutex_t lock_tcpip_core;
struct netif* netif_add_noaddr(struct netif* n, void* st, netif_init_fn i, netif_input_fn in) { (void)n; (void)st; (void)i; (void)in; lwip_reached(__func__); return 0; }
void netif_create_ip6_linklocal_address(struct netif* n, u8_t f) { (void)n; (void)f; lwip_reached(__func__); }
void netif_ip6_addr_set_state(struct netif* n, s8_t i, u8_t s) { (void)n; (void)i; (void)s; lwip_reached(__func__); }
void netif_set_default(struct netif* n) { (void)n; lwip_reached(__func__); }
void netif_set_link_up(struct netif* n) { (void)n; lwip_reached(__func__); }
void netif_set_up(struct netif* n) { (void)n; lwip_reached(__func__); }
struct pbuf* pbuf_alloc(pbuf_layer l, u16_t len, pbuf_type t) { (void)l; (void)len; (void)t; lwip_reached(__func__); return 0; }
u16_t pbuf_copy_partial(const struct pbuf* b, void* d, u16_t l, u16_t o) { (void)b; (void)d; (void)l; (void)o; lwip_reached(__func__); return 0; }
u8_t pbuf_free(struct pbuf* p) { (void)p; lwip_reached(__func__); return 0; }
err_t pbuf_take(struct pbuf* b, const void* d, u16_t l) { (void)b; (void)d; (void)l; lwip_reached(__func__); return 0; }
void sys_mutex_lock(sys_mutex_t* m) { (void)m; lwip_reached(__func__); }
void sys_mutex_unlock(sys_mutex_t* m) { (void)m; lwip_reached(__func__); }
err_t tcpip_input(struct pbuf* p, struct netif* n) { (void)p; (void)n; lwip_reached(__func__); return 0; }

struct cp_options g_opt;
volatile int g_stop;
void cp_reached(int step) { (void)step; }
void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
long long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
void sleep_ms(unsigned ms) { usleep(ms * 1000); }
void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
void put32(unsigned char* p, unsigned v) { put16(p, v >> 16); put16(p + 2, v & 0xffff); }
unsigned get16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
unsigned get32(const unsigned char* p) { return get16(p) << 16 | get16(p + 2); }
void put16le(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
void put32le(unsigned char* p, unsigned v) { put16le(p, v & 0xffff); put16le(p + 2, v >> 16); }
unsigned get16le(const unsigned char* p) { return p[0] | (unsigned)p[1] << 8; }
unsigned get32le(const unsigned char* p) { return get16le(p) | get16le(p + 2) << 16; }

static unsigned g_blocks;
static int fake_send(void* ctx, const unsigned char* b, unsigned n) { (void)ctx; (void)b; (void)n; ++g_blocks; return 0; }

static unsigned char frames[4000][1600];
static unsigned lens[4000], nf;

static void load(const char* path)
{
    FILE* f = fopen(path, "rb");
    unsigned char h[24], r[16];
    if (!f || fread(h, 1, 24, f) != 24) { printf("no %s\n", path); exit(1); }
    while (nf < 4000 && fread(r, 1, 16, f) == 16) {
        const unsigned n = get32le(r + 8);
        if (n > 1600) { fseek(f, n, SEEK_CUR); continue; }
        if (fread(frames[nf], 1, n, f) != n) break;
        lens[nf++] = n;
    }
    fclose(f);
}

static void out_path(unsigned char* f, unsigned n)   /* as tap_rx does with a frame the head unit sends */
{
    mdns_log(f, n, "out");
    factory_scan(f, n);
    frame_brief(f, n, "out");
    if (g_ctl.on && n > 42 && has_bytes(f + 42, n - 42, "_carplay-ctrl", 13))
        ctl_announce(120);
}

int main(int argc, char** argv)
{
    static const unsigned char hu[6] = {0xf6, 0x21, 0xca, 0x7b, 0xc8, 0xcc};
    int i, p[2];
    unsigned k, it;
    for (i = 1; i < argc; ++i)
        load(argv[i]);
    printf("%u frames\n", nf);
    if (pipe(p)) return 1;
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    g_tap = p[1];
    g_bridged = 1;
    g_send = fake_send;
    memcpy(g_mac, hu, 6);
    ncm_bridge_ctrl("F4:21:CA:00:00:01", 50001);
    g_ctl.on = 1;
    for (k = 0; k < nf; ++k) {   /* as recorded */
        if (!memcmp(frames[k] + 6, hu, 6))
            out_path(frames[k], lens[k]);
        else
            frame_in(frames[k], lens[k]);
    }
    {   /* the announcement alone, for the Python check */
        unsigned char b[2000];
        ssize_t r;
        while (read(p[0], b, sizeof(b)) > 0) {}
        g_ctl.last = 0;
        ctl_announce(120);
        FILE* o = fopen(argc > 1 ? "test-host/announce.bin" : "/dev/null", "wb");
        while ((r = read(p[0], b, sizeof(b))) > 0)
            fwrite(b, 1, (size_t)r, o), fflush(o);
        fclose(o);
    }
    srand(1);
    for (it = 0; it < 200000; ++it) {   /* mangled: cut, flipped bytes, both ways */
        unsigned char f[1600];
        const unsigned s = (unsigned)rand() % nf;
        unsigned n = lens[s], j;
        char drain[4096];
        memcpy(f, frames[s], n);
        if (rand() % 2) n = (unsigned)rand() % (n + 1);
        for (j = 0; j < 1 + (unsigned)rand() % 8 && n; ++j) f[(unsigned)rand() % n] = (unsigned char)rand();
        if (rand() % 3 == 0) memcpy(f + 6, hu, n >= 12 ? 6 : 0);
        if (n >= 12 && !memcmp(f + 6, hu, 6)) out_path(f, n); else frame_in(f, n);
        while (read(p[0], drain, sizeof(drain)) > 0) {}
    }
    g_ctl.on = 0;
    close(g_tap);
    g_tap = -1;   /* the bridge has ended: frames still come */
    for (k = 0; k < nf; ++k)
        frame_in(frames[k], lens[k]);
    printf("done: %u blocks to the iPhone, receiver %s, port %u, leased %d, tap6 %s\n", g_blocks,
           g_fac.addr, g_fac.port, g_fac.leased, g_fac.tap6);
    return 0;
}
