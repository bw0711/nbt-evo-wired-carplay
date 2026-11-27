/* IDEmu bench: a stand-in for the head unit's Apple authentication chip (ACP 2.0B), for mm-ipod's
 * ipod-acp-har-i2c-iap2.so, which on the car (wired-carplay/nbt-carplay-evo/src/cp_acp.c):
 *   - takes the chip lock in the shared-memory object /dev/shm_acp_lock ({int ready = 1; pthread_mutex_t}; acpd
 *     makes it on the car, this program here),
 *   - reads registers with Harman's dev-i2c devctl 0xC80C0502 {send_len = 1, recv_len = n, stop, data[2048] =
 *     register} on /dev/i2c/i2c_1/acp, and writes them with write({register, data...}).
 * Registers (Apple's MFi 2.0B layout): 00..03 version 03 / firmware 01 / protocol 2.0, 04 device id, 05 error code,
 * 10 control and status (write 1: sign; reads 0x10 when the signature is ready), 11 signature length, 12 signature,
 * 20 challenge length, 21 challenge, 30 certificate length, 31.. certificate in pages of 128 bytes.
 * The certificate is a real one (argv[1]: the car's, saved by the probe; it stays on this Mac and in the emulator). The
 * signature is made up: whoever checks it here is our own bridge's device role, which does not check it.
 * Run: fakeacp CERT.der [node]   (the node defaults to /dev/i2c/i2c_1/acp) */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/mman.h>
#include <unistd.h>

#define HAR_I2C_SENDRECV 0xC80C0502u

static unsigned char cert[1280], challenge[128], sig[128], status;
static unsigned certn, chn, nreads, nwrites, nsigns;

static void reg_read(unsigned reg, unsigned char* out, unsigned n)
{
    static const unsigned char version[4] = {0x03, 0x01, 0x02, 0x00}, id[4] = {0x00, 0x00, 0x02, 0x00};
    unsigned i;
    memset(out, 0, n);
    if (reg <= 3)
        for (i = 0; i < n && reg + i < 4; ++i)
            out[i] = version[reg + i];
    else if (reg == 0x04)
        memcpy(out, id, n < 4 ? n : 4);
    else if (reg == 0x10)
        out[0] = status;
    else if (reg == 0x11 && n >= 2)
        out[1] = sizeof(sig);
    else if (reg == 0x12)
        memcpy(out, sig, n < sizeof(sig) ? n : sizeof(sig));
    else if (reg == 0x20 && n >= 2) {
        out[0] = (unsigned char)(chn >> 8);
        out[1] = (unsigned char)chn;
    } else if (reg == 0x21)
        memcpy(out, challenge, n < sizeof(challenge) ? n : sizeof(challenge));
    else if (reg == 0x30 && n >= 2) {
        out[0] = (unsigned char)(certn >> 8);
        out[1] = (unsigned char)certn;
    } else if (reg >= 0x31 && reg <= 0x3a) {
        const unsigned off = (reg - 0x31) * 128;
        if (off < certn)
            memcpy(out, cert + off, n < certn - off ? n : certn - off);
    }
    if (++nreads <= 3 || reg == 0x30 || reg == 0x12)
        printf("fakeacp: read register 0x%02x (%u bytes)\n", reg, n), fflush(stdout);
}

static void reg_write(const unsigned char* b, unsigned n)
{
    unsigned i;
    ++nwrites;
    if (b[0] == 0x20 && n >= 3)
        chn = (unsigned)b[1] << 8 | b[2];
    else if (b[0] == 0x21)
        memcpy(challenge, b + 1, n - 1 < sizeof(challenge) ? n - 1 : sizeof(challenge));
    else if (b[0] == 0x10 && n >= 2 && b[1] == 1) {
        for (i = 0; i < sizeof(sig); ++i)
            sig[i] = (unsigned char)(challenge[i % (chn ? chn : 1)] ^ (i * 37 + 1));
        status = 0x10;
        ++nsigns;
        printf("fakeacp: signature %u made for a %u-byte challenge\n", nsigns, chn);
        fflush(stdout);
    }
}

static int io_write(resmgr_context_t* ctp, io_write_t* msg, RESMGR_OCB_T* ocb)
{
    unsigned char b[300];
    unsigned n;
    int st = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (st != EOK)
        return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    n = (unsigned)msg->i.nbytes < sizeof(b) ? (unsigned)msg->i.nbytes : (unsigned)sizeof(b);
    if (n && resmgr_msgread(ctp, b, n, sizeof(msg->i)) == (int)n)
        reg_write(b, n);
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}

static int io_devctl(resmgr_context_t* ctp, io_devctl_t* msg, RESMGR_OCB_T* ocb)
{
    unsigned char* d;
    uint32_t want;
    int st = iofunc_devctl_default(ctp, msg, ocb);
    if (st != (int)_RESMGR_DEFAULT)
        return st;
    if ((unsigned)msg->i.dcmd != HAR_I2C_SENDRECV)
        return ENOSYS;
    d = _DEVCTL_DATA(msg->i);   /* the whole request is in the message (msg_max_size below) */
    memcpy(&want, d + 4, 4);
    if (want > 2048)
        return EINVAL;
    reg_read(d[12], d + 12, want);
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = EOK;
    msg->o.nbytes = 12 + want;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + 12 + want);
}

int main(int argc, char** argv)
{
    static resmgr_connect_funcs_t cf;
    static resmgr_io_funcs_t iof;
    static iofunc_attr_t attr;
    resmgr_attr_t ra;
    dispatch_t* dpp;
    dispatch_context_t* ctp;
    pthread_mutexattr_t ma;
    unsigned char* shm;
    const char* node = argc > 2 ? argv[2] : "/dev/i2c/i2c_1/acp";
    FILE* f = argc > 1 ? fopen(argv[1], "rb") : 0;
    int fd;
    if (!f) {
        printf("fakeacp: usage: fakeacp CERT.der [node]\n");
        return 1;
    }
    certn = (unsigned)fread(cert, 1, sizeof(cert), f);
    fclose(f);
    /* the chip lock, as acpd makes it */
    fd = shm_open("/dev/shm_acp_lock", O_RDWR | O_CREAT, 0777);
    if (fd < 0 || ftruncate(fd, 12) ||
        (shm = mmap(0, 12, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        perror("fakeacp: /dev/shm_acp_lock");
        return 1;
    }
    close(fd);
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init((pthread_mutex_t*)(shm + 4), &ma);
    *(volatile int*)shm = 1;
    dpp = dispatch_create();
    memset(&ra, 0, sizeof(ra));
    ra.nparts_max = 1;
    ra.msg_max_size = 4096;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iof.write = io_write;
    iof.devctl = io_devctl;
    iofunc_attr_init(&attr, S_IFCHR | 0666, 0, 0);
    if (!dpp || resmgr_attach(dpp, &ra, node, _FTYPE_ANY, 0, &cf, &iof, &attr) == -1) {
        perror("fakeacp: resmgr_attach");
        return 1;
    }
    printf("fakeacp: %s ready (certificate %u bytes, lock /dev/shm_acp_lock)\n", node, certn);
    fflush(stdout);
    ctp = dispatch_context_alloc(dpp);
    for (;;) {
        if (!(ctp = dispatch_block(ctp)))
            return 1;
        dispatch_handler(ctp);
    }
}
