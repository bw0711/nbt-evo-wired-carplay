/* fake_acpd: test-only stand-in for the NBT Evo's acpd, for IDEmu (which has no acpd and no Apple chip).
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Creates the shared chip lock the way the head unit's acpd does (read from acpd and libipod-acp-har-i2c-iap2.so,
 * 2026-11-26): shared-memory object /dev/shm_acp_lock, 12 bytes, {int ready = 1; pthread_mutex_t mutex}, the mutex
 * process-shared. Then nbt_carplay_probe_evo's locking can be tried against it.
 *
 *   fake_acpd create           create and initialise it (it stays until "remove" or a reboot)
 *   fake_acpd hold SECONDS     take the mutex, keep it SECONDS, release it
 *   fake_acpd remove           shm_unlink it
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SHM "/dev/shm_acp_lock"

static volatile unsigned char* map(int create)
{
    int fd = shm_open(SHM, O_RDWR | (create ? O_CREAT : 0), 0777);
    void* p;
    if (fd < 0) {
        printf("fake_acpd: shm_open %s: %s\n", SHM, strerror(errno));
        return 0;
    }
    if (create && ftruncate(fd, 12) != 0) {
        printf("fake_acpd: ftruncate: %s\n", strerror(errno));
        close(fd);
        return 0;
    }
    p = mmap(0, 12, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        printf("fake_acpd: mmap: %s\n", strerror(errno));
        return 0;
    }
    return (volatile unsigned char*)p;
}

int main(int argc, char** argv)
{
    volatile unsigned char* m;
    int rc;
    setvbuf(stdout, 0, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "create")) {
        pthread_mutexattr_t a;
        if (!(m = map(1)))
            return 1;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
        rc = pthread_mutex_init((pthread_mutex_t*)(m + 4), &a);
        *(volatile int*)m = 1;
        printf("fake_acpd: %s created, mutex init %s\n", SHM, rc ? strerror(rc) : "OK");
        return rc ? 1 : 0;
    }
    if (argc == 3 && !strcmp(argv[1], "hold")) {
        if (!(m = map(0)))
            return 1;
        rc = pthread_mutex_lock((pthread_mutex_t*)(m + 4));
        printf("fake_acpd: lock %s, holding %s s\n", rc ? strerror(rc) : "OK", argv[2]);
        sleep((unsigned)atoi(argv[2]));
        pthread_mutex_unlock((pthread_mutex_t*)(m + 4));
        printf("fake_acpd: released\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "remove")) {
        rc = shm_unlink(SHM);
        printf("fake_acpd: shm_unlink %s\n", rc ? strerror(errno) : "OK");
        return rc ? 1 : 0;
    }
    fprintf(stderr, "usage: fake_acpd create | hold SECONDS | remove\n");
    return 2;
}
