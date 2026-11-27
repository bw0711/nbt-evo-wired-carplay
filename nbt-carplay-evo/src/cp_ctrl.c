/* nbt_carplay, Evo port, --bridge-ctrl: the head unit's own CarPlay finds "the iPhone" the factory way.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The factory CarPlay (Connectivity with Apple's plug-in) makes a session ACTIVE, and so shows the iDrive entry, only
 * while it is connecting to a phone it knows by Bluetooth. It connects to a "controller": a _carplay-ctrl._tcp
 * Bonjour service whose TXT id is the phone's Bluetooth address, with GET /ctrl-int/1/connect. Over the cable the
 * iPhone announces no controller, so the bridge announces one in its name on tap0 (cp_ncm.c), with its address as
 * this head unit's own 172.16.226.1 and the port of the server here. When the request has been answered,
 * CarPlayStartSession goes to the iPhone (cp_iap2.c) and the iPhone comes to the factory receiver as over Wi-Fi.
 * Findings: wired-carplay/factory-like-device-list-re-2026-11-26.md (car 2026-11-26).
 */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "cp.h"

static int g_ls = -1;
static volatile int g_connected, g_ctrl_stop;

/* one HTTP request (header and a body of Content-Length) into req; 1, 0 closed, -1 timeout or too long */
static int read_request(int c, char* req, unsigned size)
{
    unsigned n = 0, want = 0;
    const long long t0 = ms_now();
    req[0] = 0;
    while (!g_ctrl_stop && ms_now() - t0 < 5000) {
        fd_set r;
        struct timeval tv = {0, 200000};
        ssize_t k;
        const char* end = strstr(req, "\r\n\r\n");
        if (end) {
            const char* cl = strstr(req, "Content-Length:");
            if (!want)
                want = (unsigned)(end + 4 - req) + (cl && cl < end ? (unsigned)strtoul(cl + 15, 0, 10) : 0);
            if (n >= want)
                return 1;
        }
        if (n + 1 >= size)
            return -1;
        FD_ZERO(&r);
        FD_SET(c, &r);
        if (select(c + 1, &r, 0, 0, &tv) <= 0)
            continue;
        k = read(c, req + n, size - 1 - n);
        if (k <= 0)
            return 0;
        n += (unsigned)k;
        req[n] = 0;
    }
    return -1;
}

static void* ctrl_conn(void* arg)
{
    const int c = (int)(long)arg;
    char req[4096];
    while (!g_ctrl_stop && read_request(c, req, sizeof(req)) == 1) {
        static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        const char* e = strstr(req, "\r\n");
        char headers[200];
        const char* h;
        headers[0] = 0;
        for (h = e; h && h[2] != '\r'; h = strstr(h + 2, "\r\n")) {   /* the header names only */
            const char* colon = strchr(h + 2, ':');
            if (!colon || colon - h > 60)
                break;
            snprintf(headers + strlen(headers), sizeof(headers) - strlen(headers), " %.*s", (int)(colon - h - 2), h + 2);
        }
        say("ctrl: the head unit asks \"%.*s\" (%s)", e ? (int)(e - req) : 80, req, headers);
        if (strstr(req, " /ctrl-int/1/connect ") && !g_connected) {
            g_connected = 1;
            say("ctrl: the head unit's CarPlay connected to the controller we announced");
        }
        if (write(c, ok, sizeof(ok) - 1) != (ssize_t)(sizeof(ok) - 1))
            break;
    }
    close(c);
    return 0;
}

static void* ctrl_serve(void* arg)
{
    (void)arg;
    while (!g_ctrl_stop) {
        fd_set r;
        struct timeval tv = {0, 300000};
        pthread_t t;
        int c;
        FD_ZERO(&r);
        FD_SET(g_ls, &r);
        if (select(g_ls + 1, &r, 0, 0, &tv) <= 0)
            continue;
        c = accept(g_ls, 0, 0);
        if (c < 0)
            continue;
        if (pthread_create(&t, 0, ctrl_conn, (void*)(long)c))
            close(c);
        else
            pthread_detach(t);
    }
    return 0;
}

/* the server on ip (the head unit's tap0 address): its port, 0 if none */
unsigned ctrl_start(const unsigned char ip[4])
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    pthread_t t;
    const int one = 1;
    g_ls = socket(AF_INET, SOCK_STREAM, 0);
    if (g_ls < 0)
        return 0;
    setsockopt(g_ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr, ip, 4);
    if (bind(g_ls, (struct sockaddr*)&a, sizeof(a)) || listen(g_ls, 4) ||
        getsockname(g_ls, (struct sockaddr*)&a, &al) || pthread_create(&t, 0, ctrl_serve, 0)) {
        say("ctrl: no server for the controller");
        close(g_ls);
        g_ls = -1;
        return 0;
    }
    pthread_detach(t);
    say("ctrl: controller server on %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(a.sin_port));
    return ntohs(a.sin_port);
}

int ctrl_connected(void)
{
    return g_connected;
}

void ctrl_stop(void)
{
    g_ctrl_stop = 1;
    if (g_ls >= 0)
        close(g_ls);
    g_ls = -1;
}
