/* nbt_carplay: the iPhone's CDC NCM network link (step a): NTB16 blocks <-> Ethernet frames <-> lwIP.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * In the CarPlay USB configuration the iPhone offers a CDC NCM function. CarPlay runs over IPv6 link-local
 * addresses on it: ours is made from the MAC address (the NCM descriptor's iMACAddress, as xcertplay uses it), and
 * we tell the iPhone that address in CarPlayStartSession. Frames travel in NTB16 transfer blocks (one Ethernet frame
 * per block from us, as xcertplay / LIVI; a block of a multiple of 512 bytes gets one pad byte so it ends with a
 * short USB packet). Incoming blocks are cut into frames and handed to lwIP. --pcap FILE writes every frame both
 * ways (Wireshark).
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/mld6.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "netif/ethernet.h"
#include "cp.h"

#define NTH16 0x484d434eu   /* "NCMH" */
#define NDP16 0x304d434eu   /* "NCM0" */

static struct netif g_netif;
static unsigned char g_mac[6];
static ncm_send_fn g_send;
static void* g_ctx;
static pthread_mutex_t g_tx = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_seq;
static char g_address[64];
static unsigned char g_rx[65536 + 2048];
static unsigned g_rxn;
static unsigned long long g_frames_in, g_frames_out;
static FILE* g_pcap;
static pthread_mutex_t g_pcap_m = PTHREAD_MUTEX_INITIALIZER;
static int g_tap = -1;   /* Evo port: --bridge */
static volatile int g_tap_stop;
static volatile int g_bridged;   /* set for good at the bridge's start: frames never go to lwIP (not started then) */
/* Evo port, --bridge-start: the head unit's own AirPlay receiver as it announces itself on tap0, and the iPhone's
 * link addresses (for a Bonjour query in its name) */
static struct {
    pthread_mutex_t m;
    int have_txt, have_port;
    unsigned port;
    char addr[64], pk[136], srcvers[32];
    unsigned char peer_mac[6], peer_ip6[16];
    int have_peer, leased;
    char tap6[64];   /* tap0's IPv6 link-local, from its own frames */
    int have_mac;    /* peer_mac known (from any frame of the iPhone) */
} g_fac = {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, "", "", "", {0}, {0}, 0, 0, "", 0};

/* IPv4 on tap0 (the head unit's mdnsd registers IPv4 interfaces only; car 2026-11-26) and the address our DHCP
 * answer gives the iPhone, which asks for one over the cable. 172.16.226/24 is free on the car (uap0 = .224,
 * wfd0 = .222, lo1 = .223). */
static const unsigned char TAP_IP4[4] = {172, 16, 226, 1}, PEER_IP4[4] = {172, 16, 226, 100};

/* an IPv6 address as text, lower case, the longest run of zero groups as "::" (lwIP's headers and the system's
 * inet_ntop do not mix here) */
static void ip6_text(const unsigned char* a, char* out, unsigned outn)
{
    unsigned g[8], i, best = 8, bestn = 0, o = 0;
    for (i = 0; i < 8; ++i)
        g[i] = get16(a + 2 * i);
    for (i = 0; i < 8;) {
        unsigned k = i;
        while (k < 8 && !g[k])
            ++k;
        if (k - i > bestn && k - i >= 2)
            best = i, bestn = k - i;
        i = k > i ? k : i + 1;
    }
    out[0] = 0;
    for (i = 0; i < 8 && o + 6 < outn;) {
        if (i == best) {
            o += (unsigned)snprintf(out + o, outn - o, "::");
            i += bestn;
            continue;
        }
        o += (unsigned)snprintf(out + o, outn - o, "%s%x", o && out[o - 1] != ':' ? ":" : "", g[i]);
        ++i;
    }
}

void ncm_pcap(const char* path)
{
    unsigned char h[24];
    g_pcap = fopen(path, "wb");
    if (!g_pcap) {
        say("ncm: %s could not be written", path);
        return;
    }
    put32le(h, 0xa1b2c3d4u);
    put16le(h + 4, 2);
    put16le(h + 6, 4);
    put32le(h + 8, 0);
    put32le(h + 12, 0);
    put32le(h + 16, 65535);
    put32le(h + 20, 1);   /* Ethernet */
    fwrite(h, 1, sizeof(h), g_pcap);
    fflush(g_pcap);
}

static void pcap_frame(const unsigned char* f, unsigned n)
{
    unsigned char h[16];
    const long long t = ms_now();
    if (!g_pcap)
        return;
    put32le(h, (unsigned)(t / 1000));
    put32le(h + 4, (unsigned)(t % 1000) * 1000);
    put32le(h + 8, n);
    put32le(h + 12, n);
    pthread_mutex_lock(&g_pcap_m);
    fwrite(h, 1, sizeof(h), g_pcap);
    fwrite(f, 1, n, g_pcap);
    fflush(g_pcap);
    pthread_mutex_unlock(&g_pcap_m);
}

/* one frame (already at block + 28) in one NTB16 block to the iPhone */
static int send_block(unsigned char* block, unsigned n)
{
    unsigned len;
    int r;
    pcap_frame(block + 28, n);
    len = 28 + n;
    pthread_mutex_lock(&g_tx);
    put32le(block, NTH16);
    put16le(block + 4, 12);
    put16le(block + 6, g_seq++ & 0xffff);
    put16le(block + 8, len);
    put16le(block + 10, 12);
    put32le(block + 12, NDP16);
    put16le(block + 16, 16);
    put16le(block + 18, 0);
    put16le(block + 20, 28);
    put16le(block + 22, n);
    put16le(block + 24, 0);
    put16le(block + 26, 0);
    if (len % 512 == 0)
        block[len++] = 0;
    r = g_send(g_ctx, block, len);
    ++g_frames_out;
    pthread_mutex_unlock(&g_tx);
    if (g_frames_out == 1)
        say("ncm: first frame out (%u bytes)", n);
    return r;
}

/* lwIP -> USB */
static err_t link_output(struct netif* netif, struct pbuf* p)
{
    unsigned char block[28 + 1600];
    const unsigned n = p->tot_len;
    (void)netif;
    if (n > 1600)
        return ERR_BUF;
    pbuf_copy_partial(p, block + 28, (u16_t)n, 0);
    return send_block(block, n) ? ERR_IF : ERR_OK;
}

/* ---- Evo port: --bridge (NCM <-> the head unit's tap0) ---- */

/* the Bonjour services named in an mDNS frame (IPv4 or IPv6, UDP 5353): logged, so the bridge shows whether the
 * iPhone offers _carplay-ctrl over the cable and the head unit _airplay */
static void mdns_log(const unsigned char* f, unsigned n, const char* way)
{
    static unsigned logged;
    static const char* const names[] = {"_carplay-ctrl", "_airplay", "_raop", "_carplay", "_apple-mobdev2", "_rdlink"};
    unsigned ip = 14, udp, i;
    const unsigned char* d;
    char found[160];
    if (logged >= 200 || n < 14)
        return;
    if (get16(f + 12) == 0x86dd && n >= 14 + 40 + 8 && f[14 + 6] == 17)
        udp = ip + 40;
    else if (get16(f + 12) == 0x0800 && n >= 14 + 20 + 8 && f[14 + 9] == 17)
        udp = ip + (f[14] & 0x0f) * 4;
    else
        return;
    if (get16(f + udp) != 5353 && get16(f + udp + 2) != 5353)
        return;
    d = f + udp + 8;
    found[0] = 0;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const unsigned k = (unsigned)strlen(names[i]);
        unsigned j;
        for (j = 0; d + j + k <= f + n; ++j)
            if (!memcmp(d + j, names[i], k)) {
                strncat(found, " ", sizeof(found) - strlen(found) - 1);
                strncat(found, names[i], sizeof(found) - strlen(found) - 1);
                break;
            }
    }
    if (!found[0])
        return;
    ++logged;
    say("bridge: mDNS %s (%s, %u bytes):%s", way, d + 2 < f + n && (d[2] & 0x80) ? "answer" : "query", n, found);
}

/* one DNS name at off (with compression) into out as dotted text; *next = the byte after it where it started */
static int dns_name(const unsigned char* d, unsigned n, unsigned off, char* out, unsigned outn, unsigned* next)
{
    unsigned o = 0, at = off, jumps = 0;
    int jumped = 0;
    out[0] = 0;
    while (at < n) {
        const unsigned l = d[at];
        if (l == 0) {
            if (!jumped)
                *next = at + 1;
            return 0;
        }
        if ((l & 0xc0) == 0xc0) {
            if (at + 1 >= n || ++jumps > 16)
                return -1;
            if (!jumped)
                *next = at + 2;
            jumped = 1;
            at = ((l & 0x3f) << 8) | d[at + 1];
            continue;
        }
        if ((l & 0xc0) || at + 1 + l > n)
            return -1;
        if (o + l + 2 < outn) {
            if (o)
                out[o++] = '.';
            memcpy(out + o, d + at + 1, l);
            o += l;
            out[o] = 0;
        }
        at += 1 + l;
    }
    return -1;
}

/* an mDNS answer from the head unit (going out to the iPhone): the SRV port and TXT keys of its _airplay._tcp */
static void factory_scan(const unsigned char* f, unsigned n)
{
    unsigned udp;
    const unsigned char* d;
    unsigned dn, at, i, qd, rr;
    char name[256], src[64];
    if (n < 14 + 20)
        return;
    if (get16(f + 12) == 0x86dd && n >= 14 + 40 && f[14 + 6] == 17) {
        udp = 14 + 40;
        ip6_text(f + 14 + 8, src, sizeof(src));
    } else if (get16(f + 12) == 0x0800 && f[14 + 9] == 17) {
        udp = 14 + (f[14] & 0x0f) * 4u;
        snprintf(src, sizeof(src), "%u.%u.%u.%u", f[26], f[27], f[28], f[29]);
    } else
        return;
    if (n < udp + 8 + 12 || get16(f + udp) != 5353)
        return;
    d = f + udp + 8;
    dn = n - udp - 8;
    if (!(d[2] & 0x80))
        return;
    qd = get16(d + 4);
    rr = (unsigned)get16(d + 6) + get16(d + 8) + get16(d + 10);
    at = 12;
    for (i = 0; i < qd; ++i) {
        if (dns_name(d, dn, at, name, sizeof(name), &at) || at + 4 > dn)
            return;
        at += 4;
    }
    for (i = 0; i < rr; ++i) {
        unsigned type, len, rd;
        if (dns_name(d, dn, at, name, sizeof(name), &at) || at + 10 > dn)
            return;
        type = get16(d + at);
        len = get16(d + at + 8);
        rd = at + 10;
        if (rd + len > dn)
            return;
        at = rd + len;
        if (!strstr(name, "._airplay._tcp"))
            continue;
        pthread_mutex_lock(&g_fac.m);
        if (type == 33 && len >= 6 && !g_fac.have_port) {
            g_fac.port = get16(d + rd + 4);
            g_fac.have_port = 1;
            snprintf(g_fac.addr, sizeof(g_fac.addr), "%s", src);
            say("bridge: the head unit's AirPlay receiver: [%s]:%u (%s)", src, g_fac.port, name);
        } else if (type == 16 && !g_fac.have_txt) {
            unsigned k = rd;
            char keys[200];
            keys[0] = 0;
            while (k < rd + len) {
                const unsigned l = d[k];
                char kv[200];
                if (k + 1 + l > rd + len)
                    break;
                snprintf(kv, sizeof(kv), "%.*s", (int)l, (const char*)d + k + 1);
                if (!strncmp(kv, "pk=", 3))
                    snprintf(g_fac.pk, sizeof(g_fac.pk), "%s", kv + 3);
                else if (!strncmp(kv, "srcvers=", 8))
                    snprintf(g_fac.srcvers, sizeof(g_fac.srcvers), "%s", kv + 8);
                {   /* logged: the keys, and the values of features, flags and srcvers only (others may be private) */
                    char* eq = strchr(kv, '=');
                    if (eq && strncmp(kv, "features=", 9) && strncmp(kv, "srcvers=", 8) && strncmp(kv, "flags=", 6))
                        *eq = 0;
                    strncat(keys, " ", sizeof(keys) - strlen(keys) - 1);
                    strncat(keys, kv, sizeof(keys) - strlen(keys) - 1);
                }
                k += 1 + l;
            }
            g_fac.have_txt = 1;
            say("bridge: the head unit's AirPlay TXT:%s (key %s)", keys, g_fac.pk[0] ? "found" : "missing");
        }
        pthread_mutex_unlock(&g_fac.m);
    }
}

int ncm_bridge_factory(char addr[64], unsigned* port, char pk[136], char srcvers[32])
{
    int ok;
    pthread_mutex_lock(&g_fac.m);
    /* no pk on the car: its receiver is AirPlay 280.33.8 (TXT deviceid features fv flags model pi srcvers) */
    ok = g_fac.have_port && g_fac.have_txt && g_fac.addr[0];
    if (ok) {
        /* the receiver's own (IPv4) address, as it announces itself: the iPhone came over it (car 2026-11-26 20:47);
         * NBT_CP_START_V6=1: tap0's IPv6 link-local instead (car 2026-11-26 21:03: no connection, with wireless on) */
        const char* v6 = getenv("NBT_CP_START_V6");
        memcpy(addr, g_fac.tap6[0] && v6 && *v6 == '1' ? g_fac.tap6 : g_fac.addr, 64);
        *port = g_fac.port;
        memcpy(pk, g_fac.pk, 136);
        memcpy(srcvers, g_fac.srcvers, 32);
    }
    pthread_mutex_unlock(&g_fac.m);
    return ok;
}

/* a Bonjour question for _airplay._tcp.local, as if from the iPhone, into tap0: the head unit's mdnsd answers it on
 * tap0 (to the iPhone and to factory_scan) */
static unsigned ip4_csum(const unsigned char* p, unsigned n)
{
    unsigned long sum = 0;
    unsigned i;
    for (i = 0; i + 1 < n; i += 2)
        sum += get16(p + i);
    if (n & 1)
        sum += (unsigned)p[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (unsigned)(~sum & 0xffff);
}

/* an IPv4/UDP frame (UDP checksum 0, allowed for IPv4) */
static unsigned ip4_udp(unsigned char* f, const unsigned char dmac[6], const unsigned char smac[6],
                        const unsigned char sip[4], const unsigned char dip[4], unsigned sport, unsigned dport,
                        const unsigned char* data, unsigned n, unsigned ttl)
{
    memcpy(f, dmac, 6);
    memcpy(f + 6, smac, 6);
    put16(f + 12, 0x0800);
    memset(f + 14, 0, 28);
    f[14] = 0x45;
    put16(f + 16, 20 + 8 + n);
    f[22] = (unsigned char)ttl;
    f[23] = 17;
    memcpy(f + 26, sip, 4);
    memcpy(f + 30, dip, 4);
    put16(f + 24, ip4_csum(f + 14, 20));
    put16(f + 34, sport);
    put16(f + 36, dport);
    put16(f + 38, 8 + n);
    memcpy(f + 42, data, n);
    return 42 + n;
}

/* the iPhone asks for an IPv4 address over the cable (DHCP): answered here as the head unit's Wi-Fi DHCP does
 * (dhcpd.uap0.conf: 3 days, no router), never passed to tap0. 1 = it was DHCP. */
static int dhcp_answer(const unsigned char* f, unsigned n)
{
    static const unsigned char bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, all[4] = {255, 255, 255, 255};
    unsigned char block[28 + 600], r[300];
    unsigned udp, b, at, type = 0, reply, k;
    const unsigned char* asked = 0;
    if (n < 14 + 20 + 8 || get16(f + 12) != 0x0800 || f[14 + 9] != 17)
        return 0;
    udp = 14 + (f[14] & 0x0f) * 4u;
    if (n < udp + 8 || get16(f + udp) != 68 || get16(f + udp + 2) != 67)
        return 0;
    b = udp + 8;
    if (n < b + 240 || f[b] != 1 || f[b + 236] != 0x63 || f[b + 237] != 0x82 || f[b + 238] != 0x53 || f[b + 239] != 0x63)
        return 1;
    for (at = b + 240; at < n && f[at] != 255;) {
        if (f[at] == 0) {
            ++at;
            continue;
        }
        if (at + 2 > n || at + 2 + f[at + 1] > n)
            break;
        if (f[at] == 53 && f[at + 1] >= 1)
            type = f[at + 2];
        else if (f[at] == 50 && f[at + 1] == 4)
            asked = f + at + 2;
        at += 2u + f[at + 1];
    }
    if (type == 1)
        reply = 2;   /* DISCOVER -> OFFER */
    else if (type == 3)
        reply = asked && memcmp(asked, PEER_IP4, 4) ? 6 : 5;   /* REQUEST -> ACK, or NAK for another address */
    else
        return 1;
    memset(r, 0, sizeof(r));
    r[0] = 2, r[1] = 1, r[2] = 6;
    memcpy(r + 4, f + b + 4, 4);    /* xid */
    memcpy(r + 10, f + b + 10, 2);  /* flags */
    if (reply != 6)
        memcpy(r + 16, PEER_IP4, 4);
    memcpy(r + 28, f + b + 28, 16); /* chaddr */
    r[236] = 0x63, r[237] = 0x82, r[238] = 0x53, r[239] = 0x63;
    k = 240;
    r[k++] = 53, r[k++] = 1, r[k++] = (unsigned char)reply;
    r[k++] = 54, r[k++] = 4, memcpy(r + k, TAP_IP4, 4), k += 4;
    if (reply != 6) {
        r[k++] = 51, r[k++] = 4, put32(r + k, 259200), k += 4;
        r[k++] = 1, r[k++] = 4, r[k++] = 255, r[k++] = 255, r[k++] = 255, r[k++] = 0;
    }
    r[k++] = 255;
    ip4_udp(block + 28, bcast, g_mac, TAP_IP4, all, 67, 68, r, sizeof(r), 64);
    pthread_mutex_lock(&g_fac.m);
    memcpy(g_fac.peer_mac, f + 6, 6);
    g_fac.have_mac = 1;
    if (reply == 5)
        g_fac.leased = 1;
    pthread_mutex_unlock(&g_fac.m);
    say("bridge: DHCP %s from the iPhone -> %s %u.%u.%u.%u", type == 1 ? "discover" : "request",
        reply == 2 ? "offer" : reply == 5 ? "ack" : "nak", PEER_IP4[0], PEER_IP4[1], PEER_IP4[2], PEER_IP4[3]);
    send_block(block, 42 + sizeof(r));
    return 1;
}

void ncm_bridge_query(void)
{
    static const unsigned char q[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 8, '_', 'a', 'i', 'r', 'p', 'l', 'a', 'y',
                                      4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0, 0, 12, 0, 1};
    unsigned char f[14 + 40 + 8 + sizeof(q)];
    const unsigned ulen = 8 + sizeof(q);
    unsigned long sum = 0;
    unsigned i;
    if (g_tap < 0)
        return;
    pthread_mutex_lock(&g_fac.m);
    if (g_fac.leased) {   /* IPv4: the head unit's mdnsd is IPv4 only */
        static const unsigned char mc[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb}, grp[4] = {224, 0, 0, 251};
        unsigned char v4[14 + 20 + 8 + sizeof(q)];
        const unsigned len = ip4_udp(v4, mc, g_fac.peer_mac, PEER_IP4, grp, 5353, 5353, q, sizeof(q), 255);
        pthread_mutex_unlock(&g_fac.m);
        if (write(g_tap, v4, len) == (ssize_t)len)
            say("bridge: asked tap0 for _airplay._tcp (Bonjour over IPv4, in the iPhone's name)");
        return;
    }
    if (!g_fac.have_peer) {
        pthread_mutex_unlock(&g_fac.m);
        return;
    }
    memset(f, 0, sizeof(f));
    f[0] = 0x33, f[1] = 0x33, f[5] = 0xfb;
    memcpy(f + 6, g_fac.peer_mac, 6);
    memcpy(f + 14 + 8, g_fac.peer_ip6, 16);
    pthread_mutex_unlock(&g_fac.m);
    put16(f + 12, 0x86dd);
    f[14] = 0x60;
    put16(f + 14 + 4, ulen);
    f[14 + 6] = 17;
    f[14 + 7] = 255;
    f[14 + 24] = 0xff, f[14 + 25] = 0x02, f[14 + 39] = 0xfb;
    put16(f + 54, 5353);
    put16(f + 56, 5353);
    put16(f + 58, ulen);
    memcpy(f + 62, q, sizeof(q));
    for (i = 0; i < 32; i += 2)   /* pseudo header: addresses, length, next header */
        sum += get16(f + 14 + 8 + i);
    sum += ulen + 17;
    for (i = 0; i < ulen; i += 2)
        sum += i + 1 < ulen ? get16(f + 54 + i) : (unsigned)f[54 + i] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    sum = ~sum & 0xffff;
    put16(f + 60, sum ? (unsigned)sum : 0xffff);
    if (write(g_tap, f, sizeof(f)) == (ssize_t)sizeof(f))
        say("bridge: asked tap0 for _airplay._tcp (Bonjour, in the iPhone's name)");
}

/* ---- Evo port, --bridge-ctrl: a CarPlay controller (_carplay-ctrl._tcp) announced on tap0 in the iPhone's name ----
 * TXT id = the iPhone's Bluetooth address (the factory plug-in matches it with the phone it knows by Bluetooth), SRV
 * port = our server (cp_ctrl.c), A = the head unit's own tap0 address. TTL 120 s, re-announced, and a goodbye
 * (TTL 0) at the end, so Connectivity keeps no dead controller. */
#define CTL_INST "iPhone USB._carplay-ctrl._tcp.local"
#define CTL_HOST "nbt-usb-iphone.local"
static struct {
    char id[18];
    unsigned port;
    volatile int on;
    long long last;
} g_ctl;

static int has_bytes(const unsigned char* b, unsigned n, const char* s, unsigned k)   /* no memmem on QNX 6.5 */
{
    unsigned i;
    for (i = 0; i + k <= n; ++i)
        if (!memcmp(b + i, s, k))
            return 1;
    return 0;
}

static unsigned put_name(unsigned char* b, unsigned k, const char* name)
{
    while (*name) {
        const char* dot = strchr(name, '.');
        const unsigned l = dot ? (unsigned)(dot - name) : (unsigned)strlen(name);
        b[k++] = (unsigned char)l;
        memcpy(b + k, name, l);
        k += l;
        name += l + (dot ? 1 : 0);
    }
    b[k++] = 0;
    return k;
}

/* name, type, class, TTL; the caller writes rdlength and rdata */
static unsigned put_rr(unsigned char* b, unsigned k, const char* name, unsigned type, unsigned cls, unsigned ttl)
{
    k = put_name(b, k, name);
    put16(b + k, type);
    put16(b + k + 2, cls);
    put32(b + k + 4, ttl);
    return k + 8;
}

static void ctl_announce(unsigned ttl)
{
    static const unsigned char mc[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb}, grp[4] = {224, 0, 0, 251};
    unsigned char d[400], f[14 + 20 + 8 + 400], mac[6];
    unsigned k, r, len;
    if (g_tap < 0 || !g_ctl.port)
        return;
    pthread_mutex_lock(&g_fac.m);
    if (!g_fac.have_mac) {
        pthread_mutex_unlock(&g_fac.m);
        return;
    }
    memcpy(mac, g_fac.peer_mac, 6);
    pthread_mutex_unlock(&g_fac.m);
    memset(d, 0, 12);
    put16(d + 2, 0x8400);   /* response, authoritative */
    put16(d + 6, 4);
    k = put_rr(d, 12, "_carplay-ctrl._tcp.local", 12, 0x0001, ttl);
    r = k;
    k = put_name(d, k + 2, CTL_INST);
    put16(d + r, k - r - 2);
    k = put_rr(d, k, CTL_INST, 33, 0x8001, ttl);
    r = k;
    put16(d + k + 2, 0);
    put16(d + k + 4, 0);
    put16(d + k + 6, g_ctl.port);
    k = put_name(d, k + 8, CTL_HOST);
    put16(d + r, k - r - 2);
    k = put_rr(d, k, CTL_INST, 16, 0x8001, ttl);
    put16(d + k, 1 + 3 + 17);
    d[k + 2] = 3 + 17;
    memcpy(d + k + 3, "id=", 3);
    memcpy(d + k + 6, g_ctl.id, 17);
    k += 2 + 1 + 3 + 17;
    k = put_rr(d, k, CTL_HOST, 1, 0x8001, ttl);
    put16(d + k, 4);
    memcpy(d + k + 2, TAP_IP4, 4);
    k += 6;
    len = ip4_udp(f, mc, mac, PEER_IP4, grp, 5353, 5353, d, k, 255);
    g_ctl.last = ms_now();
    if (write(g_tap, f, len) != (ssize_t)len)
        say("ctrl: the announcement did not go into tap0");
}

static void* ctl_announcer(void* arg)
{
    static const unsigned at[] = {0, 1000, 2000, 4000, 8000, 16000};
    const long long t0 = ms_now();
    unsigned i = 0;
    (void)arg;
    while (g_ctl.on && !g_tap_stop) {
        const long long t = ms_now() - t0;
        if (i < sizeof(at) / sizeof(at[0]) ? t >= at[i] : ms_now() - g_ctl.last >= 20000) {
            ctl_announce(120);
            if (i < sizeof(at) / sizeof(at[0]))
                ++i;
        }
        sleep_ms(100);
    }
    return 0;
}

void ncm_bridge_ctrl(const char* id, unsigned port)
{
    snprintf(g_ctl.id, sizeof(g_ctl.id), "%s", id);
    g_ctl.port = port;
}

void ncm_bridge_ctrl_go(void)
{
    pthread_t t;
    if (g_ctl.on || !g_ctl.port)
        return;
    g_ctl.on = 1;
    if (pthread_create(&t, 0, ctl_announcer, 0))
        g_ctl.on = 0;
    else {
        pthread_detach(t);
        say("ctrl: announcing the controller on tap0 (port %u)", g_ctl.port);
    }
}

const unsigned char* ncm_bridge_ip4(void)
{
    return TAP_IP4;
}

/* a short line per frame (the first 60 each way, mDNS left to mdns_log): what the head unit and the iPhone say */
static void frame_brief(const unsigned char* f, unsigned n, const char* way)
{
    static unsigned logged_in, logged_out;
    unsigned* c = way[0] == 'i' ? &logged_in : &logged_out;
    char a[64], b[64];
    if (*c >= 60 || n < 14)
        return;
    if (get16(f + 12) == 0x86dd && n >= 14 + 40) {
        const unsigned nh = f[14 + 6], l4 = 14 + 40;
        ip6_text(f + 14 + 8, a, sizeof(a));
        ip6_text(f + 14 + 24, b, sizeof(b));
        if ((nh == 17 || nh == 6) && n >= l4 + 4 && (get16(f + l4) == 5353 || get16(f + l4 + 2) == 5353))
            return;
        ++*c;
        if (nh == 58 && n > l4)
            say("bridge: %s ICMPv6 %u %s -> %s", way, f[l4], a, b);
        else if (nh == 6 && n >= l4 + 14)
            say("bridge: %s TCP %s.%u -> %s.%u flags %02x", way, a, get16(f + l4), b, get16(f + l4 + 2), f[l4 + 13]);
        else if (nh == 17 && n >= l4 + 4)
            say("bridge: %s UDP %s.%u -> %s.%u", way, a, get16(f + l4), b, get16(f + l4 + 2));
        else
            say("bridge: %s IPv6 next header %u %s -> %s", way, nh, a, b);
    } else if (get16(f + 12) == 0x0800 && n >= 14 + 20) {
        const unsigned l4 = 14 + (f[14] & 0x0f) * 4u, p = f[14 + 9];
        if (p == 17 && n >= l4 + 4 && (get16(f + l4) == 5353 || get16(f + l4 + 2) == 5353))
            return;
        ++*c;
        if ((p == 17 || p == 6) && n >= l4 + 14)
            say("bridge: %s %s %u.%u.%u.%u.%u -> %u.%u.%u.%u.%u%s%02x", way, p == 6 ? "TCP" : "UDP", f[26], f[27],
                f[28], f[29], get16(f + l4), f[30], f[31], f[32], f[33], get16(f + l4 + 2), p == 6 ? " flags " : "",
                p == 6 ? f[l4 + 13] : 0);
        else
            say("bridge: %s IPv4 protocol %u %u.%u.%u.%u -> %u.%u.%u.%u", way, p, f[26], f[27], f[28], f[29], f[30],
                f[31], f[32], f[33]);
    } else {
        ++*c;
        say("bridge: %s type %04x, %u bytes", way, get16(f + 12), n);
    }
}

static void* tap_rx(void* arg)
{
    unsigned char block[28 + 1600];
    (void)arg;
    while (!g_tap_stop) {
        const ssize_t r = read(g_tap, block + 28, 1600);
        if (r <= 0) {
            if (g_tap_stop)
                break;
            sleep_ms(10);
            continue;
        }
        mdns_log(block + 28, (unsigned)r, "out");
        factory_scan(block + 28, (unsigned)r);
        frame_brief(block + 28, (unsigned)r, "out");
        if (g_ctl.on && ms_now() - g_ctl.last > 1000 && r > 42 &&
            has_bytes(block + 28 + 42, (unsigned)r - 42, "_carplay-ctrl", 13))   /* the head unit looks for controllers */
            ctl_announce(120);
        if (!g_fac.tap6[0] && r >= 14 + 40 && get16(block + 28 + 12) == 0x86dd && block[28 + 22] == 0xfe &&
            (block[28 + 23] & 0xc0) == 0x80) {
            pthread_mutex_lock(&g_fac.m);   /* tap0's own IPv6 link-local */
            ip6_text(block + 28 + 22, g_fac.tap6, sizeof(g_fac.tap6));
            pthread_mutex_unlock(&g_fac.m);
            say("bridge: tap0's IPv6 address %s", g_fac.tap6);
        }
        if (send_block(block, (unsigned)r) && g_frames_out < 5)
            say("bridge: a frame to the iPhone was not sent");
    }
    return 0;
}

int ncm_bridge_start(const unsigned char mac[6], ncm_send_fn send, void* ctx)
{
    char cmd[128];
    pthread_t t;
    memcpy(g_mac, mac, 6);
    g_send = send;
    g_ctx = ctx;
    g_bridged = 1;
    if (system("ifconfig tap0 destroy > /dev/null 2>&1; ifconfig tap0 create") != 0) {
        say("bridge: ifconfig tap0 create failed");
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "ifconfig tap0 link %02x:%02x:%02x:%02x:%02x:%02x active && ifconfig tap0 up", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (system(cmd) != 0)
        say("bridge: %s failed", cmd);
    snprintf(cmd, sizeof(cmd), "ifconfig tap0 inet %u.%u.%u.%u netmask 255.255.255.0", TAP_IP4[0], TAP_IP4[1],
             TAP_IP4[2], TAP_IP4[3]);
    if (system(cmd) != 0)
        say("bridge: %s failed", cmd);
    g_tap = open("/dev/tap0", O_RDWR);
    if (g_tap < 0) {
        say("bridge: /dev/tap0: cannot open");
        ncm_bridge_stop();
        return -1;
    }
    if (pthread_create(&t, 0, tap_rx, 0)) {
        ncm_bridge_stop();
        return -1;
    }
    snprintf(g_address, sizeof(g_address), "tap0");
    say("bridge: tap0 up with MAC %02x:%02x:%02x:%02x:%02x:%02x; the iPhone's link is now the head unit's interface",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    system("ifconfig tap0 | grep -E 'address|inet'");
    return 0;
}

void ncm_bridge_stop(void)
{
    if (g_ctl.on && g_tap >= 0) {   /* goodbye: the controller goes with us */
        g_ctl.on = 0;
        ctl_announce(0);
        ctl_announce(0);
        say("ctrl: controller withdrawn");
    }
    g_tap_stop = 1;
    if (g_tap >= 0)
        close(g_tap);
    g_tap = -1;
    if (system("ifconfig tap0 destroy") == 0)
        say("bridge: tap0 removed");
}

static err_t netif_setup(struct netif* netif)
{
    netif->name[0] = 'n';
    netif->name[1] = 'c';
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, g_mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHERNET | NETIF_FLAG_MLD6 | NETIF_FLAG_LINK_UP;
    netif->output_ip6 = ethip6_output;
    netif->linkoutput = link_output;
    return ERR_OK;
}

int ncm_start(const unsigned char mac[6], ncm_send_fn send, void* ctx)
{
    memcpy(g_mac, mac, 6);
    g_send = send;
    g_ctx = ctx;
    LOCK_TCPIP_CORE();
    if (!netif_add_noaddr(&g_netif, 0, netif_setup, tcpip_input)) {
        UNLOCK_TCPIP_CORE();
        say("ncm: no network interface");
        return -1;
    }
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);
    netif_create_ip6_linklocal_address(&g_netif, 1);
    netif_ip6_addr_set_state(&g_netif, 0, IP6_ADDR_PREFERRED);
    snprintf(g_address, sizeof(g_address), "%s", ip6addr_ntoa(netif_ip6_addr(&g_netif, 0)));
    UNLOCK_TCPIP_CORE();
    {   /* lower case, as xcertplay writes it */
        char* c;
        for (c = g_address; *c; ++c)
            if (*c >= 'A' && *c <= 'F')
                *c = (char)(*c - 'A' + 'a');
    }
    say("ncm: interface up, MAC %02x:%02x:%02x:%02x:%02x:%02x, address %s", mac[0], mac[1], mac[2], mac[3], mac[4],
        mac[5], g_address);
    return 0;
}

void ncm_counts(unsigned long long* in, unsigned long long* out)
{
    *in = g_frames_in;
    *out = g_frames_out;
}

const char* ncm_address(void)
{
    return g_address;
}

static void frame_in(const unsigned char* f, unsigned n)
{
    struct pbuf* p;
    if (n < 14 || n > 1600)
        return;
    pcap_frame(f, n);
    if (++g_frames_in == 1)
        say("ncm: first frame in (%u bytes, from %02x:%02x:%02x:%02x:%02x:%02x, type %04x)", n, f[6], f[7], f[8], f[9],
            f[10], f[11], get16(f + 12));
    if (g_bridged && g_tap < 0)   /* Evo port: frames after the bridge ended (car 2026-11-26: SIGSEGV, ip=0) */
        return;
    if (g_tap >= 0) {   /* Evo port: --bridge */
        mdns_log(f, n, "in");
        if (dhcp_answer(f, n))
            return;
        frame_brief(f, n, "in");
        if (get16(f + 12) == 0x86dd && n >= 14 + 40 && f[14 + 8] == 0xfe && (f[14 + 9] & 0xc0) == 0x80) {
            pthread_mutex_lock(&g_fac.m);   /* the iPhone's link-local address and MAC */
            memcpy(g_fac.peer_mac, f + 6, 6);
            memcpy(g_fac.peer_ip6, f + 14 + 8, 16);
            g_fac.have_peer = g_fac.have_mac = 1;
            pthread_mutex_unlock(&g_fac.m);
        }
        if (write(g_tap, f, n) != (ssize_t)n && g_frames_in < 5)
            say("bridge: a frame from the iPhone was not taken by tap0");
        return;
    }
    p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
    if (!p)
        return;
    pbuf_take(p, f, (u16_t)n);
    if (g_netif.input(p, &g_netif) != ERR_OK)
        pbuf_free(p);
}

/* the frames of one block */
static void block_in(const unsigned char* b, unsigned n)
{
    unsigned ndp = get16le(b + 10), hops = 0;
    while (ndp && hops++ < 16) {
        unsigned len, next, e;
        if (ndp + 12 > n || (get32le(b + ndp) & 0x00ffffffu) != (NDP16 & 0x00ffffffu))
            return;
        len = get16le(b + ndp + 4);
        next = get16le(b + ndp + 6);
        for (e = ndp + 8; e + 4 <= ndp + len && e + 4 <= n; e += 4) {
            const unsigned at = get16le(b + e), dl = get16le(b + e + 2);
            if (!at || !dl)
                break;
            if (at + dl <= n)
                frame_in(b + at, dl);
        }
        ndp = next;
    }
}

/* bytes from the bulk IN pipe (one or more blocks, maybe a pad byte after one of a multiple of 512 bytes) */
void ncm_input(const unsigned char* data, unsigned n)
{
    if (n > sizeof(g_rx) - g_rxn) {
        say("ncm: receive buffer overflow, %u bytes dropped", g_rxn);
        g_rxn = 0;
        if (n > sizeof(g_rx))
            return;
    }
    memcpy(g_rx + g_rxn, data, n);
    g_rxn += n;
    for (;;) {
        unsigned len, used;
        if (g_rxn < 12)
            return;
        if (get32le(g_rx) != NTH16) {   /* not at a block: on to the next "NCMH" */
            unsigned i;
            for (i = 1; i + 4 <= g_rxn && get32le(g_rx + i) != NTH16; ++i) {
            }
            if (i > 1 || g_rx[0])
                say("ncm: %u bytes outside a block skipped", i);
            memmove(g_rx, g_rx + i, g_rxn - i);
            g_rxn -= i;
            continue;
        }
        len = get16le(g_rx + 8);
        if (len < 12 || len > sizeof(g_rx)) {
            say("ncm: block length %u: buffer reset", len);
            g_rxn = 0;
            return;
        }
        if (g_rxn < len)
            return;
        block_in(g_rx, len);
        used = len;
        if (len % 512 == 0 && g_rxn > len && g_rx[len] == 0)
            used = len + 1;   /* the short-packet pad byte */
        memmove(g_rx, g_rx + used, g_rxn - used);
        g_rxn -= used;
    }
}
