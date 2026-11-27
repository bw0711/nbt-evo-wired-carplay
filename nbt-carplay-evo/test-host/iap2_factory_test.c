/* Mac test: cp_iap2.c as nbt_carplay --bridge-start --iap2-factory runs it, both ends faked. GPL-3.0-only
 * From nbt-carplay-evo/ (D: an empty folder for the bridge's RAM files):
 *   D=/tmp/iap2ft; mkdir -p $D/nbt-aa-native && clang -std=gnu99 -g -fsanitize=address,undefined -Wall -Wextra \
 *     -Wno-unused-function -Wno-unused-parameter -DIAP2DEV_TEST -DCP_RAM="\"$D\"" -Isrc \
 *     test-host/iap2_factory_test.c -o test-host/iap2_factory_test -lpthread && ./test-host/iap2_factory_test
 * The bridge's iap2_run runs in its own thread as on the head unit. This thread plays the iPhone on the carkit stream
 * (raw iAP2: SYN+ACK, authentication, identification, CarPlayAvailability, updates) and mm-ipod on the adapter's
 * connection (iAP2 in the adapter's frames). Checks:
 *  - the bridge's own flow is as without --iap2-factory: link, the chip's certificate and signature, its
 *    subscriptions, an identification that declares what the device role forwards, CarPlayStartSession to the head
 *    unit's receiver, its own media.state from NowPlayingUpdate;
 *  - the device role: mm-ipod's StartNowPlayingUpdates / StartCallStateUpdates / StartCommunicationsUpdates reach the
 *    iPhone unchanged, the iPhone's updates reach mm-ipod unchanged, also when longer than both sides' packets. */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include "../src/cp_iap2dev.c"
#include "../src/cp_iap2.c"

struct cp_options g_opt;
volatile int g_stop;
void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("  [bridge] "); vprintf(fmt, ap); va_end(ap); putchar('\n'); fflush(stdout); }
long long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
void sleep_ms(unsigned ms) { usleep(ms * 1000); }
void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
void put32(unsigned char* p, unsigned v) { put16(p, v >> 16); put16(p + 2, v & 0xffff); }
unsigned get16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
unsigned get32(const unsigned char* p) { return get16(p) << 16 | get16(p + 2); }
void flag_set(const char* path, const char* text)
{
    FILE* f = fopen(path, "w");
    if (f) { if (text) fputs(text, f); fclose(f); }
}
void flag_clear(const char* path) { unlink(path); }
void cp_reached(int step) { (void)step; }
int usb_gone(void) { return 0; }

/* the "chip" */
static const unsigned char CERT[600] = {0x30, 0x82, 0x02, 0x54};
int acp_open(void) { return 0; }
int acp_cert(const unsigned char** cert, unsigned* n) { *cert = CERT; *n = sizeof(CERT); return 0; }
int acp_sign(const unsigned char* c, unsigned cl, unsigned char* sig, unsigned* sl)
{
    (void)c;
    assert(cl == 20);
    memset(sig, 0x5a, 128);
    *sl = 128;
    return 0;
}
int acp_major(void) { return 2; }

/* the head unit's AirPlay receiver as heard on tap0 */
int ncm_bridge_factory(char addr[64], unsigned* port, char pk[136], char srcvers[32])
{
    strcpy(addr, "fe80::99");
    *port = 7001;
    pk[0] = 0;
    strcpy(srcvers, "320.17");
    return 1;
}
void ncm_bridge_query(void) {}
void ncm_bridge_ctrl_go(void) {}
int ctrl_connected(void) { return 0; }

/* the carkit stream */
struct stream {
    int fd;
};
int stream_write(struct stream* s, const unsigned char* b, unsigned n)
{
    while (n) {
        const ssize_t w = write(s->fd, b, n);
        if (w <= 0)
            return -1;
        b += w;
        n -= (unsigned)w;
    }
    return 0;
}
int stream_read_some(struct stream* s, unsigned char* b, unsigned n, unsigned ms)
{
    fd_set r;
    struct timeval tv = {(long)(ms / 1000), (int)(ms % 1000) * 1000};
    ssize_t got;
    FD_ZERO(&r);
    FD_SET(s->fd, &r);
    if (select(s->fd + 1, &r, 0, 0, &tv) <= 0)
        return 0;
    got = read(s->fd, b, n);
    return got > 0 ? (int)got : -1;
}

/* ---- the iPhone: raw iAP2 on the carkit stream ---- */
static int P;
static unsigned char pbuf[300000], pcsm[300000];
static unsigned pbn, pcsmn, p_seq = 0x30, p_rx;
static unsigned seen[64], nseen;   /* the ids of the bridge's messages, in order */

static void p_packet(unsigned control, unsigned session, const unsigned char* p, unsigned n)
{
    unsigned char f[3000];
    const unsigned len = 9 + (n ? n + 1 : 0);
    if (n)
        p_seq = (p_seq + 1) & 0xff;
    f[0] = 0xff; f[1] = 0x5a; put16(f + 2, len); f[4] = (unsigned char)control;
    f[5] = (unsigned char)p_seq; f[6] = (unsigned char)p_rx; f[7] = (unsigned char)session; f[8] = csum8(f, 8);
    if (n) { memcpy(f + 9, p, n); f[9 + n] = csum8(p, n); }
    assert(write(P, f, len) == (ssize_t)len);
}
static void p_msg(unsigned id, const unsigned char* params, unsigned n)   /* in packets of up to 1000 bytes */
{
    static unsigned char m[70000];
    unsigned off = 0;
    m[0] = 0x40; m[1] = 0x40; put16(m + 2, n + 6); put16(m + 4, id); memcpy(m + 6, params, n);
    while (off < n + 6) {
        const unsigned k = n + 6 - off < 1000 ? n + 6 - off : 1000;
        p_packet(0x40, 10, m + off, k);
        off += k;
    }
}
/* the next packet from the bridge (0x100: its marker); fills the payload */
static int p_read(unsigned char* pl, unsigned* pln, unsigned* session)
{
    for (;;) {
        if (pbn >= 6 && pbuf[0] == 0xff && pbuf[1] == 0x55) {
            memmove(pbuf, pbuf + 6, pbn -= 6);
            return 0x100;
        }
        if (pbn >= 9) {
            unsigned len;
            assert(pbuf[0] == 0xff && pbuf[1] == 0x5a && csum8(pbuf, 8) == pbuf[8]);
            len = get16(pbuf + 2);
            if (pbn >= len) {
                const int c = pbuf[4];
                *pln = len > 9 ? len - 10 : 0;
                if (*pln) {
                    assert(csum8(pbuf + 9, *pln) == pbuf[9 + *pln]);
                    memcpy(pl, pbuf + 9, *pln);
                    p_rx = pbuf[5];
                }
                *session = pbuf[7];
                memmove(pbuf, pbuf + len, pbn -= len);
                return c;
            }
        }
        {
            fd_set r;
            struct timeval tv = {3, 0};
            ssize_t got;
            FD_ZERO(&r); FD_SET(P, &r);
            assert(select(P + 1, &r, 0, 0, &tv) == 1);
            got = read(P, pbuf + pbn, sizeof(pbuf) - pbn);
            assert(got > 0);
            pbn += (unsigned)got;
        }
    }
}
/* waits for a control message with this id from the bridge (the others are noted in seen[]) */
static void p_expect(unsigned id, unsigned char* params, unsigned* n)
{
    static unsigned char pl[70000];
    for (;;) {
        unsigned pln, session;
        if (pcsmn >= 6) {
            const unsigned len = get16(pcsm + 2);
            assert(pcsm[0] == 0x40 && pcsm[1] == 0x40 && len >= 6);
            if (pcsmn >= len) {
                const unsigned got = get16(pcsm + 4);
                if (nseen < 64)
                    seen[nseen++] = got;
                *n = len - 6;
                memcpy(params, pcsm + 6, *n);
                memmove(pcsm, pcsm + len, pcsmn -= len);
                if (got == id)
                    return;
                continue;
            }
        }
        if (p_read(pl, &pln, &session) == 0x40 && pln) {
            assert(session == 10);
            memcpy(pcsm + pcsmn, pl, pln);
            pcsmn += pln;
            p_packet(0x40, 0, 0, 0);   /* acknowledged */
        }
    }
}
static int was_seen(unsigned id)
{
    unsigned i;
    for (i = 0; i < nseen; ++i)
        if (seen[i] == id)
            return 1;
    return 0;
}
static int list_has(const unsigned char* p, unsigned n, unsigned prm_id, unsigned id)
{
    unsigned l, i;
    const unsigned char* v = dprm_find(p, n, prm_id, &l);
    for (i = 0; v && i + 1 < l; i += 2)
        if (get16(v + i) == id)
            return 1;
    return 0;
}

/* ---- mm-ipod: iAP2 in the adapter's frames (as test-host/iap2dev_test.c) ---- */
static int A;
static unsigned a_seq = 0x10, a_rx, dev_seq_seen;
static unsigned char abuf[300000];
static unsigned an;

static void a_frame(const unsigned char* p, unsigned n)
{
    unsigned char h[3] = {1, (unsigned char)(n >> 8), (unsigned char)n};
    assert(write(A, h, 3) == 3 && write(A, p, n) == (ssize_t)n);
}
static void a_packet(unsigned control, unsigned session, const unsigned char* p, unsigned n)
{
    unsigned char f[2000];
    const unsigned len = 9 + (n ? n + 1 : 0);
    if (control != 0x40 || n)
        a_seq = (a_seq + 1) & 0xff;
    f[0] = 0xff; f[1] = 0x5a; put16(f + 2, len); f[4] = (unsigned char)control;
    f[5] = (unsigned char)a_seq; f[6] = (unsigned char)a_rx; f[7] = (unsigned char)session; f[8] = csum8(f, 8);
    if (n) { memcpy(f + 9, p, n); f[9 + n] = csum8(p, n); }
    a_frame(f, len);
}
static void a_msg(unsigned id, const unsigned char* params, unsigned n)   /* mm-ipod's packets: up to 0x400 */
{
    static unsigned char m[70000];
    unsigned off = 0;
    m[0] = 0x40; m[1] = 0x40; put16(m + 2, n + 6); put16(m + 4, id); memcpy(m + 6, params, n);
    while (off < n + 6) {
        const unsigned k = n + 6 - off < 0x3f0 ? n + 6 - off : 0x3f0;
        a_packet(0x40, 10, m + off, k);
        off += k;
    }
}
static int a_next(unsigned char* pl, unsigned* pn, unsigned* session)
{
    for (;;) {
        if (an >= 6 && abuf[0] == 0xff && abuf[1] == 0x55) {
            memmove(abuf, abuf + 6, an -= 6);
            return 0x100;
        }
        if (an >= 9 && abuf[0] == 0xff && abuf[1] == 0x5a) {
            const unsigned len = get16(abuf + 2);
            assert(csum8(abuf, 8) == abuf[8]);
            if (an >= len) {
                const int c = abuf[4];
                *pn = len > 9 ? len - 10 : 0;
                if (*pn) { assert(csum8(abuf + 9, *pn) == abuf[9 + *pn]); memcpy(pl, abuf + 9, *pn); dev_seq_seen = abuf[5]; }
                *session = abuf[7];
                memmove(abuf, abuf + len, an -= len);
                return c;
            }
        }
        {
            unsigned char h[3];
            unsigned n;
            fd_set r; struct timeval tv = {3, 0};
            FD_ZERO(&r); FD_SET(A, &r);
            assert(select(A + 1, &r, 0, 0, &tv) == 1);
            assert(read_full(A, h, 3) == 0 && h[0] == 1);
            n = (unsigned)h[1] << 8 | h[2];
            assert(read_full(A, abuf + an, n) == 0);
            an += n;
        }
    }
}
/* the next control message from the device role, reassembled */
static unsigned a_expect(unsigned char* params, unsigned* n)
{
    static unsigned char pl[70000], m[200000];
    static unsigned mn;
    for (;;) {
        unsigned pn, session;
        if (mn >= 6 && mn >= get16(m + 2)) {
            const unsigned len = get16(m + 2), id = get16(m + 4);
            *n = len - 6;
            memcpy(params, m + 6, *n);
            memmove(m, m + len, mn -= len);
            return id;
        }
        if (a_next(pl, &pn, &session) == 0x40 && pn) {
            assert(session == 10);
            memcpy(m + mn, pl, pn);
            mn += pn;
            a_rx = dev_seq_seen;
            a_packet(0x40, 0, 0, 0);
        }
    }
}

static int g_rc = 99;
static void* bridge(void* arg)
{
    static const struct iap2_carplay cp = {"fe80::1", 7000, "aa:bb:cc:dd:ee:ff", "", "950.7.1", 1};
    g_rc = iap2_run((struct stream*)arg, &cp);
    return 0;
}

int main(void)
{
    int ps[2], ms[2];
    pthread_t th;
    static struct stream st;
    static unsigned char pl[70000], params[70000], big[3000];
    unsigned pln, session, n, at, k;
    static const unsigned char detect[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    /* the iPhone's SYN+ACK: version 1, 4 outstanding, packets of 0x200 (the bridge cuts its messages), acks for each */
    static const unsigned char psyn[19] = {1, 4, 0x02, 0x00, 0x07, 0xd0, 0, 22, 30, 1, 10, 0, 2, 11, 2, 1, 12, 1, 2};
    /* mm-ipod's SYN: packets of 0x400 */
    static const unsigned char asyn[19] = {1, 4, 0x04, 0x00, 0x07, 0xd0, 0, 22, 30, 5, 10, 0, 2, 11, 2, 1, 12, 1, 2};
    mkdir(CP_RAM, 0755);
    mkdir(CP_RAM "/nbt-aa-native", 0755);
    unlink(MEDIA_STATE);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, ps) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, ms) == 0);
    P = ps[0];
    st.fd = ps[1];
    A = ms[0];
    iap2dev_test_attach(ms[1]);
    g_opt.bridge = 1;
    g_opt.bridge_start = 1;
    g_opt.iap2_factory = 1;
    assert(pthread_create(&th, 0, bridge, &st) == 0);

    /* the iPhone: the bridge's marker and SYN, our SYN+ACK */
    assert(p_read(pl, &pln, &session) == 0x100);
    assert(p_read(pl, &pln, &session) == 0x80 && pln == 19);
    p_packet(0xc0, 0, psyn, sizeof(psyn));
    /* authentication with the "chip" */
    p_msg(0xaa00, 0, 0);
    p_expect(0xaa01, params, &n);
    { unsigned l; const unsigned char* c = dprm_find(params, n, 0, &l); assert(c && l == sizeof(CERT) && !memcmp(c, CERT, l)); }
    { unsigned char ch[20]; memset(ch, 0x21, sizeof(ch)); at = dprm(params, 0, 0, ch, sizeof(ch)); p_msg(0xaa02, params, at); }
    p_expect(0xaa03, params, &n);
    { unsigned l; assert(dprm_find(params, n, 0, &l) && l == 128); }
    p_msg(0xaa05, 0, 0);
    p_expect(0x6800, params, &n);   /* the bridge's own start: power, subscriptions, the HID remote */
    assert(was_seen(0xae03) && was_seen(0x5000) && was_seen(0x4157) && was_seen(0x4154));
    /* identification: it declares what the device role passes on */
    p_msg(0x1d00, 0, 0);
    p_expect(0x1d01, params, &n);
    assert(list_has(params, n, 6, 0x5000) && list_has(params, n, 6, 0x4154) && list_has(params, n, 6, 0x4157));
    assert(list_has(params, n, 7, 0x5001) && list_has(params, n, 7, 0x4155) && list_has(params, n, 7, 0x4158));
    p_msg(0x1d02, 0, 0);
    /* CarPlay available: the session goes to the head unit's receiver */
    { unsigned char g[8]; const unsigned gn = dprm(g, 0, 0, "\x01", 1); at = dprm(params, 0, 0, g, gn); p_msg(0x4300, params, at); }
    p_expect(0x4301, params, &n);
    { unsigned l, l2; const unsigned char* g = dprm_find(params, n, 0, &l); const unsigned char* a = g ? dprm_find(g, l, 0, &l2) : 0;
      assert(a && !strncmp((const char*)a, "fe80::99", 8)); assert(dprm_find(params, n, 2, &l) && get32(dprm_find(params, n, 2, &l)) == 7001); }

    /* mm-ipod: link, authentication, identification (as with the wireless iPhone) */
    a_frame(detect, 6);
    assert(a_next(pl, &pln, &session) == 0x100);
    a_packet(0x80, 0, asyn, sizeof(asyn));
    assert(a_next(pl, &pln, &session) == 0xc0);
    a_rx = dev_seq_seen;
    a_packet(0x40, 0, 0, 0);
    assert(a_expect(params, &n) == 0xaa00);
    { unsigned char cert[700]; memset(cert, 0x30, sizeof(cert)); at = dprm(params, 0, 0, cert, sizeof(cert)); a_msg(0xaa01, params, at); }
    { unsigned l; assert(a_expect(params, &n) == 0xaa02 && dprm_find(params, n, 0, &l) && l == 20); }
    { unsigned char sig[64]; memset(sig, 0x51, sizeof(sig)); at = dprm(params, 0, 0, sig, sizeof(sig)); a_msg(0xaa03, params, at); }
    assert(a_expect(params, &n) == 0xaa05);
    assert(a_expect(params, &n) == 0x1d00);
    {
        static const unsigned short s[] = {0x5000, 0x4154, 0x4157}, t[] = {0x5001, 0x4155, 0x4158, 0x4e09};
        unsigned char l[16];
        unsigned i;
        at = dprm(params, 0, 0, "BMW RaHo", 9);
        at = dprm(params, at, 1, "F30-NBTEvo", 11);
        at = dprm(params, at, 2, "BMW", 4);
        for (i = 0; i < 3; ++i) put16(l + 2 * i, s[i]);
        at = dprm(params, at, 6, l, 6);
        for (i = 0; i < 4; ++i) put16(l + 2 * i, t[i]);
        at = dprm(params, at, 7, l, 8);
        a_msg(0x1d01, params, at);
    }
    assert(a_expect(params, &n) == 0x1d02);
    assert(a_expect(params, &n) == 0x4e09);

    /* mm-ipod subscribes with its own attributes (iap2_Wifi.cfg [nowplaying]): the iPhone gets them unchanged */
    {
        unsigned char np[200], g0[100], g1[40];
        unsigned gn0 = 0, gn1 = 0, i;
        static const unsigned char a0[] = {0, 1, 4, 6, 7, 8, 9, 10, 12, 13, 14, 26, 27, 28, 29, 30, 31}, a1[] = {0, 1, 2, 3, 7};
        for (i = 0; i < sizeof(a0); ++i) gn0 = dprm(g0, gn0, a0[i], 0, 0);
        for (i = 0; i < sizeof(a1); ++i) gn1 = dprm(g1, gn1, a1[i], 0, 0);
        at = dprm(np, 0, 0, g0, gn0);
        at = dprm(np, at, 1, g1, gn1);
        a_msg(0x5000, np, at);
        p_expect(0x5000, params, &n);
        assert(n == at && !memcmp(params, np, at));
    }
    /* the iPhone's NowPlayingUpdate: to mm-ipod unchanged, and into the bridge's own media.state */
    {
        unsigned char upd[300], g0[200], g1[40], u[4];
        unsigned gn0, gn1;
        put32(u, 200000);
        gn0 = dprm(g0, 0, 1, "Song", 5);
        gn0 = dprm(g0, gn0, 4, u, 4);
        gn0 = dprm(g0, gn0, 6, "Album", 6);
        gn0 = dprm(g0, gn0, 12, "Band", 5);
        put32(u, 5000);
        gn1 = dprm(g1, 0, 0, "\x01", 1);
        gn1 = dprm(g1, gn1, 1, u, 4);
        at = dprm(upd, 0, 0, g0, gn0);
        at = dprm(upd, at, 1, g1, gn1);
        p_msg(0x5001, upd, at);
        assert(a_expect(params, &n) == 0x5001 && n == at && !memcmp(params, upd, at));
        {
            char ms_[1000] = "";
            FILE* f;
            usleep(100000);
            f = fopen(MEDIA_STATE, "r");
            assert(f);
            ms_[fread(ms_, 1, sizeof(ms_) - 1, f)] = 0;
            fclose(f);
            assert(strstr(ms_, "title=Song\n") && strstr(ms_, "artist=Band\n") && strstr(ms_, "state=2\n"));
        }
    }
    /* longer than a packet on both sides (the iPhone's 1000, mm-ipod's 0x400): unchanged */
    for (k = 0; k < sizeof(big); ++k) big[k] = (unsigned char)(k * 13 + 7);
    put16(big, sizeof(big)); put16(big + 2, 0);   /* one group spanning it all */
    p_msg(0x5001, big, sizeof(big));
    assert(a_expect(params, &n) == 0x5001 && n == sizeof(big) && !memcmp(params, big, sizeof(big)));
    /* calls and communications: subscriptions to the iPhone, updates to mm-ipod */
    { static const unsigned char cs[] = {0, 4, 0, 0, 0, 4, 0, 1, 0, 4, 0, 4}; a_msg(0x4154, cs, sizeof(cs));
      p_expect(0x4154, params, &n); assert(n == sizeof(cs) && !memcmp(params, cs, n)); }
    { static const unsigned char up[] = {0, 5, 0, 1, 2}; p_msg(0x4155, up, sizeof(up));
      assert(a_expect(params, &n) == 0x4155 && n == sizeof(up) && !memcmp(params, up, n)); }
    { static const unsigned char cs[] = {0, 4, 0, 0, 0, 4, 0, 4}; a_msg(0x4157, cs, sizeof(cs));
      p_expect(0x4157, params, &n); assert(n == sizeof(cs) && !memcmp(params, cs, n)); }
    { static const unsigned char up[] = {0, 5, 0, 0, 3, 0, 5, 0, 4, 1}; p_msg(0x4158, up, sizeof(up));
      assert(a_expect(params, &n) == 0x4158 && n == sizeof(up) && !memcmp(params, up, n)); }

    g_stop = 1;
    pthread_join(th, 0);
    assert(g_rc == 0);
    printf("iap2 factory test: all checks passed\n");
    return 0;
}
