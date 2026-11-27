/* A stand-in for Connectivity when testing conn_patch on a QNX guest: prints answer() every 300 ms.
 * The selftest table patches answer()'s "mov r0, #1" to "mov r0, #2" in the running process. */
#include <dlfcn.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

__attribute__((noinline, used)) int answer(void)
{
    __asm__ volatile("" ::: "memory");
    return 1;
}

/* argv[1]: libpt.so (answer2, the library patch t2); without it only answer() */
int main(int argc, char** argv)
{
    int last = -1, last2 = -1, n = 0;
    void* h = argc > 1 ? dlopen(argv[1], RTLD_NOW) : 0;
    int (*answer2)(void) = h ? (int (*)(void))dlsym(h, "answer2") : 0;
    if (argc > 1 && !answer2)
        printf("patch_target: no answer2 in %s\n", argv[1]);
    for (;;) {
        const int a = answer(), b = answer2 ? answer2() : -1;
        if (a != last || b != last2 || ++n % 20 == 0) {
            printf("patch_target: answer() = %d, answer2() = %d\n", a, b);
            fflush(stdout);
            last = a;
            last2 = b;
        }
        usleep(300000);
    }
}
