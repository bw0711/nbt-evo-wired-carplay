/* Host test of nbtcp_iap2_adapter.c (Mac, ASan/UBSan):
 *   mkdir -p /tmp/nbtcp-test && rm -f /tmp/nbtcp-test/starts /tmp/nbtcp-test/disable && cc -std=gnu99 -g -Wall -Wextra -Werror \
 *      -fsanitize=address,undefined -DADAPTER_TEST -DBRIDGE_PORT=47611 -DSTATE_DIR='"/tmp/nbtcp-test"' -DHEALTHY_S=1 \
 *      -DLOG_PATH='"/tmp/nbtcp-test/log"' -DSERVICE_PID='"/tmp/nbtcp-test/cpd.pid"' -o test_host test_host.c nbtcp_iap2_adapter.c -lpthread && ./test_host
 * A fake "next adapter" stands for the factory one (mm-ipod's node): it records what reaches messageReceived and, when
 * told, "writes" like mm-ipod by calling the send() it was given. A fake plug-in send stands for the AirPlay session.
 * A TCP client stands for the bridge. Checks: calls pass through; mm-ipod's writes go to AirPlay without the bridge, to
 * the bridge with it, to AirPlay again after it left; the bridge's frames reach messageReceived; the plug-in's ctx
 * reaches the next adapter's send unchanged. */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>

#ifndef TEST_STATE
#define TEST_STATE "/tmp/nbtcp-test"
#endif
typedef int (*send_fn)(const unsigned char*, unsigned, void*);
struct cxx {
    void** vptr;
};
extern struct cxx* (*test_next_get)(void);
void* iAP2WiFiAdapterGet(void);
void iAP2WiFiAdapterDispose(void*);

/* the fake next adapter */
static struct {
    send_fn send;
    void* ctx;
    int setups, teardowns, deleted;
    unsigned char got[256];
    unsigned gotn;
} N;
static int n_init(struct cxx* s, void* cfg, void* ctx, send_fn send)
{
    (void)s;
    assert(cfg == (void*)0x1234);
    N.ctx = ctx;
    N.send = send;
    return 0;
}
static void n_setup(struct cxx* s) { (void)s; N.setups++; }
static void n_teardown(struct cxx* s) { (void)s; N.teardowns++; }
static void n_received(struct cxx* s, const unsigned char* p, unsigned n)
{
    (void)s;
    memcpy(N.got + N.gotn, p, n);
    N.gotn += n;
}
static struct cxx* n_dtor(struct cxx* s) { return s; }
static struct cxx* n_delete(struct cxx* s) { N.deleted++; return s; }
static void* n_vt[] = {(void*)n_init, (void*)n_setup, (void*)n_teardown, (void*)n_received, (void*)n_dtor, (void*)n_delete};
static struct cxx n_obj = {n_vt};
static struct cxx* n_get(void) { return &n_obj; }

/* the fake AirPlay session */
static struct {
    unsigned char got[256];
    unsigned gotn;
    void* ctx;
} A;
static int air_send(const unsigned char* p, unsigned n, void* ctx)
{
    memcpy(A.got + A.gotn, p, n);
    A.gotn += n;
    A.ctx = ctx;
    return (int)n;
}

typedef int (*init_fn)(void*, void*, void*, send_fn);
typedef void (*void_fn)(void*);
typedef void (*recv_fn)(void*, const unsigned char*, unsigned);

static int bridge_connect(void)
{
    struct sockaddr_in sa;
    int s = socket(AF_INET, SOCK_STREAM, 0), i;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(47611);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (i = 0; i < 50 && connect(s, (struct sockaddr*)&sa, sizeof(sa)); i++)
        usleep(20000);
    assert(i < 50);
    usleep(400000);   /* the adapter's accept loop wakes every 300 ms */
    return s;
}

int main(void)
{
    void* ad;
    void** vt;
    const unsigned char probe[] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    unsigned char frame[64];
    int b, k;
    test_next_get = n_get;
    ad = iAP2WiFiAdapterGet();
    vt = *(void***)ad;
    /* the plug-in: initialize(config, ctx = CCarPlayServiceCore*, send) */
    ((init_fn)vt[0])(ad, (void*)0x1234, (void*)0xc0ffee, air_send);
    assert(N.send && N.send != air_send && N.ctx == (void*)0xc0ffee);
    ((void_fn)vt[2])(ad);   /* the plug-in's session stop runs teardown for any transport, also without a setup */
    ((void_fn)vt[1])(ad);
    ((void_fn)vt[2])(ad);
    ((void_fn)vt[1])(ad);   /* a second session (cable plugged again) */
    ((void_fn)vt[2])(ad);
    assert(N.setups == 2 && N.teardowns == 3);   /* passed on as they come */
    /* wireless: mm-ipod's write goes into the AirPlay session with the plug-in's ctx */
    assert(N.send(probe, sizeof(probe), N.ctx) == (int)sizeof(probe));
    assert(A.gotn == sizeof(probe) && !memcmp(A.got, probe, sizeof(probe)) && A.ctx == (void*)0xc0ffee);
    /* wireless: the iPhone's answer from AirPlay reaches the node */
    ((recv_fn)vt[3])(ad, probe, sizeof(probe));
    assert(N.gotn == sizeof(probe));
    N.gotn = 0;
    /* wired: the bridge connects; mm-ipod's write goes to it, not to AirPlay */
    b = bridge_connect();
    A.gotn = 0;
    assert(N.send(probe, sizeof(probe), N.ctx) == (int)sizeof(probe));
    assert(A.gotn == 0);
    for (k = 0; k < 3 + (int)sizeof(probe);) {   /* TCP may cut it anywhere */
        const int r = (int)read(b, frame + k, sizeof(frame) - (unsigned)k);
        assert(r > 0);
        k += r;
    }
    assert(k == 3 + (int)sizeof(probe) && frame[0] == 1 && frame[1] == 0 && frame[2] == sizeof(probe));
    assert(!memcmp(frame + 3, probe, sizeof(probe)));
    /* the bridge answers: it reaches the node through the next adapter's messageReceived */
    frame[0] = 1;
    frame[1] = 0;
    frame[2] = sizeof(probe);
    memcpy(frame + 3, probe, sizeof(probe));
    assert(write(b, frame, 3 + sizeof(probe)) == 3 + (int)sizeof(probe));
    frame[0] = 1;   /* a second frame split over two writes */
    frame[2] = 2;
    frame[3] = 0xab;
    frame[4] = 0xcd;
    assert(write(b, frame, 2) == 2);
    usleep(50000);
    assert(write(b, frame + 2, 3) == 3);
    usleep(200000);
    assert(N.gotn == sizeof(probe) + 2 && N.got[sizeof(probe)] == 0xab && N.got[sizeof(probe) + 1] == 0xcd);
    /* a second bridge is refused while the first is there */
    {
        int b2 = bridge_connect();
        char c;
        assert(read(b2, &c, 1) == 0);
        close(b2);
    }
    /* the bridge leaves: back to AirPlay */
    close(b);
    usleep(400000);
    A.gotn = 0;
    assert(N.send(probe, sizeof(probe), N.ctx) == (int)sizeof(probe));
    assert(A.gotn == sizeof(probe));
    /* and can come back */
    b = bridge_connect();
    A.gotn = 0;
    assert(N.send(probe, 2, N.ctx) == 2 && A.gotn == 0);
    close(b);
    iAP2WiFiAdapterDispose(ad);
    assert(N.deleted == 1);
    {   /* a clean dispose cleared the count */
        FILE* f = fopen(TEST_STATE "/starts", "r");
        assert(f && fgetc(f) == '0');
        fclose(f);
    }
    /* boot-loop guard: two starts that did not last (count 2 in the file) -> safe mode: the plug-in gets the next
     * adapter's own object, which then talks to AirPlay directly (as before the install) */
    {
        FILE* f = fopen(TEST_STATE "/starts", "w");
        void* a2;
        fprintf(f, "2\n");
        fclose(f);
        memset(&N, 0, sizeof(N));
        a2 = iAP2WiFiAdapterGet();
        assert(a2 == &n_obj);
        ((init_fn)(*(void***)a2)[0])(a2, (void*)0x1234, (void*)0xc0ffee, air_send);
        assert(N.send == air_send && N.ctx == (void*)0xc0ffee);
        f = fopen(TEST_STATE "/starts", "r");
        assert(f && fgetc(f) == '0');   /* the next start tries again */
        fclose(f);
        iAP2WiFiAdapterDispose(a2);
        assert(N.deleted == 1);
    }
    /* the disable file: safe mode, and the count is left alone */
    {
        FILE* f = fopen(TEST_STATE "/disable", "w");
        void* a4;
        fclose(f);
        memset(&N, 0, sizeof(N));
        a4 = iAP2WiFiAdapterGet();
        assert(a4 == &n_obj);
        iAP2WiFiAdapterDispose(a4);
        assert(N.deleted == 1);
        unlink(TEST_STATE "/disable");
        f = fopen(TEST_STATE "/starts", "r");
        assert(f && fgetc(f) == '0');
        fclose(f);
    }
    /* a start that lasts clears the count */
    {
        void* a3;
        FILE* f;
        memset(&N, 0, sizeof(N));
        a3 = iAP2WiFiAdapterGet();
        ((init_fn)(*(void***)a3)[0])(a3, (void*)0x1234, (void*)0xc0ffee, air_send);
        f = fopen(TEST_STATE "/starts", "r");
        assert(f && fgetc(f) == '1');
        fclose(f);
        sleep(2);   /* HEALTHY_S is 1 in the test */
        f = fopen(TEST_STATE "/starts", "r");
        assert(f && fgetc(f) == '0');
        fclose(f);
        iAP2WiFiAdapterDispose(a3);
    }
    /* AUTOSTART: the service is started once; while its pid lives, not again; without the flag, not at all */
    {
        FILE* f = fopen(TEST_STATE "/cpd.sh", "w");
        void* a5;
        char b[32] = "";
        int pid1, pid2;
        fprintf(f, "echo $$ > " TEST_STATE "/cpd.pid\nexec sleep 30\n");
        fclose(f);
        unlink(TEST_STATE "/cpd.pid");
        unlink(TEST_STATE "/autostart");
        memset(&N, 0, sizeof(N));
        a5 = iAP2WiFiAdapterGet();
        ((init_fn)(*(void***)a5)[0])(a5, (void*)0x1234, (void*)0xc0ffee, air_send);
        usleep(300000);
        assert(access(TEST_STATE "/cpd.pid", F_OK) != 0);   /* no flag: nothing started */
        iAP2WiFiAdapterDispose(a5);
        f = fopen(TEST_STATE "/autostart", "w");
        fclose(f);
        a5 = iAP2WiFiAdapterGet();
        ((init_fn)(*(void***)a5)[0])(a5, (void*)0x1234, (void*)0xc0ffee, air_send);
        usleep(500000);
        f = fopen(TEST_STATE "/cpd.pid", "r");
        assert(f && fgets(b, sizeof(b), f));
        fclose(f);
        pid1 = atoi(b);
        assert(pid1 > 0 && kill(pid1, 0) == 0);
        assert(getpgid(pid1) == pid1);   /* a process group of its own */
        iAP2WiFiAdapterDispose(a5);
        a5 = iAP2WiFiAdapterGet();   /* Connectivity restarted: the service still runs, not started again */
        ((init_fn)(*(void***)a5)[0])(a5, (void*)0x1234, (void*)0xc0ffee, air_send);
        usleep(500000);
        f = fopen(TEST_STATE "/cpd.pid", "r");
        assert(f && fgets(b, sizeof(b), f));
        fclose(f);
        pid2 = atoi(b);
        assert(pid2 == pid1);
        iAP2WiFiAdapterDispose(a5);
        kill(pid1, SIGTERM);
        waitpid(pid1, 0, 0);
        unlink(TEST_STATE "/autostart");
        unlink(TEST_STATE "/cpd.pid");
    }
    printf("adapter host test: all checks passed\n");
    return 0;
}
