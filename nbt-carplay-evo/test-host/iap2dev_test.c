/* Mac test: cp_iap2dev.c answers a fake mm-ipod (the accessory side of iAP2) as the iPhone would. GPL-3.0-only
 * From nbt-carplay-evo/: clang -std=gnu99 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-function \
 *   -DIAP2DEV_TEST -Isrc test-host/iap2dev_test.c -o test-host/iap2dev_test -lpthread && ./test-host/iap2dev_test
 * The fake accessory checks every header and payload checksum and the acknowledgement numbers, and walks the whole
 * device-side sequence: detect, SYN -> SYN+ACK, ACK (link up), certificate asked -> sent, challenge -> signature,
 * AuthenticationSucceeded, StartIdentification -> IdentificationInformation -> accepted, DeviceInformationUpdate,
 * StartNowPlayingUpdates (must reach the "iPhone" queue unchanged), NowPlayingUpdate from the "iPhone" (must reach the
 * accessory unchanged, also when longer than one packet), an unknown message (dropped), StartCallStateUpdates. */
#include <assert.h>
#include <stdarg.h>
#include <time.h>
#include "../src/cp_iap2dev.c"

struct cp_options g_opt;
volatile int g_stop;
void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("  [dev] "); vprintf(fmt, ap); va_end(ap); putchar('\n'); fflush(stdout); }
long long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
void put32(unsigned char* p, unsigned v) { put16(p, v >> 16); put16(p + 2, v & 0xffff); }
unsigned get16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
unsigned get32(const unsigned char* p) { return get16(p) << 16 | get16(p + 2); }

/* --- the fake mm-ipod --- */
static int A;   /* its end of the "adapter" connection */
static unsigned a_seq = 0x10, a_rx, dev_seq_seen;
static unsigned char abuf[200000];
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
    f[0] = 0xff; f[1] = 0x5a; put16(f + 2, len); f[4] = (unsigned char)control;
    if (control != 0x40 || n)
        a_seq = (a_seq + 1) & 0xff;
    f[5] = (unsigned char)a_seq; f[6] = (unsigned char)a_rx; f[7] = (unsigned char)session; f[8] = csum8(f, 8);
    if (n) { memcpy(f + 9, p, n); f[9 + n] = csum8(p, n); }
    a_frame(f, len);
}
static void a_msg(unsigned id, const unsigned char* params, unsigned n)
{
    unsigned char m[1000];
    m[0] = 0x40; m[1] = 0x40; put16(m + 2, n + 6); put16(m + 4, id); memcpy(m + 6, params, n);
    a_packet(0x40, 10, m, n + 6);
}
/* reads frames until a whole link packet (or the detect) is there; returns its control byte, fills the payload */
static int a_next(unsigned char* pl, unsigned* pn, unsigned* session)
{
    for (;;) {
        if (an >= 6 && abuf[0] == 0xff && abuf[1] == 0x55) {
            memmove(abuf, abuf + 6, an -= 6);
            return 0x100;   /* the detect */
        }
        if (an >= 9 && abuf[0] == 0xff && abuf[1] == 0x5a) {
            const unsigned len = get16(abuf + 2);
            assert(csum8(abuf, 8) == abuf[8]);
            if (an >= len) {
                const int c = abuf[4];
                *pn = len > 9 ? len - 10 : 0;
                if (*pn) { assert(csum8(abuf + 9, *pn) == abuf[9 + *pn]); memcpy(pl, abuf + 9, *pn); }
                *session = abuf[7];
                assert(((a_seq - abuf[6]) & 0xff) <= 3);   /* the device acknowledges what we sent (it may lag) */
                if (*pn) { dev_seq_seen = abuf[5]; }
                memmove(abuf, abuf + len, an -= len);
                return c;
            }
        }
        {
            unsigned char h[3];
            unsigned n;
            fd_set r; struct timeval tv = {2, 0};
            FD_ZERO(&r); FD_SET(A, &r);
            assert(select(A + 1, &r, 0, 0, &tv) == 1);
            assert(read_full(A, h, 3) == 0 && h[0] == 1);
            n = (unsigned)h[1] << 8 | h[2];
            assert(read_full(A, abuf + an, n) == 0);
            an += n;
        }
    }
}
/* the next control message from the device (its data packet acked by us) */
static unsigned a_expect(unsigned char* params, unsigned* n)
{
    unsigned char pl[70000];
    unsigned pn, session;
    for (;;) {
        const int c = a_next(pl, &pn, &session);
        if (c == 0x40 && pn) {
            assert(session == 10 && pl[0] == 0x40 && pl[1] == 0x40);
            a_rx = dev_seq_seen;
            a_packet(0x40, 0, 0, 0);   /* our ack */
            *n = get16(pl + 2) - 6;
            memcpy(params, pl + 6, *n);
            return get16(pl + 4);
        }
    }
}

static unsigned char sent[4][2000];
static unsigned sentn[4], sentid[4], nsent;
static int fake_phone_send(unsigned id, const unsigned char* p, unsigned n)
{
    sentid[nsent] = id; sentn[nsent] = n; memcpy(sent[nsent], p, n); nsent++;
    return 0;
}

int main(void)
{
    int sv[2];
    unsigned char pl[70000], params[70000];
    unsigned pn, session, n, id, at;
    static const unsigned char detect[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    /* SYN: version 1, 4 outstanding, max packet 0x0400 (forces the device to split long messages), sessions 10 control,
     * 11 external, 12 files */
    static const unsigned char syn[19] = {1, 4, 0x04, 0x00, 0x07, 0xd0, 0, 22, 30, 5, 10, 0, 2, 11, 2, 1, 12, 1, 2};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    A = sv[0];
    iap2dev_test_attach(sv[1]);
    iap2dev_start(1, "Test iPhone");
    a_frame(detect, 6);
    assert(a_next(pl, &pn, &session) == 0x100);   /* the detect answered */
    a_packet(0x80, 0, syn, sizeof(syn));
    assert(a_next(pl, &pn, &session) == 0xc0 && pn == sizeof(syn) && !memcmp(pl, syn, sizeof(syn)));
    a_rx = dev_seq_seen;              /* the SYN+ACK carries the device's first sequence number */
    a_packet(0x40, 0, 0, 0);          /* ACK: link up */
    id = a_expect(params, &n);
    assert(id == 0xaa00);
    { unsigned char cert[700]; memset(cert, 0x30, sizeof(cert)); at = dprm(params, 0, 0, cert, sizeof(cert)); a_msg(0xaa01, params, at); }
    id = a_expect(params, &n);
    { unsigned l; assert(id == 0xaa02 && dprm_find(params, n, 0, &l) && l == 20); }
    { unsigned char sig[64]; memset(sig, 0x51, sizeof(sig)); at = dprm(params, 0, 0, sig, sizeof(sig)); a_msg(0xaa03, params, at); }
    assert(a_expect(params, &n) == 0xaa05);
    assert(a_expect(params, &n) == 0x1d00);
    {   /* IdentificationInformation: name, model, maker, sends 5000 4154, takes 5001 4155 4e09 */
        static const unsigned short s[] = {0x5000, 0x4154}, t[] = {0x5001, 0x4155, 0x4e09};
        unsigned char l[16];
        unsigned i;
        at = dprm(params, 0, 0, "BMW RaHo", 9);
        at = dprm(params, at, 1, "F30-NBTEvo-0716", 16);
        at = dprm(params, at, 2, "BMW", 4);
        for (i = 0; i < 2; ++i) put16(l + 2 * i, s[i]);
        at = dprm(params, at, 6, l, 4);
        for (i = 0; i < 3; ++i) put16(l + 2 * i, t[i]);
        at = dprm(params, at, 7, l, 6);
        at = dprm(params, at, 22, "x", 1);
        a_msg(0x1d01, params, at);
    }
    assert(a_expect(params, &n) == 0x1d02);
    id = a_expect(params, &n);
    assert(id == 0x4e09 && !strcmp((const char*)params + 4, "Test iPhone"));
    /* StartNowPlayingUpdates: queued for the iPhone, unchanged */
    { static const unsigned char np[] = {0, 8, 0, 0, 0, 4, 0, 1}; a_msg(0x5000, np, sizeof(np));
      usleep(100000); iap2dev_drain(fake_phone_send);
      assert(nsent == 1 && sentid[0] == 0x5000 && sentn[0] == sizeof(np) && !memcmp(sent[0], np, sizeof(np))); }
    /* an unknown message: dropped (logged once) */
    a_msg(0xae00, 0, 0);
    usleep(100000);
    iap2dev_drain(fake_phone_send);
    assert(nsent == 1);
    /* NowPlayingUpdate from the iPhone, longer than one packet of 0x400: reaches mm-ipod unchanged */
    {
        static unsigned char big[3000];
        unsigned char got[70000];
        unsigned gn = 0, k;
        for (k = 0; k < sizeof(big); ++k) big[k] = (unsigned char)(k * 7);
        put16(big, sizeof(big)); put16(big + 2, 0);   /* one parameter group spanning the whole message */
        iap2dev_from_phone(0x5001, big, sizeof(big));
        for (;;) {   /* the device splits it into packets; collect the control session's bytes */
            const int c = a_next(pl, &pn, &session);
            if (c == 0x40 && pn) {
                memcpy(got + gn, pl, pn); gn += pn;
                a_rx = dev_seq_seen; a_packet(0x40, 0, 0, 0);
                if (gn >= 6 && gn >= get16(got + 2)) break;
            }
        }
        assert(get16(got + 4) == 0x5001 && gn == sizeof(big) + 6 && !memcmp(got + 6, big, sizeof(big)));
    }
    /* a call state update before mm-ipod subscribed: not passed */
    iap2dev_from_phone(0x4155, (const unsigned char*)"\0\x05\0\x00\x01", 5);
    usleep(100000);
    { fd_set r; struct timeval tv = {0, 200000}; FD_ZERO(&r); FD_SET(A, &r); assert(select(A + 1, &r, 0, 0, &tv) == 0); }
    /* after StartCallStateUpdates it is */
    a_msg(0x4154, 0, 0);
    usleep(100000);
    iap2dev_drain(fake_phone_send);
    assert(nsent == 2 && sentid[1] == 0x4154);
    iap2dev_from_phone(0x4155, (const unsigned char*)"\0\x05\0\x00\x01", 5);
    assert(a_expect(params, &n) == 0x4155 && n == 5);
    iap2dev_stop();
    printf("iap2dev host test: all checks passed\n");
    return 0;
}
