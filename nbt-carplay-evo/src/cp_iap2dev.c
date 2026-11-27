/* nbt_carplay, Evo port, --iap2-factory: the iPhone's stand-in for the head unit's own iAP2 driver.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * In wireless CarPlay the head unit's mm-ipod (OSCAR package CONN_CARPLAY_WLAN_DRIVER) runs iAP2 with the iPhone
 * inside the AirPlay session and gives Connectivity "now playing", calls, ... (cluster, head-up display, media
 * screen). Over the cable the iPhone keeps iAP2 on USB, with us. wired-carplay/iap2-adapter (in Connectivity's CarPlay
 * plug-in, LibPath) hands mm-ipod's bytes to us on 127.0.0.1:47600 while we are connected; here we answer mm-ipod in
 * the iPhone's place: the device side of the iAP2 link (detect, SYN+ACK, acks), the device side of authentication
 * (asking mm-ipod for its certificate and a signature: it signs with the car's chip; we do not check), identification
 * (we accept), and then the messages that carry what the head unit shows:
 *   mm-ipod -> iPhone: StartNowPlayingUpdates 5000, StartCallStateUpdates 4154, StartCommunicationsUpdates 4157
 *   iPhone -> mm-ipod: NowPlayingUpdate 5001, CallStateUpdate 4155, CommunicationsUpdate 4158
 * Everything else from mm-ipod is logged once and dropped. Design and the head unit side:
 * wired-carplay/iap2-factory-re-2026-11-27.md.
 *
 * Threads: ours reads the adapter and answers mm-ipod at once (its link wants acks within tens of ms). The iPhone's
 * link is a TLS stream read by cp_iap2.c's loop, so what goes to the iPhone is queued here and sent from that loop
 * (iap2dev_drain). What comes from the iPhone is written to mm-ipod from that loop (iap2dev_from_phone), under a lock.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "cp.h"

#define ADAPTER_PORT 47600
#define F_IAP2 1
#define QMAX 32

static struct {
    int fd;                           /* the adapter */
    pthread_t thr;
    int thr_on;
    volatile int stop;
    pthread_mutex_t wmu;              /* writes to fd, sequence numbers */
    int auth;                         /* 1: ask mm-ipod for certificate and signature */
    /* link */
    unsigned inn;
    unsigned seq, last_rx, synced, up, maxlen;
    unsigned ctl, files, ea;          /* session ids from mm-ipod's SYN */
    unsigned csmn;
    /* state */
    int authed, identified, sub_np, sub_call, sub_comm;
    unsigned short recv_ids[160];     /* what mm-ipod's identification says it receives */
    unsigned nrecv;
    unsigned short seen[64];          /* ids from mm-ipod logged once */
    unsigned nseen;
    char name[64];
    /* to the iPhone */
    pthread_mutex_t qmu;
    struct {
        unsigned id, n;
        unsigned char* p;
    } q[QMAX];
    unsigned qn;
    unsigned long to_phone, from_phone;
} g_d = {.fd = -1, .wmu = PTHREAD_MUTEX_INITIALIZER, .qmu = PTHREAD_MUTEX_INITIALIZER};
static unsigned char g_in[70000], g_csm[70000];   /* outside g_d: zero-filled, not in the file */

static unsigned char csum8(const unsigned char* b, unsigned n)
{
    unsigned s = 0, i;
    for (i = 0; i < n; ++i)
        s += b[i];
    return (unsigned char)(0x100 - (s & 0xff));
}

/* raw bytes to mm-ipod, one adapter frame */
static int dev_raw(const unsigned char* p, unsigned n)
{
    static unsigned char f[3 + 0xffff];   /* one write per frame; callers hold wmu */
    unsigned off = 0;
    if (g_d.fd < 0 || n > 0xffff)
        return -1;
    f[0] = F_IAP2;
    f[1] = (unsigned char)(n >> 8);
    f[2] = (unsigned char)n;
    memcpy(f + 3, p, n);
    while (off < n + 3) {
        const ssize_t k = write(g_d.fd, f + off, n + 3 - off);
        if (k <= 0)
            return -1;
        off += (unsigned)k;
    }
    return 0;
}

/* one link packet; caller holds wmu */
static int dev_packet(unsigned control, unsigned seq, unsigned session, const unsigned char* p, unsigned n)
{
    static unsigned char f[70000];
    const unsigned len = 9 + (n ? n + 1 : 0);
    if (len > sizeof(f))
        return -1;
    f[0] = 0xff;
    f[1] = 0x5a;
    put16(f + 2, len);
    f[4] = (unsigned char)control;
    f[5] = (unsigned char)seq;
    f[6] = (unsigned char)g_d.last_rx;
    f[7] = (unsigned char)session;
    f[8] = csum8(f, 8);
    if (n) {
        memcpy(f + 9, p, n);
        f[9 + n] = csum8(p, n);
    }
    return dev_raw(f, len);
}

/* a control-session message to mm-ipod (any thread) */
static int dev_send(unsigned id, const unsigned char* params, unsigned n)
{
    static unsigned char m[70000];
    unsigned off = 0, chunk;
    int rc = 0;
    if (n + 6 > sizeof(m))
        return -1;
    pthread_mutex_lock(&g_d.wmu);
    if (!g_d.up) {
        pthread_mutex_unlock(&g_d.wmu);
        return -1;
    }
    chunk = g_d.maxlen > 10 ? g_d.maxlen - 10 : 1000;
    m[0] = 0x40;
    m[1] = 0x40;
    put16(m + 2, n + 6);
    put16(m + 4, id);
    memcpy(m + 6, params, n);
    while (off < n + 6 && !rc) {
        const unsigned k = n + 6 - off < chunk ? n + 6 - off : chunk;
        g_d.seq = (g_d.seq + 1) & 0xff;
        rc = dev_packet(0x40, g_d.seq, g_d.ctl, m + off, k);
        off += k;
    }
    pthread_mutex_unlock(&g_d.wmu);
    return rc;
}

static unsigned dprm(unsigned char* b, unsigned at, unsigned id, const void* data, unsigned n)
{
    put16(b + at, n + 4);
    put16(b + at + 2, id);
    if (n)
        memcpy(b + at + 4, data, n);
    return at + 4 + n;
}

static const unsigned char* dprm_find(const unsigned char* p, unsigned n, unsigned id, unsigned* len)
{
    unsigned at = 0;
    while (at + 4 <= n) {
        const unsigned l = get16(p + at), i = get16(p + at + 2);
        if (l < 4 || at + l > n)
            return 0;
        if (i == id) {
            *len = l - 4;
            return p + at + 4;
        }
        at += l;
    }
    return 0;
}

static int receives(unsigned id)
{
    unsigned i;
    for (i = 0; i < g_d.nrecv; ++i)
        if (g_d.recv_ids[i] == id)
            return 1;
    return 0;
}

static void to_phone(unsigned id, const unsigned char* p, unsigned n)
{
    pthread_mutex_lock(&g_d.qmu);
    if (g_d.qn < QMAX && (g_d.q[g_d.qn].p = malloc(n ? n : 1)) != 0) {
        g_d.q[g_d.qn].id = id;
        g_d.q[g_d.qn].n = n;
        memcpy(g_d.q[g_d.qn].p, p, n);
        g_d.qn++;
    } else
        say("iap2dev: queue to the iPhone full, %04x dropped", id);
    pthread_mutex_unlock(&g_d.qmu);
}

static void str_param(const unsigned char* p, unsigned n, unsigned id, char* out, unsigned size)
{
    unsigned l;
    const unsigned char* v = dprm_find(p, n, id, &l);
    out[0] = 0;
    if (v && l)
        snprintf(out, size, "%.*s", (int)(l < size ? l : size - 1), (const char*)v);
}

/* IdentificationInformation from mm-ipod: who it says it is and what it sends and takes */
static void identification(const unsigned char* p, unsigned n)
{
    char name[64], model[64], maker[64];
    unsigned l, i;
    const unsigned char* v;
    char list[700];
    str_param(p, n, 0, name, sizeof(name));
    str_param(p, n, 1, model, sizeof(model));
    str_param(p, n, 2, maker, sizeof(maker));
    say("iap2dev: mm-ipod is \"%s\" (%s, %s)", name, model, maker);
    for (i = 6; i <= 7; ++i) {
        unsigned k, at = 0;
        list[0] = 0;
        if ((v = dprm_find(p, n, i, &l)) != 0)
            for (k = 0; k + 1 < l && at + 6 < sizeof(list); k += 2) {
                at += (unsigned)snprintf(list + at, sizeof(list) - at, " %04x", get16(v + k));
                if (i == 7 && g_d.nrecv < sizeof(g_d.recv_ids) / sizeof(g_d.recv_ids[0]))
                    g_d.recv_ids[g_d.nrecv++] = (unsigned short)get16(v + k);
            }
        say("iap2dev: mm-ipod %s:%s", i == 6 ? "sends" : "takes", list);
    }
    {   /* the components, by parameter id, for the record */
        unsigned at = 0, pos = 0;
        list[0] = 0;
        while (at + 4 <= n && pos + 8 < sizeof(list)) {
            const unsigned ll = get16(p + at);
            if (ll < 4)
                break;
            pos += (unsigned)snprintf(list + pos, sizeof(list) - pos, " %u", get16(p + at + 2));
            at += ll;
        }
        say("iap2dev: identification parameters:%s", list);
    }
}

static void log_once(unsigned id, unsigned n)
{
    unsigned i;
    for (i = 0; i < g_d.nseen; ++i)
        if (g_d.seen[i] == id)
            return;
    if (g_d.nseen < sizeof(g_d.seen) / sizeof(g_d.seen[0]))
        g_d.seen[g_d.nseen++] = (unsigned short)id;
    say("iap2dev: mm-ipod sent %04x (%u bytes), not passed on", id, n + 6);
}

static void after_auth(void)
{
    say("iap2dev: identification asked");
    dev_send(0x1d00, 0, 0);
}

static void dev_message(unsigned id, const unsigned char* p, unsigned n)
{
    unsigned char out[128];
    unsigned at;
    switch (id) {
    case 0xaa01: {   /* AuthenticationCertificate: answer with a challenge */
        /* 20 bytes, as the iPhone sends to the car's ACP 2.0B over Bluetooth (head unit log 27.11): with 32 the chip
         * refused to sign (status 0x80, error code 04, mm-ipod "Authenticated verification error"), mm-ipod never
         * answered, and Connectivity ended the wired CarPlay session 10 s after it started */
        unsigned l, i;
        unsigned char ch[20];
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, ch, sizeof(ch)) != (ssize_t)sizeof(ch))
            for (i = 0; i < sizeof(ch); ++i)
                ch[i] = (unsigned char)(rand() >> 7);
        if (fd >= 0)
            close(fd);
        say("iap2dev: mm-ipod's certificate, %u bytes", dprm_find(p, n, 0, &l) ? l : 0);
        at = dprm(out, 0, 0, ch, sizeof(ch));
        dev_send(0xaa02, out, at);
        break;
    }
    case 0xaa03: {   /* AuthenticationResponse: we do not check it */
        unsigned l;
        say("iap2dev: mm-ipod signed (%u bytes): authentication succeeded", dprm_find(p, n, 0, &l) ? l : 0);
        g_d.authed = 1;
        dev_send(0xaa05, 0, 0);
        after_auth();
        break;
    }
    case 0x1d01:
        identification(p, n);
        g_d.identified = 1;
        dev_send(0x1d02, 0, 0);
        say("iap2dev: identification accepted");
        if (receives(0x4e09) && g_d.name[0]) {   /* DeviceInformationUpdate: the iPhone's name */
            at = dprm(out, 0, 0, g_d.name, (unsigned)strlen(g_d.name) + 1);
            dev_send(0x4e09, out, at);
        }
        break;
    case 0x5000:
        g_d.sub_np = 1;
        say("iap2dev: mm-ipod wants now-playing updates");
        to_phone(id, p, n);
        break;
    case 0x5002:
        g_d.sub_np = 0;
        break;
    case 0x4154:
        g_d.sub_call = 1;
        say("iap2dev: mm-ipod wants call state updates");
        to_phone(id, p, n);
        break;
    case 0x4156:
        g_d.sub_call = 0;
        break;
    case 0x4157:
        g_d.sub_comm = 1;
        say("iap2dev: mm-ipod wants communications updates");
        to_phone(id, p, n);
        break;
    case 0x4159:
        g_d.sub_comm = 0;
        break;
    default:
        log_once(id, n);
        break;
    }
}

static void dev_csm(const unsigned char* b, unsigned n)
{
    if (n > sizeof(g_csm) - g_d.csmn)
        g_d.csmn = 0;
    memcpy(g_csm + g_d.csmn, b, n);
    g_d.csmn += n;
    for (;;) {
        unsigned skip = 0, len;
        while (skip + 1 < g_d.csmn && !(g_csm[skip] == 0x40 && g_csm[skip + 1] == 0x40))
            ++skip;
        if (skip) {
            memmove(g_csm, g_csm + skip, g_d.csmn - skip);
            g_d.csmn -= skip;
        }
        if (g_d.csmn < 6)
            return;
        len = get16(g_csm + 2);
        if (len < 6) {
            memmove(g_csm, g_csm + 1, --g_d.csmn);
            continue;
        }
        if (g_d.csmn < len)
            return;
        dev_message(get16(g_csm + 4), g_csm + 6, len - 6);
        memmove(g_csm, g_csm + len, g_d.csmn - len);
        g_d.csmn -= len;
    }
}

/* mm-ipod's SYN: link parameters and sessions; answered with SYN+ACK carrying the same parameters */
static void dev_syn(unsigned seq, const unsigned char* pl, unsigned pn)
{
    unsigned at;
    if (pn < 10) {
        say("iap2dev: short SYN (%u bytes)", pn);
        return;
    }
    g_d.maxlen = get16(pl + 2);
    g_d.ctl = g_d.files = g_d.ea = 0;
    for (at = 10; at + 3 <= pn; at += 3) {
        if (pl[at + 1] == 0)
            g_d.ctl = pl[at];
        else if (pl[at + 1] == 1)
            g_d.files = pl[at];
        else if (pl[at + 1] == 2)
            g_d.ea = pl[at];
    }
    say("iap2dev: mm-ipod's SYN: max packet %u, sessions control %u, files %u, external %u", g_d.maxlen, g_d.ctl,
        g_d.files, g_d.ea);
    pthread_mutex_lock(&g_d.wmu);
    g_d.last_rx = seq;
    g_d.synced = 1;
    g_d.up = 0;
    g_d.seq = 0x2b;
    dev_packet(0xc0, g_d.seq, 0, pl, pn);
    pthread_mutex_unlock(&g_d.wmu);
}

static void dev_link(void)
{
    static const unsigned char detect[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    for (;;) {
        unsigned skip = 0, len, control, seq, session, pn;
        const unsigned char* pl;
        while (skip < g_d.inn && !(g_in[skip] == 0xff && skip + 1 < g_d.inn && (g_in[skip + 1] == 0x5a ||
                                                                               g_in[skip + 1] == 0x55)))
            ++skip;
        if (skip) {
            memmove(g_in, g_in + skip, g_d.inn - skip);
            g_d.inn -= skip;
        }
        if (g_d.inn >= 2 && g_in[1] == 0x55) {   /* mm-ipod's detect: answered with the same */
            if (g_d.inn < 6)
                return;
            if (!memcmp(g_in, detect, 6)) {
                pthread_mutex_lock(&g_d.wmu);
                dev_raw(detect, 6);
                pthread_mutex_unlock(&g_d.wmu);
                if (!g_d.synced)
                    say("iap2dev: mm-ipod's iAP2 detect answered");
                memmove(g_in, g_in + 6, g_d.inn - 6);
                g_d.inn -= 6;
            } else {
                memmove(g_in, g_in + 1, --g_d.inn);
            }
            continue;
        }
        if (g_d.inn < 9)
            return;
        if (csum8(g_in, 8) != g_in[8]) {
            memmove(g_in, g_in + 1, --g_d.inn);
            continue;
        }
        len = get16(g_in + 2);
        if (len < 9) {
            memmove(g_in, g_in + 1, --g_d.inn);
            continue;
        }
        if (g_d.inn < len)
            return;
        control = g_in[4];
        seq = g_in[5];
        session = g_in[7];
        pl = g_in + 9;
        pn = len > 9 ? len - 10 : 0;
        if (len > 9 && csum8(pl, pn) != pl[pn])
            say("iap2dev: packet %u with a bad payload checksum (ignored)", seq);
        else if (control & 0x10) {
            say("iap2dev: mm-ipod reset the link");
            g_d.synced = g_d.up = g_d.authed = g_d.identified = 0;
            g_d.sub_np = g_d.sub_call = g_d.sub_comm = 0;
        } else if (control & 0x80)
            dev_syn(seq, pl, pn);
        else if (control & 0x40) {
            if (!g_d.up && g_d.synced) {
                pthread_mutex_lock(&g_d.wmu);
                g_d.up = 1;
                pthread_mutex_unlock(&g_d.wmu);
                say("iap2dev: link to mm-ipod up");
                if (g_d.auth) {
                    say("iap2dev: certificate asked");
                    dev_send(0xaa00, 0, 0);
                } else
                    after_auth();
            }
            if (pn) {
                if (seq == ((g_d.last_rx + 1) & 0xff)) {
                    g_d.last_rx = seq;
                    if (session == g_d.ctl)
                        dev_csm(pl, pn);
                }
                pthread_mutex_lock(&g_d.wmu);   /* ack each data packet at once */
                dev_packet(0x40, g_d.seq, 0, 0, 0);
                pthread_mutex_unlock(&g_d.wmu);
            }
        }
        memmove(g_in, g_in + len, g_d.inn - len);
        g_d.inn -= len;
    }
}

static int read_full(int fd, unsigned char* p, unsigned n)
{
    while (n) {
        const ssize_t k = read(fd, p, n);
        if (k <= 0)
            return -1;
        p += k;
        n -= (unsigned)k;
    }
    return 0;
}

static int connect_adapter(void)
{
    struct sockaddr_in sa;
    const int one = 1;
    struct timeval st = {1, 0};
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ADAPTER_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa))) {
        close(s);
        return -1;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
    return s;
}

static void* dev_thread(void* arg)
{
    long long last_try = 0;
    int told = 0;
    (void)arg;
    while (!g_d.stop) {
        fd_set r;
        struct timeval tv = {0, 200000};
        unsigned char h[3];
        if (g_d.fd < 0) {
            if (ms_now() - last_try < 2000) {
                usleep(100000);
                continue;
            }
            last_try = ms_now();
            pthread_mutex_lock(&g_d.wmu);
            g_d.fd = connect_adapter();
            pthread_mutex_unlock(&g_d.wmu);
            if (g_d.fd < 0) {
                if (!told++)
                    say("iap2dev: no adapter on 127.0.0.1:%d yet (nbtcp_iap2_adapter in carplay.ini LibPath?)",
                        ADAPTER_PORT);
                continue;
            }
            say("iap2dev: connected to the head unit's iAP2 adapter; waiting for mm-ipod");
        }
        FD_ZERO(&r);
        FD_SET(g_d.fd, &r);
        if (select(g_d.fd + 1, &r, 0, 0, &tv) <= 0)
            continue;
        if (read_full(g_d.fd, h, 3)) {
            say("iap2dev: the adapter closed the connection");
            pthread_mutex_lock(&g_d.wmu);
            close(g_d.fd);
            g_d.fd = -1;
            g_d.up = g_d.synced = 0;
            pthread_mutex_unlock(&g_d.wmu);
            continue;
        }
        {
            const unsigned n = (unsigned)h[1] << 8 | h[2];
            if (n > sizeof(g_in) - g_d.inn)
                g_d.inn = 0;
            if (read_full(g_d.fd, g_in + g_d.inn, n)) {
                say("iap2dev: the adapter connection broke in a frame");
                pthread_mutex_lock(&g_d.wmu);
                close(g_d.fd);
                g_d.fd = -1;
                g_d.up = g_d.synced = 0;
                pthread_mutex_unlock(&g_d.wmu);
                continue;
            }
            if (h[0] == F_IAP2) {
                g_d.inn += n;
                dev_link();
            }
        }
    }
    return 0;
}

/* --- for cp_iap2.c --- */

void iap2dev_start(int auth, const char* phone_name)
{
    g_d.auth = auth;
    snprintf(g_d.name, sizeof(g_d.name), "%s", phone_name && *phone_name ? phone_name : "iPhone");
    g_d.stop = 0;
    if (pthread_create(&g_d.thr, 0, dev_thread, 0) == 0)
        g_d.thr_on = 1;
    say("iap2dev: on (authentication of mm-ipod %s)", auth ? "asked" : "skipped");
}

void iap2dev_stop(void)
{
    unsigned i;
    g_d.stop = 1;
    if (g_d.thr_on)
        pthread_join(g_d.thr, 0);
    g_d.thr_on = 0;
    pthread_mutex_lock(&g_d.wmu);
    if (g_d.fd >= 0)
        close(g_d.fd);
    g_d.fd = -1;
    pthread_mutex_unlock(&g_d.wmu);
    pthread_mutex_lock(&g_d.qmu);
    for (i = 0; i < g_d.qn; ++i)
        free(g_d.q[i].p);
    g_d.qn = 0;
    pthread_mutex_unlock(&g_d.qmu);
    say("iap2dev: off (%lu messages to the iPhone, %lu to mm-ipod)", g_d.to_phone, g_d.from_phone);
}

/* from cp_iap2.c's loop: what mm-ipod asked of the iPhone */
void iap2dev_drain(int (*send)(unsigned id, const unsigned char* p, unsigned n))
{
    for (;;) {
        unsigned id, n;
        unsigned char* p;
        pthread_mutex_lock(&g_d.qmu);
        if (!g_d.qn) {
            pthread_mutex_unlock(&g_d.qmu);
            return;
        }
        id = g_d.q[0].id;
        n = g_d.q[0].n;
        p = g_d.q[0].p;
        memmove(g_d.q, g_d.q + 1, (g_d.qn - 1) * sizeof(g_d.q[0]));
        g_d.qn--;
        pthread_mutex_unlock(&g_d.qmu);
        send(id, p, n);
        g_d.to_phone++;
        free(p);
    }
}

/* from cp_iap2.c's on_message: the iPhone's updates mm-ipod subscribed to */
void iap2dev_from_phone(unsigned id, const unsigned char* p, unsigned n)
{
    int want = 0;
    if (!g_d.identified)
        return;
    switch (id) {
    case 0x5001:
        want = g_d.sub_np;
        break;
    case 0x4155:
        want = g_d.sub_call;
        break;
    case 0x4158:
        want = g_d.sub_comm;
        break;
    default:
        break;
    }
    if (want && !dev_send(id, p, n))
        g_d.from_phone++;
}

#ifdef IAP2DEV_TEST
/* the host test drives the link without the adapter */
void iap2dev_test_attach(int fd) { g_d.fd = fd; }
#endif
