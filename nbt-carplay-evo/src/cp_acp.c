/* nbt_carplay: the car's Apple authentication chip.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu (NBT version); NBT Evo port 2026-11-26
 *
 * NBT Evo (car 2026-11-26, nbt_carplay_probe_evo): ACP 2.0B on /dev/i2c/i2c_1/acp (NBT_ACP_NODE overrides it),
 * version 03, protocol 2.0, certificate 908 bytes, a signature in about 370 ms. The head unit's dev-i2c is Harman's:
 * registers are read with its send-and-receive devctl 0xC80C0502 {send_len = 1, recv_len = n, stop = 1,
 * data[2048] = register}, written with write({register, data...}), each followed by a 10 ms pause (300 ms after
 * register 0x10), as the factory driver libipod-acp-har-i2c-iap2.so does. The factory users of the chip (mm-ipod for
 * wireless CarPlay, io-fs-media -dipod for an iPod / iPhone on USB) share it through a process-shared mutex in the
 * shared-memory object /dev/shm_acp_lock that acpd creates ({int ready = 1; pthread_mutex_t mutex}, 12 bytes); every
 * use of the chip here takes it too (5 s timeout, then the use fails). The NBT's way (write the register number, read
 * after a pause) stays as the fallback when the devctl is refused.
 * Used by iAP2 (authentication) and AirPlay (auth-setup), one at a time.
 * Bench only (IDEmu has no chip): when the chip node does not exist and NBT_ACP_CERT_FILE names a certificate file
 * (for example the car's, saved by the probe), that certificate is offered and every signature fails, so the steps up
 * to the iPhone's authentication challenge can run on the emulator. On the car the node exists and this never applies.
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "cp.h"

#ifndef ACP_NODE
#define ACP_NODE "/dev/i2c/i2c_1/acp"
#endif
#define ACP_SHM "/dev/shm_acp_lock"
#define HAR_I2C_SENDRECV 0xC80C0502u

struct har_sendrecv {
    uint32_t send_len;
    uint32_t recv_len;
    uint32_t stop;
    unsigned char data[2048];
};

static int g_fd = -1;
static int g_devctl;   /* 1: Harman's devctl reads (the Evo); 0: write the register, then read (the NBT) */
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_cert[1280];
static unsigned g_certn;
static unsigned char g_version[4];
static volatile unsigned char* g_shm;
static int g_shm_tried;
static int g_bench;   /* 1: no chip, certificate from NBT_ACP_CERT_FILE (IDEmu) */

static const char* node(void)
{
    const char* e = getenv("NBT_ACP_NODE");
    return e && *e ? e : ACP_NODE;
}

/* the head unit's shared chip lock (taken with g_m held) */
static int shared_lock(void)
{
    struct timespec t;
    int rc;
    if (!g_shm_tried) {
        int fd;
        g_shm_tried = 1;
        fd = shm_open(ACP_SHM, O_RDWR, 0777);
        if (fd < 0) {
            say("acp: no %s (%s): going on without the head unit's chip lock", ACP_SHM, strerror(errno));
        } else {
            void* p = mmap(0, 12, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (p == MAP_FAILED)
                say("acp: %s could not be mapped (%s): going on without the lock", ACP_SHM, strerror(errno));
            else
                g_shm = (volatile unsigned char*)p;
        }
    }
    if (!g_shm || *(volatile int*)g_shm != 1)
        return 0;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += 5;
    rc = pthread_mutex_timedlock((pthread_mutex_t*)(g_shm + 4), &t);
    if (rc != EOK) {
        say("acp: the chip is in use by the head unit (lock: %s)", strerror(rc));
        return -1;
    }
    return 0;
}

static void shared_unlock(void)
{
    if (g_shm && *(volatile int*)g_shm == 1)
        pthread_mutex_unlock((pthread_mutex_t*)(g_shm + 4));
}

static int raw_write(const unsigned char* data, unsigned n)
{
    int tries;
    for (tries = 0; tries < 50; ++tries) {
        if (write(g_fd, data, n) == (ssize_t)n) {
            if (g_devctl)
                usleep(data[0] == 0x10 ? 300000 : 10000);
            return 0;
        }
        usleep(1000);
    }
    say("acp: write of register 0x%02x failed: %s", data[0], strerror(errno));
    return -1;
}

static int read_devctl(unsigned reg, unsigned char* out, unsigned n)
{
    static struct har_sendrecv m;
    int tries, rc = EOK;
    if (n == 0 || n > sizeof(m.data))
        return -1;
    for (tries = 0; tries < 50; ++tries) {
        memset(&m, 0, 16);
        m.send_len = 1;
        m.recv_len = n;
        m.stop = 1;
        m.data[0] = (unsigned char)reg;
        rc = devctl(g_fd, HAR_I2C_SENDRECV, &m, sizeof(m), 0);
        usleep(10000);
        if (rc == EOK) {
            memcpy(out, m.data, n);
            return 0;
        }
    }
    errno = rc;
    return -1;
}

static int read_raw(unsigned reg, unsigned char* out, unsigned n)
{
    const unsigned char r = (unsigned char)reg;
    int tries, k;
    for (tries = 0; tries < 5; ++tries) {
        if (raw_write(&r, 1))
            return -1;
        for (k = 0; k < 20; ++k) {
            usleep(1000);
            if (read(g_fd, out, n) == (ssize_t)n)
                return 0;
        }
    }
    return -1;
}

static int reg_read(unsigned reg, unsigned char* out, unsigned n)
{
    if ((g_devctl ? read_devctl : read_raw)(reg, out, n) == 0)
        return 0;
    say("acp: read of register 0x%02x (%u bytes) failed: %s", reg, n, strerror(errno));
    return -1;
}

static int reg_write(unsigned reg, const unsigned char* data, unsigned n)
{
    unsigned char b[1 + 128];
    if (n > 128)
        return -1;
    b[0] = (unsigned char)reg;
    memcpy(b + 1, data, n);
    return raw_write(b, n + 1);
}

int acp_open(void)
{
    unsigned char n[2];
    unsigned i;
    int tries, locked = 0;
    pthread_mutex_lock(&g_m);
    if (g_fd >= 0) {
        pthread_mutex_unlock(&g_m);
        return 0;
    }
    g_fd = open(node(), O_RDWR);
    if (g_fd < 0 && errno == ENOENT && getenv("NBT_ACP_CERT_FILE")) {
        FILE* f = fopen(getenv("NBT_ACP_CERT_FILE"), "rb");
        g_certn = f ? (unsigned)fread(g_cert, 1, sizeof(g_cert), f) : 0;
        if (f)
            fclose(f);
        if (g_certn) {
            g_bench = 1;
            g_fd = 1000000;   /* "open", never used for I/O */
            g_version[0] = 0x03;
            g_version[2] = 2;
            say("acp: BENCH: no chip, certificate %u bytes from %s; signatures will fail", g_certn,
                getenv("NBT_ACP_CERT_FILE"));
            pthread_mutex_unlock(&g_m);
            return 0;
        }
    }
    if (g_fd < 0) {
        say("acp: %s: %s", node(), strerror(errno));
        goto fail;
    }
    if (shared_lock())
        goto fail;
    locked = 1;
    /* the Evo's way first: version 0x03 / 0x05 / 0x07 in register 0 */
    g_devctl = 1;
    if (read_devctl(0x00, g_version, 4) || (g_version[0] != 0x03 && g_version[0] != 0x05 && g_version[0] != 0x07)) {
        g_devctl = 0;   /* the NBT's way; the first answer after a pause can be ff ff ff ff (car 2026-11-23) */
        for (tries = 0; tries < 3; ++tries)
            if (!read_raw(0x00, g_version, 4) && g_version[0] != 0xff)
                break;
        if (tries == 3) {
            say("acp: no way reads the chip");
            goto fail;
        }
    }
    say("acp: %s via %s: version 0x%02x (%s), protocol %u.%u", node(), g_devctl ? "devctl" : "write+read",
        g_version[0], g_version[0] == 0x03 ? "2.0B" : g_version[0] == 0x05 ? "2.0C" : g_version[0] == 0x07 ? "3.0" : "?",
        g_version[2], g_version[3]);
    if (reg_read(0x30, n, 2))
        goto fail;
    g_certn = get16(n);
    if (!g_certn || g_certn > sizeof(g_cert)) {
        say("acp: certificate length %u", g_certn);
        goto fail;
    }
    for (i = 0; i * 128 < g_certn; ++i)
        if (reg_read(0x31 + i, g_cert + i * 128, g_certn - i * 128 < 128 ? g_certn - i * 128 : 128))
            goto fail;
    say("acp: certificate %u bytes", g_certn);
    shared_unlock();
    pthread_mutex_unlock(&g_m);
    return 0;
fail:
    if (locked)
        shared_unlock();
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
    pthread_mutex_unlock(&g_m);
    return -1;
}

int acp_cert(const unsigned char** cert, unsigned* n)
{
    if (acp_open())
        return -1;
    *cert = g_cert;
    *n = g_certn;
    return 0;
}

int acp_major(void)
{
    return g_version[2] ? g_version[2] : 2;
}

/* challenge length, challenge, start; the status polled until "signature ready" (0x10), up to 5 s */
int acp_sign(const unsigned char* challenge, unsigned clen, unsigned char* sig, unsigned* slen)
{
    unsigned char b[2];
    unsigned i, status = 0, n;
    const long long t0 = ms_now();
    int r = -1;
    if (clen == 0 || clen > 128 || acp_open())
        return -1;
    if (g_bench) {
        say("acp: BENCH: no chip, no signature for the %u-byte challenge", clen);
        return -1;
    }
    pthread_mutex_lock(&g_m);
    if (shared_lock()) {
        pthread_mutex_unlock(&g_m);
        return -1;
    }
    put16(b, clen);
    if (reg_write(0x20, b, 2) || reg_write(0x21, challenge, clen))
        goto out;
    b[0] = 1;
    if (reg_write(0x10, b, 1))
        goto out;
    for (i = 0; i < 250; ++i) {
        usleep(20000);
        if (reg_read(0x10, b, 1) == 0) {
            status = b[0];
            if ((status & 0x70) == 0x10 || (status & 0x80))
                break;
        }
    }
    if ((status & 0x70) != 0x10) {
        say("acp: signing ended with status 0x%02x after %lld ms", status, ms_now() - t0);
        if (reg_read(0x05, b, 1) == 0)
            say("acp: error code 0x%02x", b[0]);
        goto out;
    }
    if (reg_read(0x11, b, 2))
        goto out;
    n = get16(b);
    if (n == 0 || n > 128 || reg_read(0x12, sig, n))
        goto out;
    *slen = n;
    say("acp: signed %u bytes in %lld ms", clen, ms_now() - t0);
    r = 0;
out:
    shared_unlock();
    pthread_mutex_unlock(&g_m);
    return r;
}
