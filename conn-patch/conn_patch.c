/* conn_patch: RAM-only patches of the head unit's running Connectivity (NBT Evo, NBTevo_N18032I) for wired CarPlay.
 *
 *   conn_patch [--pid N | --file PATH] check [c1 c2 ...]
 *   conn_patch [--pid N | --file PATH] apply|revert c1 [c2 ...]
 *
 * On the head unit it patches the running process in RAM: it reads the process through /proc/<pid>/as and writes into
 * the physical page behind the process's code page (see phys_write), with the process stopped for the moment of the
 * write. Nothing on flash changes; a reboot (or `revert`) restores the factory code. --file works on a copy of the
 * binary instead (tests on a Mac).
 *
 * Fail closed: every patch carries the whole function around it (conn_patch_tab.h, from the car's own binary). A patch
 * is written only if every byte of that function matches, with the patch site holding the original bytes for apply or
 * the new bytes for revert. If any selected patch is in an unknown state, nothing is written at all. After a write the
 * function is read back; if it does not read as expected, the original bytes are written back.
 *
 * Patches and why: wired-carplay/factory-like-device-list-re-2026-11-26.md (section 4c) and RESULT notes.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __QNXNTO__
#include <devctl.h>
#include <limits.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/procfs.h>
#endif

struct patch {
    const char* name;
    const char* what;
    unsigned ctx_va, ctx_len;      /* the function around the site, as in the factory binary */
    const unsigned char* ctx;
    unsigned site_va, site_len;    /* the bytes that change */
    const unsigned char* site_new;
    const char* module;            /* 0: the executable; else the shared library (file name) it is in */
};
#ifdef CONN_PATCH_SELFTEST
#include "selftest_tab.h"           /* patches selftest/patch_target, the stand-in process for a QNX guest */
#define CONN_PATH "/patch_target"
__attribute__((unused)) static int path_ok(const char* p)
{
    const size_t n = strlen(p), k = strlen(CONN_PATH);
    return n >= k && !strcmp(p + n - k, CONN_PATH);
}
#else
#include "conn_patch_tab.h"
#define CONN_PATH "/opt/conn/bin/Connectivity"
__attribute__((unused)) static int path_ok(const char* p)
{
    /* procfs gives the path without its leading slash (seen in the IDEmu guest: "fs/shim/carplug/_tools/..."), and on
     * the car it may be the resolved one (".../repository/istep/opt/conn/bin/Connectivity"; pidin shows only its end).
     * Any path ending in /conn/bin/Connectivity counts: what decides is the byte check of every patched function. */
    static const char tail[] = "/conn/bin/Connectivity";
    const size_t n = strlen(p), k = sizeof(tail) - 1;
    return !strcmp(p + (p[0] == '/'), &CONN_PATH[1]) || (n >= k && !strcmp(p + n - k, tail));
}
#endif

#define NPATCH (sizeof(g_patches) / sizeof(g_patches[0]))

enum { ST_ORIG, ST_NEW, ST_UNKNOWN };
static const char* const st_name[] = {"original", "patched", "UNKNOWN"};

struct target {
    int fd;
    int file;                       /* 1: a copy of the binary, vaddr -> file offset through its LOAD segments */
    unsigned nseg, seg_va[8], seg_off[8], seg_len[8];
    unsigned base;                  /* in a process: where the module's address 0 is (0 for the executable) */
};

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("conn_patch: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static unsigned le32(const unsigned char* p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static int full_read(int fd, unsigned char* b, unsigned n)
{
    while (n) {
        const ssize_t k = read(fd, b, n);
        if (k <= 0)
            return -1;
        b += k;
        n -= (unsigned)k;
    }
    return 0;
}

static int full_write(int fd, const unsigned char* b, unsigned n)
{
    while (n) {
        const ssize_t k = write(fd, b, n);
        if (k <= 0)
            return -1;
        b += k;
        n -= (unsigned)k;
    }
    return 0;
}

/* a copy of the binary: its LOAD segments */
static int file_map(struct target* t)
{
    unsigned char h[52], ph[32];
    unsigned i, phoff, phentsize, phnum;
    if (lseek(t->fd, 0, SEEK_SET) != 0 || full_read(t->fd, h, sizeof(h)) || memcmp(h, "\177ELF\1\1", 6))
        return -1;
    phoff = le32(h + 0x1c);
    phentsize = h[0x2a] | h[0x2b] << 8;
    phnum = h[0x2c] | h[0x2d] << 8;
    if (phentsize < sizeof(ph))
        return -1;
    for (i = 0; i < phnum; i++) {
        if (lseek(t->fd, (off_t)(phoff + i * phentsize), SEEK_SET) < 0 || full_read(t->fd, ph, sizeof(ph)))
            return -1;
        if (le32(ph) == 1 && t->nseg < 8) {
            t->seg_off[t->nseg] = le32(ph + 4);
            t->seg_va[t->nseg] = le32(ph + 8);
            t->seg_len[t->nseg] = le32(ph + 16);
            t->nseg++;
        }
    }
    return t->nseg ? 0 : -1;
}

static int seek_va(struct target* t, unsigned va, unsigned n)
{
    unsigned i;
    if (!t->file)
        return lseek(t->fd, (off_t)va, SEEK_SET) == (off_t)va ? 0 : -1;
    for (i = 0; i < t->nseg; i++)
        if (t->seg_va[i] <= va && va - t->seg_va[i] + n <= t->seg_len[i])
            return lseek(t->fd, (off_t)(t->seg_off[i] + va - t->seg_va[i]), SEEK_SET) < 0 ? -1 : 0;
    return -1;
}

static int read_va(struct target* t, unsigned va, unsigned char* b, unsigned n)
{
    return seek_va(t, va, n) || full_read(t->fd, b, n) ? -1 : 0;
}

#ifdef __QNXNTO__
/* the physical address of the byte at va in the target, from its page table; 0 if not mapped */
static uint64_t target_paddr(int fd, unsigned va)
{
    procfs_mapinfo one, *m;
    uint64_t pa = 0;
    int n = 0, i;
    memset(&one, 0, sizeof(one));
    one.vaddr = va;
    if (devctl(fd, DCMD_PROC_PTINFO, &one, sizeof(one), &n) != EOK || n <= 0 || n > 1 << 20)
        return 0;
    m = calloc((size_t)n, sizeof(*m));
    if (!m)
        return 0;
    m[0].vaddr = va;
    if (devctl(fd, DCMD_PROC_PTINFO, m, sizeof(*m) * (size_t)n, &n) == EOK)
        for (i = 0; i < n; i++)
            if (m[i].vaddr <= va && va - m[i].vaddr < m[i].size && m[i].paddr) {
                pa = m[i].paddr + (va - m[i].vaddr);
                break;
            }
    free(m);
    return pa;
}

/* QNX 6.5 refuses writes to a process's read-only code pages through /proc/<pid>/as (EFAULT, IDEmu guest
 * 2026-11-27, also with the process stopped). So the bytes go into the physical page behind the process's page:
 * the whole 4 KiB page is first compared with what the process sees, and nothing is written if they differ. Then the
 * D-cache is cleaned and the I-cache invalidated for that page. The process's code page is RAM of its own: the
 * executable file on flash is not touched. */
static int phys_write(struct target* t, unsigned va, const unsigned char* b, unsigned n)
{
    static unsigned char seen[4096];
    const unsigned page_va = va & ~0xfffu, off = va & 0xfffu;
    const uint64_t pa = target_paddr(t->fd, va);
    unsigned char* p;
    if (!pa || off + n > 4096 || (pa & 0xfffu) != off) {
        say("no physical page for 0x%x", va);
        errno = EFAULT;
        return -1;
    }
    if (read_va(t, page_va, seen, sizeof(seen)))
        return -1;
    p = mmap_device_memory(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, 0, pa & ~(uint64_t)0xfff);
    if (p == MAP_FAILED) {
        say("map physical 0x%llx: %s", (unsigned long long)(pa & ~(uint64_t)0xfff), strerror(errno));
        return -1;
    }
    if (memcmp(p, seen, sizeof(seen))) {
        say("physical 0x%llx does not hold the process's page 0x%x: not written", (unsigned long long)(pa & ~(uint64_t)0xfff),
            page_va);
        munmap(p, 4096);
        errno = EFAULT;
        return -1;
    }
    say("page 0x%x of the process is physical 0x%llx", page_va, (unsigned long long)(pa & ~(uint64_t)0xfff));
    memcpy(p + off, b, n);
    if (msync(p, 4096, MS_SYNC | MS_INVALIDATE_ICACHE))
        say("msync: %s", strerror(errno));
    munmap(p, 4096);
    return 0;
}
#endif

static int write_va(struct target* t, unsigned va, const unsigned char* b, unsigned n)
{
#ifdef __QNXNTO__
    if (!t->file)
        return phys_write(t, va, b, n);
#endif
    return seek_va(t, va, n) || full_write(t->fd, b, n) ? -1 : 0;
}

/* the patch's state in the target; *bad = offset of the first byte that matches neither, for the log */
static int state_of(struct target* t, const struct patch* p, unsigned* bad)
{
    unsigned char cur[512];
    const unsigned s = p->site_va - p->ctx_va;
    unsigned i;
    int orig = 1, neu = 1;
    *bad = 0;
    if (p->ctx_len > sizeof(cur) || read_va(t, t->base + p->ctx_va, cur, p->ctx_len))
        return -1;
    for (i = 0; i < p->ctx_len; i++) {
        const int in_site = i >= s && i < s + p->site_len;
        if (!in_site && cur[i] != p->ctx[i]) {
            *bad = p->ctx_va + i;
            return ST_UNKNOWN;
        }
        if (in_site) {
            orig &= cur[i] == p->ctx[i];
            neu &= cur[i] == p->site_new[i - s];
        }
    }
    if (orig)
        return ST_ORIG;
    if (neu)
        return ST_NEW;
    *bad = p->site_va;
    return ST_UNKNOWN;
}

#ifdef __QNXNTO__
/* the executable of a process, from procfs */
static int proc_path(int fd, char* path, unsigned size)
{
    struct {
        procfs_debuginfo info;
        char buf[PATH_MAX];
    } map;
    memset(&map, 0, sizeof(map));
    if (devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof(map), 0) != EOK)
        return -1;
    snprintf(path, size, "%s", map.info.path);
    return 0;
}

/* A running process's code pages are read-only: a plain write through /proc/<pid>/as fails with EFAULT (IDEmu guest,
 * 2026-11-27). Stop it for the write, the way a debugger does, with run-on-last-close set first so that it runs again
 * even if this tool dies before proc_run. */
static int proc_stop(int fd)
{
    int flag = _DEBUG_FLAG_RLC, e;
    procfs_status st;
    if ((e = devctl(fd, DCMD_PROC_SET_FLAG, &flag, sizeof(flag), 0)) != EOK) {
        say("run-on-last-close: %s; not stopping the process", strerror(e));
        return -1;
    }
    memset(&st, 0, sizeof(st));
    if ((e = devctl(fd, DCMD_PROC_STOP, &st, sizeof(st), 0)) != EOK) {
        say("stop: %s", strerror(e));
        return -1;
    }
    return 0;
}

static void proc_run(int fd)
{
    procfs_run r;
    int e;
    memset(&r, 0, sizeof(r));
    if ((e = devctl(fd, DCMD_PROC_RUN, &r, sizeof(r), 0)) != EOK)
        say("run: %s (it runs again when this tool exits)", strerror(e));
}

/* where a shared library of the process is loaded (its address 0): a mapping procfs names after it, minus that mapping's
 * address inside the library; 0 if none */
static unsigned module_base(int fd, const char* module)
{
    enum { MAXMAP = 1024 };
    procfs_mapinfo* m = calloc(MAXMAP, sizeof(*m));
    int n = 0, i;
    unsigned base = 0;
    const size_t k = strlen(module);
    if (!m)
        return 0;
    if (devctl(fd, DCMD_PROC_MAPINFO, m, MAXMAP * sizeof(*m), &n) == EOK) {
        if (n > MAXMAP)
            n = MAXMAP;
        for (i = 0; i < n && !base; i++) {
            struct {
                procfs_debuginfo info;
                char buf[PATH_MAX];
            } dbg;
            size_t l;
            memset(&dbg, 0, sizeof(dbg));
            dbg.info.vaddr = m[i].vaddr;
            if (devctl(fd, DCMD_PROC_MAPDEBUG, &dbg, sizeof(dbg), 0) != EOK)
                continue;
            l = strlen(dbg.info.path);
            /* MAPDEBUG's vaddr is the object's own (link-time) address of the mapping's start, not where it is
             * loaded (IDEmu guest 2026-11-27: 0 for the code, 0x1000 for the data of a library at 0x781ec000) */
            if (l >= k && !strcmp(dbg.info.path + l - k, module) && (l == k || dbg.info.path[l - k - 1] == '/'))
                base = (unsigned)(m[i].vaddr - dbg.info.vaddr);
        }
    }
    free(m);
    return base;
}

/* the one running Connectivity; 0 if none or more than one */
static int find_conn(void)
{
    DIR* d = opendir("/proc");
    struct dirent* e;
    int found = 0, n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)) != 0) {
        char as[64], path[PATH_MAX] = "";
        int fd;
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        snprintf(as, sizeof(as), "/proc/%s/as", e->d_name);
        fd = open(as, O_RDONLY);
        if (fd < 0)
            continue;
        if (!proc_path(fd, path, sizeof(path)) && path_ok(path)) {
            found = atoi(e->d_name);
            n++;
        } else if (strstr(path, "Connectivity"))
            say("pid %s runs %s: not taken as the factory Connectivity", e->d_name, path);
        close(fd);
    }
    closedir(d);
    if (n != 1)
        say("%d processes run %s, need exactly one", n, CONN_PATH);
    return n == 1 ? found : 0;
}
#endif

static int open_target(struct target* t, const char* file, int pid, int rw)
{
    memset(t, 0, sizeof(*t));
    if (file) {
        t->file = 1;
        t->fd = open(file, rw ? O_RDWR : O_RDONLY);
        if (t->fd < 0 || file_map(t)) {
            say("%s: not a readable ELF copy (%s)", file, strerror(errno));
            return -1;
        }
        say("target: file %s", file);
        return 0;
    }
#ifdef __QNXNTO__
    {
        char as[64], path[PATH_MAX];
        if (!pid)
            pid = find_conn();
        if (!pid)
            return -1;
        snprintf(as, sizeof(as), "/proc/%d/as", pid);
        t->fd = open(as, rw ? O_RDWR : O_RDONLY);
        if (t->fd < 0) {
            say("%s: %s", as, strerror(errno));
            return -1;
        }
        if (proc_path(t->fd, path, sizeof(path)) || !path_ok(path)) {
            say("pid %d is not %s", pid, CONN_PATH);
            close(t->fd);
            return -1;
        }
        say("target: pid %d (%s), in memory", pid, path);
        return 0;
    }
#else
    (void)pid;
    say("a running process can be patched only on the head unit; use --file here");
    return -1;
#endif
}

static int usage(void)
{
    unsigned i;
    fprintf(stderr, "usage: conn_patch [--pid N | --file PATH [--lib PATH]] check [patch ...]\n"
                    "       conn_patch [--pid N | --file PATH [--lib PATH]] apply|revert patch [patch ...]\n"
                    "  (--file: a copy of the executable, --lib: a copy of the library a patch is in)\n"
                    "patches:\n");
    for (i = 0; i < NPATCH; i++)
        fprintf(stderr, "  %s  %s%s%s\n", g_patches[i].name, g_patches[i].what, g_patches[i].module ? ", in " : "",
                g_patches[i].module ? g_patches[i].module : "");
    return 1;
}

int main(int argc, char** argv)
{
    const char* file = 0;
    const char* lib = 0;
    int pid = 0, a = 1, rw, refuse = 0, rc = 0, need_exe = 0, need_lib = 0, lib_fd_own = 0;
    unsigned i, want[NPATCH], nwant = 0;
    struct target t, tl;            /* the executable; the library (in a process: the same fd, its base) */
    const char* cmd;
    const char* module = 0;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "--file") && a + 1 < argc)
            file = argv[++a];
        else if (!strcmp(argv[a], "--lib") && a + 1 < argc)
            lib = argv[++a];
        else if (!strcmp(argv[a], "--pid") && a + 1 < argc)
            pid = atoi(argv[++a]);
        else
            return usage();
    }
    if (a >= argc)
        return usage();
    cmd = argv[a++];
    if (strcmp(cmd, "check") && strcmp(cmd, "apply") && strcmp(cmd, "revert"))
        return usage();
    for (; a < argc; a++) {
        for (i = 0; i < NPATCH && strcmp(argv[a], g_patches[i].name); i++)
            ;
        if (i == NPATCH) {
            say("no patch \"%s\"", argv[a]);
            return usage();
        }
        {
            unsigned k;
            for (k = 0; k < nwant && want[k] != i; k++)
                ;
            if (k == nwant)
                want[nwant++] = i;      /* a name given twice counts once */
        }
    }
    rw = strcmp(cmd, "check") != 0;
    if (rw && !nwant)
        return usage();             /* writes only what was named */
    if (!nwant)                     /* check: all (on copies, those whose file was given) */
        for (i = 0; i < NPATCH; i++)
            if (!(file || lib) || (g_patches[i].module ? lib != 0 : file != 0))
                want[nwant++] = i;
    for (i = 0; i < nwant; i++) {
        if (g_patches[want[i]].module) {
            need_lib = 1;
            module = g_patches[want[i]].module;
        } else
            need_exe = 1;
    }
    memset(&t, 0, sizeof(t));
    memset(&tl, 0, sizeof(tl));
    t.fd = tl.fd = -1;
    if (file || lib) {              /* copies */
        if ((need_exe && !file) || (need_lib && !lib)) {
            say("a patch in %s needs %s", need_lib && !lib ? "a library" : "the executable", need_lib && !lib ? "--lib" : "--file");
            return 1;
        }
        if (need_exe && open_target(&t, file, 0, rw))
            return 1;
        if (need_lib) {
            if (open_target(&tl, lib, 0, rw))
                return 1;
            lib_fd_own = 1;
        }
    } else {
        if (open_target(&t, 0, pid, rw))
            return 1;
        tl = t;
#ifndef __QNXNTO__
        (void)module;
#else
        if (need_lib) {
            tl.base = module_base(t.fd, module);
            if (tl.base)
                say("%s is loaded at 0x%x", module, tl.base);
            else
                say("%s is not loaded in this process", module);
        }
#endif
    }

    /* 1. every selected patch must be in a known state before anything is written */
    for (i = 0; i < nwant; i++) {
        const struct patch* p = &g_patches[want[i]];
        struct target* tp = p->module ? &tl : &t;
        unsigned bad;
        const int st = p->module && !tl.file && !tl.base ? -1 : state_of(tp, p, &bad);
        if (st < 0) {
            say("%s: cannot read 0x%x..0x%x (%s)", p->name, p->ctx_va, p->ctx_va + p->ctx_len,
                p->module && !tl.file && !tl.base ? "library not loaded" : strerror(errno));
            refuse = 1;
        } else if (st == ST_UNKNOWN) {
            say("%s: UNKNOWN code (first difference at 0x%x): not the binary these patches are for", p->name, bad);
            refuse = 1;
        } else
            say("%s: %s  (%s)", p->name, st_name[st], p->what);
    }
    if (!rw || refuse) {
        if (refuse && rw)
            say("nothing written");
        if (t.fd >= 0)
            close(t.fd);
        if (lib_fd_own)
            close(tl.fd);
        return refuse ? 2 : 0;
    }

    /* 2. write, read back, undo a write that does not read back; the process stands still meanwhile */
#ifdef __QNXNTO__
    if (!t.file && t.fd >= 0 && proc_stop(t.fd)) {
        close(t.fd);
        return 3;
    }
#endif
    for (i = 0; i < nwant; i++) {
        const struct patch* p = &g_patches[want[i]];
        struct target* tp = p->module ? &tl : &t;
        const int to = !strcmp(cmd, "apply") ? ST_NEW : ST_ORIG;
        const unsigned char* bytes = to == ST_NEW ? p->site_new : p->ctx + (p->site_va - p->ctx_va);
        unsigned bad;
        if (state_of(tp, p, &bad) == to) {
            say("%s: already %s, not written", p->name, st_name[to]);
            continue;
        }
        if (write_va(tp, tp->base + p->site_va, bytes, p->site_len)) {
            say("%s: write at 0x%x failed (%s)", p->name, p->site_va, strerror(errno));
            rc = 3;
            break;
        }
        if (state_of(tp, p, &bad) != to) {
            say("%s: does not read back as %s (0x%x); writing the original bytes back", p->name, st_name[to], bad);
            write_va(tp, tp->base + p->site_va, p->ctx + (p->site_va - p->ctx_va), p->site_len);
            say("%s: now %s", p->name, st_name[state_of(tp, p, &bad) == ST_ORIG ? ST_ORIG : ST_UNKNOWN]);
            rc = 3;
            break;
        }
        say("%s: %s at 0x%x (%u bytes)", p->name, st_name[to], p->site_va, p->site_len);
    }
#ifdef __QNXNTO__
    if (!t.file && t.fd >= 0)
        proc_run(t.fd);
#endif
    if (t.fd >= 0)
        close(t.fd);
    if (lib_fd_own)
        close(tl.fd);
    return rc;
}
