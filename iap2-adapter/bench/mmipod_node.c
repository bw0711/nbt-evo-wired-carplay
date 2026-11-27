/* IDEmu bench: mm-ipod's side of iAP2 on the factory adapter's node, as mm-ipod's 'bt' transport uses it (open, read,
 * write): detect, SYN, ACK, then (iap2dev_run noauth: no authentication) the identification, StartNowPlayingUpdates,
 * and it waits for the NowPlayingUpdate the device role makes up. Without a bridge its bytes go to the plug-in's
 * "AirPlay session" instead and nothing answers: then it reports that after the detect (the wireless path).
 * Run: mmipod_node [node, default /dev/carplayiap] */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int N;
static unsigned char buf[70000];
static unsigned bn, seq = 0x10, rx;

static void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static unsigned get16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned char csum(const unsigned char* b, unsigned n)
{
    unsigned s = 0, i;
    for (i = 0; i < n; ++i)
        s += b[i];
    return (unsigned char)(0x100 - (s & 0xff));
}
static void fail(const char* why)
{
    printf("mmipod_node: FAILED: %s\n", why);
    exit(1);
}
static void on_alarm(int s)
{
    (void)s;
    printf("mmipod_node: no answer within 10 s\n");
    exit(2);
}
static void out(const unsigned char* p, unsigned n)
{
    if (write(N, p, n) != (ssize_t)n)
        fail("write to the node");
}
static void packet(unsigned control, unsigned session, const unsigned char* p, unsigned n)
{
    unsigned char f[2000];
    const unsigned len = 9 + (n ? n + 1 : 0);
    if (control != 0x40 || n)
        seq = (seq + 1) & 0xff;
    f[0] = 0xff; f[1] = 0x5a; put16(f + 2, len); f[4] = (unsigned char)control;
    f[5] = (unsigned char)seq; f[6] = (unsigned char)rx; f[7] = (unsigned char)session; f[8] = csum(f, 8);
    if (n) { memcpy(f + 9, p, n); f[9 + n] = csum(p, n); }
    out(f, len);
}
static unsigned prm(unsigned char* b, unsigned at, unsigned id, const void* d, unsigned n)
{
    put16(b + at, n + 4);
    put16(b + at + 2, id);
    if (n)
        memcpy(b + at + 4, d, n);
    return at + 4 + n;
}
static void msg(unsigned id, const unsigned char* params, unsigned n)
{
    unsigned char m[1000];
    m[0] = 0x40; m[1] = 0x40; put16(m + 2, n + 6); put16(m + 4, id); memcpy(m + 6, params, n);
    packet(0x40, 10, m, n + 6);
}
/* the next packet (0x100: the detect); its payload into pl */
static int next(unsigned char* pl, unsigned* pn)
{
    for (;;) {
        if (bn >= 6 && buf[0] == 0xff && buf[1] == 0x55) {
            memmove(buf, buf + 6, bn -= 6);
            return 0x100;
        }
        if (bn >= 9 && buf[0] == 0xff && buf[1] == 0x5a) {
            const unsigned len = get16(buf + 2);
            if (csum(buf, 8) != buf[8])
                fail("header checksum");
            if (bn >= len) {
                const int c = buf[4];
                *pn = len > 9 ? len - 10 : 0;
                if (*pn) {
                    if (csum(buf + 9, *pn) != buf[9 + *pn])
                        fail("payload checksum");
                    memcpy(pl, buf + 9, *pn);
                    rx = buf[5];
                }
                memmove(buf, buf + len, bn -= len);
                return c;
            }
        } else if (bn >= 2)
            fail("bytes that are no iAP2 packet");
        {
            const ssize_t r = read(N, buf + bn, sizeof(buf) - bn);
            if (r <= 0)
                fail("read from the node");
            bn += (unsigned)r;
        }
    }
}
static unsigned expect(unsigned char* params, unsigned* n)   /* the next control message (one packet each here) */
{
    unsigned char pl[70000];
    unsigned pn;
    for (;;) {
        if (next(pl, &pn) == 0x40 && pn) {
            packet(0x40, 0, 0, 0);
            if (pn < 6 || pl[0] != 0x40 || pl[1] != 0x40)
                fail("a control message cut over packets (not expected here)");
            *n = get16(pl + 2) - 6;
            memcpy(params, pl + 6, *n);
            return get16(pl + 4);
        }
    }
}
static void text(const unsigned char* p, unsigned n, unsigned id, char* o)
{
    unsigned at = 0;
    o[0] = 0;
    while (at + 4 <= n) {
        const unsigned l = get16(p + at);
        if (l < 4 || at + l > n)
            return;
        if (get16(p + at + 2) == id) {
            snprintf(o, 64, "%.*s", (int)(l - 4), (const char*)p + at + 4);
            return;
        }
        at += l;
    }
}

int main(int argc, char** argv)
{
    static const unsigned char detect[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    static const unsigned char syn[19] = {1, 4, 0x04, 0x00, 0x07, 0xd0, 0, 22, 30, 5, 10, 0, 2, 11, 2, 1, 12, 1, 2};
    unsigned char pl[70000], params[70000];
    unsigned pn, n, at, id;
    const char* path = argc > 1 ? argv[1] : "/dev/carplayiap";
    signal(SIGALRM, on_alarm);
    alarm(10);
    N = open(path, O_RDWR);
    if (N < 0)
        fail("open the node");
    out(detect, 6);
    if (next(pl, &pn) != 0x100)
        fail("no detect back");
    printf("mmipod_node: detect answered\n");
    packet(0x80, 0, syn, sizeof(syn));
    if (next(pl, &pn) != 0xc0)
        fail("no SYN+ACK");
    packet(0x40, 0, 0, 0);
    printf("mmipod_node: link up\n");
    if ((id = expect(params, &n)) != 0x1d00)
        fail("no StartIdentification");
    {
        static const unsigned char s[] = {0x50, 0x00, 0x41, 0x54, 0x41, 0x57}, t[] = {0x50, 0x01, 0x41, 0x55, 0x41, 0x58, 0x4e, 0x09};
        at = prm(params, 0, 0, "BMW RaHo", 9);
        at = prm(params, at, 1, "bench", 6);
        at = prm(params, at, 2, "BMW", 4);
        at = prm(params, at, 6, s, sizeof(s));
        at = prm(params, at, 7, t, sizeof(t));
        msg(0x1d01, params, at);
    }
    if ((id = expect(params, &n)) != 0x1d02)
        fail("identification not accepted");
    printf("mmipod_node: identification accepted\n");
    if ((id = expect(params, &n)) != 0x4e09)
        fail("no DeviceName");
    printf("mmipod_node: device name \"%.*s\"\n", n > 4 ? (int)(n - 4) : 0, (const char*)params + 4);
    {
        unsigned char g[40], np[60];
        unsigned gn = prm(g, 0, 1, 0, 0);
        gn = prm(g, gn, 12, 0, 0);
        at = prm(np, 0, 0, g, gn);
        msg(0x5000, np, at);
    }
    printf("mmipod_node: StartNowPlayingUpdates sent\n");
    for (;;) {
        char title[64], artist[64];
        if (expect(params, &n) == 0x5001) {
            const unsigned l = get16(params);   /* group 0: the song */
            text(params + 4, l - 4, 1, title);
            text(params + 4, l - 4, 12, artist);
            printf("mmipod_node: NowPlayingUpdate: \"%s\" / \"%s\"\n", title, artist);
            return 0;
        }
    }
}
