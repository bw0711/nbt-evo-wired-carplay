/* Mac test: the controller server answers GET /ctrl-int/1/connect as the factory plug-in sends it. GPL-3.0-only
 * From nbt-carplay-evo/: clang -std=gnu99 -g -fsanitize=address,undefined -w -Isrc test-host/ctrl_server.c \
 *   -o test-host/ctrl_server && ASAN_OPTIONS=detect_leaks=0 ./test-host/ctrl_server */
#include <stdarg.h>
#include <time.h>
#include "../src/cp_ctrl.c"
void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); fflush(stdout); }
long long ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
int main(void)
{
    static const unsigned char lo[4] = {127, 0, 0, 1};
    const unsigned port = ctrl_start(lo);
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "printf 'GET /ctrl-int/1/connect HTTP/1.1\\r\\nHost: x\\r\\nUser-Agent: AirPlay/280.33.8\\r\\nAirPlay-Receiver-Device-ID: 123\\r\\n\\r\\nGET /ctrl-int/1/other HTTP/1.1\\r\\nContent-Length: 3\\r\\n\\r\\nabc' | nc -w 2 127.0.0.1 %u", port);
    printf("nc says:\n");
    fflush(stdout);
    system(cmd);
    printf("\nconnected: %d\n", ctrl_connected());
    ctrl_stop();
    return ctrl_connected() ? 0 : 1;
}
