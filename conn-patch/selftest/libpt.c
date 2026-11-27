/* A stand-in for libconn_carplay.so in the selftest: the selftest table patches answer2()'s "mov r0, #1" to "#2". */
__attribute__((noinline, used)) int answer2(void)
{
    __asm__ volatile("" ::: "memory");
    return 1;
}
