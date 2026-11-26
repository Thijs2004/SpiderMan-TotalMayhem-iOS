// bionic (Android libc, arm64) on top of Darwin: the ~450 functions libspiderman.so imports.
// Same-ABI functions are handed out directly; everything whose structs, flags or calling convention differ
// is wrapped here (pthread objects, stat/dirent/statfs/utsname, open/mmap/fcntl flags, clocks, sysconf,
// stdio streams, AAPCS variadics and va_lists, semaphores, dl*).
#include "bionic.h"
#include "elfload.h"
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <mach/mach.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <wchar.h>
#include <wctype.h>
#include <xlocale.h>
#include <arpa/inet.h>

#define LOG elf_log

// ---------------------------------------------------------------- AAPCS va_list
typedef struct { char *stack; char *gr_top; char *vr_top; int gr_offs; int vr_offs; } AVa;

static uint64_t va_gp(AVa *va) {
    if (va->gr_offs < 0) { uint64_t v = *(uint64_t *)(va->gr_top + va->gr_offs); va->gr_offs += 8; return v; }
    uint64_t v = *(uint64_t *)va->stack; va->stack += 8; return v;
}
static double va_fp(AVa *va) {
    if (va->vr_offs < 0) { double v = *(double *)(va->vr_top + va->vr_offs); va->vr_offs += 16; return v; }
    double v = *(double *)va->stack; va->stack += 8; return v;
}
// binary128 long double (AArch64 Linux) -> double
static double quad_to_double(const uint64_t q[2]) {
    uint64_t lo = q[0], hi = q[1];
    int sign = (int)(hi >> 63);
    int exp = (int)((hi >> 48) & 0x7fff);
    uint64_t mant = ((hi & 0xffffffffffffULL) << 4) | (lo >> 60);   // top 52 of 112 mantissa bits
    uint64_t bits;
    if (exp == 0) bits = 0;
    else if (exp == 0x7fff) bits = 0x7ffULL << 52 | (mant ? 1 : 0);
    else {
        int e = exp - 16383 + 1023;
        if (e >= 0x7ff) bits = 0x7ffULL << 52;
        else if (e <= 0) bits = 0;
        else bits = (uint64_t)e << 52 | mant;
    }
    bits |= (uint64_t)sign << 63;
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
static void double_to_quad(double d, uint64_t q[2]) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    uint64_t sign = bits >> 63, exp = (bits >> 52) & 0x7ff, mant = bits & 0xfffffffffffffULL;
    uint64_t qe = exp == 0 ? 0 : exp == 0x7ff ? 0x7fff : exp - 1023 + 16383;
    q[1] = sign << 63 | qe << 48 | mant >> 4;
    q[0] = mant << 60;
}
static double va_ldouble(AVa *va) {
    uint64_t q[2];
    if (va->vr_offs < 0) { memcpy(q, va->vr_top + va->vr_offs, 16); va->vr_offs += 16; }
    else { va->stack = (char *)(((uintptr_t)va->stack + 15) & ~15ULL); memcpy(q, va->stack, 16); va->stack += 16; }
    return quad_to_double(q);
}

// Reads the arguments a printf format consumes from an AAPCS va_list into Darwin's va_list layout
// (consecutive 8-byte slots).
static int printf_slots(const char *f, AVa *va, uint64_t *slots, int max) {
    int n = 0;
    for (; *f; f++) {
        if (*f != '%') continue;
        f++;
        if (*f == '%') continue;
        while (*f && strchr("-+ #0'", *f)) f++;
        if (*f == '*') { if (n < max) slots[n++] = (uint32_t)va_gp(va); f++; } else while (isdigit((unsigned char)*f)) f++;
        if (*f == '.') { f++; if (*f == '*') { if (n < max) slots[n++] = (uint32_t)va_gp(va); f++; } else while (isdigit((unsigned char)*f)) f++; }
        int L = 0;
        while (*f && strchr("hlLqjzt", *f)) { if (*f == 'L') L = 1; f++; }
        if (!*f) break;
        if (n >= max) break;
        if (strchr("aAeEfFgG", *f)) {
            double d = L ? va_ldouble(va) : va_fp(va);
            memcpy(&slots[n++], &d, 8);
        } else slots[n++] = va_gp(va);
    }
    return n;
}

int bionic_vsnprintf(char *buf, size_t n, const char *fmt, AVa *va) {
    uint64_t slots[64];
    AVa copy = *va;
    printf_slots(fmt, &copy, slots, 64);
    return vsnprintf(buf, n, fmt, (va_list)slots);
}

static int scanf_slots(const char *f, AVa *va, uint64_t *slots, int max) {
    int n = 0;
    for (; *f; f++) {
        if (*f != '%') continue;
        f++;
        if (*f == '%') continue;
        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }
        while (isdigit((unsigned char)*f)) f++;
        while (*f && strchr("hlLqjzt", *f)) f++;
        if (*f == '[') { f++; if (*f == ']') f++; while (*f && *f != ']') f++; }
        if (!*f) break;
        if (!suppress && n < max) slots[n++] = va_gp(va);
    }
    return n;
}

// ---------------------------------------------------------------- stdio streams
// bionic's __sF[3] (old stdin/stdout/stderr) are fake FILEs mapped onto Darwin's; stdin/stdout/stderr
// variables hold the real ones.
static char g_sF[3][152];
static FILE *g_stdin, *g_stdout, *g_stderr;
static FILE *F(FILE *f) {
    if ((char *)f >= g_sF[0] && (char *)f < g_sF[0] + sizeof g_sF) return ((char *)f - g_sF[0]) / 152 == 0 ? stdin : ((char *)f - g_sF[0]) / 152 == 1 ? stdout : stderr;
    return f;
}
static int b_fclose(FILE *f) { return F(f) == f ? fclose(f) : 0; }
static int b_fflush(FILE *f) { return fflush(f ? F(f) : NULL); }
static int b_fgetc(FILE *f) { return fgetc(F(f)); }
static char *b_fgets(char *s, int n, FILE *f) { return fgets(s, n, F(f)); }
static wint_t b_fgetwc(FILE *f) { return fgetwc(F(f)); }
static int b_fputc(int c, FILE *f) { return fputc(c, F(f)); }
static int b_fputs(const char *s, FILE *f) { return fputs(s, F(f)); }
static wint_t b_fputwc(wchar_t c, FILE *f) { return fputwc(c, F(f)); }
static size_t b_fread(void *p, size_t a, size_t b, FILE *f) { return fread(p, a, b, F(f)); }
static size_t b_fwrite(const void *p, size_t a, size_t b, FILE *f) { return fwrite(p, a, b, F(f)); }
static int b_fseek(FILE *f, long o, int w) { return fseek(F(f), o, w); }
static long b_ftell(FILE *f) { return ftell(F(f)); }
static void b_rewind(FILE *f) { rewind(F(f)); }
static void b_clearerr(FILE *f) { clearerr(F(f)); }
static int b_ungetc(int c, FILE *f) { return ungetc(c, F(f)); }
static wint_t b_ungetwc(wint_t c, FILE *f) { return ungetwc(c, F(f)); }
static int b_vfprintf(FILE *f, const char *fmt, AVa *va) {
    char buf[4096];
    int r = bionic_vsnprintf(buf, sizeof buf, fmt, va);
    fputs(buf, F(f));
    if (F(f) == stdout || F(f) == stderr) LOG("stdio: %s", buf);
    return r;
}
int aimpl_fprintf(FILE *f, const char *fmt, AVa *va) { return b_vfprintf(f, fmt, va); }
int aimpl_printf(const char *fmt, AVa *va) { return b_vfprintf(stdout, fmt, va); }
int aimpl_snprintf(char *b, size_t n, const char *fmt, AVa *va) { return bionic_vsnprintf(b, n, fmt, va); }
int aimpl_sprintf(char *b, const char *fmt, AVa *va) { return bionic_vsnprintf(b, 1 << 20, fmt, va); }
static int b_vsnprintf(char *b, size_t n, const char *fmt, AVa *va) { return bionic_vsnprintf(b, n, fmt, va); }
static int b___vsnprintf_chk(char *b, size_t n, int flags, size_t bos, const char *fmt, AVa *va) { (void)flags; (void)bos; return bionic_vsnprintf(b, n, fmt, va); }
static int b_vasprintf(char **out, const char *fmt, AVa *va) {
    AVa c = *va;
    int n = bionic_vsnprintf(NULL, 0, fmt, &c);
    *out = malloc(n + 1);
    return bionic_vsnprintf(*out, n + 1, fmt, va);
}
static int b_vsscanf(const char *s, const char *fmt, AVa *va) {
    uint64_t slots[64];
    scanf_slots(fmt, va, slots, 64);
    return vsscanf(s, fmt, (va_list)slots);
}
int aimpl_sscanf(const char *s, const char *fmt, AVa *va) { return b_vsscanf(s, fmt, va); }
static int b_putchar(int c) { return putchar(c); }
static int b_puts(const char *s) { LOG("stdio: %s", s); return puts(s); }

// ---------------------------------------------------------------- android log
static void android_log(int prio, const char *tag, const char *msg) {
    static const char *p = "??VDIWEF";
    LOG("%c/%s: %s", p[prio & 7], tag ? tag : "", msg ? msg : "");
}
static int b___android_log_write(int prio, const char *tag, const char *msg) { android_log(prio, tag, msg); return 1; }
int aimpl_android_log_print(int prio, const char *tag, const char *fmt, AVa *va) {
    char buf[2048];
    bionic_vsnprintf(buf, sizeof buf, fmt, va);
    android_log(prio, tag, buf);
    return 1;
}
static void b_android_set_abort_message(const char *m) { LOG("abort message: %s", m); }
static void b_openlog(const char *a, int b, int c) { (void)a; (void)b; (void)c; }
static void b_closelog(void) {}
int aimpl_syslog(int prio, const char *fmt, AVa *va) {
    char buf[2048];
    bionic_vsnprintf(buf, sizeof buf, fmt, va);
    LOG("syslog %d: %s", prio, buf);
    return 0;
}

// ---------------------------------------------------------------- errno, chk, misc libc
static int *b___errno(void) { return __error(); }
static void *b___memcpy_chk(void *d, const void *s, size_t n, size_t bos) { (void)bos; return memcpy(d, s, n); }
static ssize_t b___read_chk(int fd, void *b, size_t n, size_t bos) { (void)bos; return read(fd, b, n); }
static ssize_t b___pread_chk(int fd, void *b, size_t n, off_t o, size_t bos) { (void)bos; return pread(fd, b, n, o); }
static ssize_t b___write_chk(int fd, const void *b, size_t n, size_t bos) { (void)bos; return write(fd, b, n); }
static char *b___strchr_chk(const char *s, int c, size_t bos) { (void)bos; return strchr(s, c); }
static char *b___strcpy_chk(char *d, const char *s, size_t bos) { (void)bos; return strcpy(d, s); }
static char *b___strncpy_chk2(char *d, const char *s, size_t n, size_t dl, size_t sl) { (void)dl; (void)sl; return strncpy(d, s, n); }
static mode_t b___umask_chk(mode_t m) { return umask(m); }
static void b___stack_chk_fail(void) { LOG("FATAL: stack protector tripped"); abort(); }
static size_t b___ctype_get_mb_cur_max(void) { return MB_CUR_MAX; }
static int b___cxa_atexit(void (*f)(void *), void *a, void *d) { (void)f; (void)a; (void)d; return 0; }
static void b___cxa_finalize(void *d) { (void)d; }
static int b___cxa_thread_atexit_impl(void (*f)(void *), void *o, void *d) { (void)f; (void)o; (void)d; return 0; }
static int b___register_atfork(void (*a)(void), void (*b)(void), void (*c)(void), void *d) { (void)a; (void)b; (void)c; (void)d; return 0; }

// System properties: the runtime reads debug.smhp.* switches.
static int b___system_property_get(const char *name, char *value) {
    const char *v = bionic_property(name);
    strlcpy(value, v ? v : "", 92);
    return (int)strlen(value);
}

static unsigned long b_getauxval(unsigned long type) {
    if (type == 16) return (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8); // AT_HWCAP
    if (type == 6) return 16384;                                                                                // AT_PAGESZ
    return 0;
}

static long b_sysconf(int name) {
    switch (name) {
    case 2: return sysconf(_SC_CLK_TCK);
    case 39: case 40: return getpagesize();      // _SC_PAGESIZE, _SC_PAGE_SIZE
    case 83: return sysconf(_SC_NPROCESSORS_CONF);
    case 84: return sysconf(_SC_NPROCESSORS_ONLN);
    case 85: return sysconf(_SC_PHYS_PAGES);
    case 0: return sysconf(_SC_ARG_MAX);
    case 4: return 1024;                         // _SC_OPEN_MAX
    }
    LOG("sysconf(%d) unknown", name);
    errno = EINVAL;
    return -1;
}

static clockid_t clk(int id) {
    switch (id) {
    case 0: case 5: return CLOCK_REALTIME;
    case 1: case 6: case 7: return CLOCK_MONOTONIC;
    case 4: return CLOCK_MONOTONIC_RAW;
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
    case 3: return CLOCK_THREAD_CPUTIME_ID;
    }
    return CLOCK_MONOTONIC;
}
static int b_clock_gettime(int id, struct timespec *ts) { return clock_gettime(clk(id), ts); }

struct b_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
static int b_uname(struct b_utsname *u) {
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, "Linux");
    strcpy(u->nodename, "localhost");
    strcpy(u->release, "5.10.0");
    strcpy(u->version, "#1 SMP PREEMPT");
    strcpy(u->machine, "aarch64");
    return 0;
}

static pid_t b_gettid(void) { return (pid_t)pthread_mach_thread_np(pthread_self()); }

long aimpl_syscall(long nr, AVa *va) {
    switch (nr) {
    case 178: return b_gettid();                 // gettid
    case 172: return getpid();
    case 98: {                                   // futex: only plain waits/wakes are expected
        LOG("syscall futex called");
        (void)va;
        return 0;
    }
    case 278: {                                  // getrandom(buf, len, flags)
        void *b = (void *)va_gp(va);
        size_t n = va_gp(va);
        arc4random_buf(b, n);
        return (long)n;
    }
    }
    LOG("syscall(%ld) unsupported", nr);
    errno = ENOSYS;
    return -1;
}
int aimpl_prctl(int op, AVa *va) { (void)op; (void)va; return 0; }

// ---------------------------------------------------------------- files
static int oflags(int f) {
    int r = f & 3;
    if (f & 0100) r |= O_CREAT;
    if (f & 0200) r |= O_EXCL;
    if (f & 0400) r |= O_NOCTTY;
    if (f & 01000) r |= O_TRUNC;
    if (f & 02000) r |= O_APPEND;
    if (f & 04000) r |= O_NONBLOCK;
    if (f & 040000) r |= O_DIRECTORY;
    if (f & 0100000) r |= O_NOFOLLOW;
    if (f & 02000000) r |= O_CLOEXEC;
    return r;
}
static int oflags_back(int f) {
    int r = f & 3;
    if (f & O_APPEND) r |= 02000;
    if (f & O_NONBLOCK) r |= 04000;
    return r;
}
int aimpl_open(const char *path, int flags, AVa *va) {
    int mode = (flags & 0100) ? (int)va_gp(va) : 0;
    const char *p = bionic_path(path);
    int fd = open(p, oflags(flags), mode);
    if (fd < 0) LOG("open %s failed (%d)", p, errno);
    return fd;
}
int aimpl_fcntl(int fd, int cmd, AVa *va) {
    switch (cmd) {
    case 1: return fcntl(fd, F_GETFD);
    case 2: return fcntl(fd, F_SETFD, (int)va_gp(va));
    case 3: { int r = fcntl(fd, F_GETFL); return r < 0 ? r : oflags_back(r); }
    case 4: return fcntl(fd, F_SETFL, oflags((int)va_gp(va)));
    case 0: return fcntl(fd, F_DUPFD, (int)va_gp(va));
    case 1030: return fcntl(fd, F_DUPFD_CLOEXEC, (int)va_gp(va));
    }
    LOG("fcntl cmd %d unsupported", cmd);
    errno = EINVAL;
    return -1;
}

struct b_stat {
    uint64_t st_dev, st_ino; uint32_t st_mode, st_nlink, st_uid, st_gid; uint64_t st_rdev, pad1; int64_t st_size;
    int32_t st_blksize, pad2; int64_t st_blocks; struct timespec st_atim, st_mtim, st_ctim; uint32_t unused[2];
};
static void conv_stat(const struct stat *s, struct b_stat *b) {
    memset(b, 0, sizeof *b);
    b->st_dev = (uint64_t)s->st_dev; b->st_ino = s->st_ino; b->st_mode = s->st_mode; b->st_nlink = s->st_nlink;
    b->st_uid = s->st_uid; b->st_gid = s->st_gid; b->st_rdev = (uint64_t)s->st_rdev; b->st_size = s->st_size;
    b->st_blksize = s->st_blksize; b->st_blocks = s->st_blocks;
    b->st_atim = s->st_atimespec; b->st_mtim = s->st_mtimespec; b->st_ctim = s->st_ctimespec;
}
static int b_stat(const char *p, struct b_stat *b) { struct stat s; int r = stat(bionic_path(p), &s); if (!r) conv_stat(&s, b); return r; }
static int b_fstat(int fd, struct b_stat *b) { struct stat s; int r = fstat(fd, &s); if (!r) conv_stat(&s, b); return r; }

struct b_statfs { uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree; int32_t f_fsid[2]; uint64_t f_namelen, f_frsize, f_flags, f_spare[4]; };
static int b_statfs(const char *p, struct b_statfs *b) {
    struct statfs s;
    int r = statfs(bionic_path(p), &s);
    if (r) return r;
    memset(b, 0, sizeof *b);
    b->f_type = 0xef53; b->f_bsize = s.f_bsize; b->f_blocks = s.f_blocks; b->f_bfree = s.f_bfree; b->f_bavail = s.f_bavail;
    b->f_files = s.f_files; b->f_ffree = s.f_ffree; b->f_namelen = 255; b->f_frsize = s.f_bsize;
    return 0;
}

struct b_dirent { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; };
typedef struct { DIR *dir; struct b_dirent ent; } BDir;
static BDir *b_opendir(const char *p) {
    DIR *d = opendir(bionic_path(p));
    if (!d) return NULL;
    BDir *b = calloc(1, sizeof *b);
    b->dir = d;
    return b;
}
static struct b_dirent *b_readdir(BDir *b) {
    struct dirent *e = readdir(b->dir);
    if (!e) return NULL;
    b->ent.d_ino = e->d_ino; b->ent.d_off = 0; b->ent.d_reclen = sizeof b->ent; b->ent.d_type = e->d_type;
    strlcpy(b->ent.d_name, e->d_name, sizeof b->ent.d_name);
    return &b->ent;
}
static int b_closedir(BDir *b) { int r = closedir(b->dir); free(b); return r; }

static FILE *b_fopen(const char *p, const char *m) {
    FILE *f = fopen(bionic_path(p), m);
    if (!f) LOG("fopen %s (%s) failed (%d)", bionic_path(p), m, errno);
    return f;
}
static int b_access(const char *p, int m) { return access(bionic_path(p), m); }
static int b_mkdir(const char *p, mode_t m) { return mkdir(bionic_path(p), m); }
static int b_rmdir(const char *p) { return rmdir(bionic_path(p)); }
static int b_unlink(const char *p) { return unlink(bionic_path(p)); }
static int b_remove(const char *p) { return remove(bionic_path(p)); }
static int b_rename(const char *a, const char *b) { char pa[1024]; strlcpy(pa, bionic_path(a), sizeof pa); return rename(pa, bionic_path(b)); }
static int b_chdir(const char *p) { return chdir(bionic_path(p)); }
static int b_chmod(const char *p, mode_t m) { return chmod(bionic_path(p), m); }
static int b_utime(const char *p, const struct utimbuf *t) { return utime(bionic_path(p), t); }
static ssize_t b_sendfile(int out, int in, off_t *off, size_t n) {
    char buf[65536];
    size_t done = 0;
    if (off) lseek(in, *off, SEEK_SET);
    while (done < n) {
        ssize_t r = read(in, buf, n - done < sizeof buf ? n - done : sizeof buf);
        if (r <= 0) break;
        if (write(out, buf, r) != r) break;
        done += r;
    }
    if (off) *off += done;
    return (ssize_t)done;
}

// ---------------------------------------------------------------- memory
static void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    int f = flags & 3;
    if (flags & 0x10) f |= MAP_FIXED;
    if (flags & 0x20) f |= MAP_ANON;
    if (flags & 0x4000) f |= MAP_NORESERVE;
    if (flags & 0x100000) f |= MAP_FIXED;      // MAP_FIXED_NOREPLACE
    if (prot & PROT_EXEC) {
        // dynarmic's code cache. iOS has no JIT: hand out plain memory. The recompiled engine covers
        // 19089 of 19096 guest functions; reaching the JIT would fault here and show in the log.
        LOG("mmap: executable %#zx bytes requested (JIT code cache), giving read/write memory", len);
        prot &= ~PROT_EXEC;
    }
    void *r = mmap(addr, len, prot, f, (flags & 0x20) ? -1 : fd, off);
    if (r == MAP_FAILED) LOG("mmap(%p, %#zx, prot %d, flags %#x) failed (%d)", addr, len, prot, flags, errno);
    return r;
}
static int b_mprotect(void *a, size_t l, int p) {
    if (p & PROT_EXEC) { LOG("mprotect: exec on %p (%#zx) refused, kept read/write", a, l); p = PROT_READ | PROT_WRITE; }
    int r = mprotect(a, l, p);
    if (r) LOG("mprotect(%p, %#zx, %d) failed (%d)", a, l, p, errno);
    return r;
}

// ---------------------------------------------------------------- pthread
// bionic objects are smaller than Darwin's; each holds a pointer (at offset 8) to a heap Darwin object,
// created on first use so zero-initialised statics keep working. Word 0 keeps bionic's static-initializer
// type bits (recursive = 0x4000, errorcheck = 0x8000).
typedef struct { uint32_t type; uint32_t pad; _Atomic(void *) real; } BObj;

static pthread_mutex_t *mx(void *m) {
    BObj *o = m;
    pthread_mutex_t *r = atomic_load(&o->real);
    if (r) return r;
    pthread_mutex_t *n = malloc(sizeof *n);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    uint32_t t = (o->type >> 14) & 3;
    pthread_mutexattr_settype(&a, t == 1 ? PTHREAD_MUTEX_RECURSIVE : t == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(n, &a);
    pthread_mutexattr_destroy(&a);
    void *expected = NULL;
    if (atomic_compare_exchange_strong(&o->real, &expected, n)) return n;
    pthread_mutex_destroy(n);
    free(n);
    return expected;
}
static int b_pthread_mutexattr_init(long *a) { *a = 0; return 0; }
static int b_pthread_mutexattr_destroy(long *a) { (void)a; return 0; }
static int b_pthread_mutexattr_settype(long *a, int t) { *a = t; return 0; }
static int b_pthread_mutex_init(void *m, const long *a) {
    memset(m, 0, 40);
    ((BObj *)m)->type = (uint32_t)((a ? *a & 3 : 0) << 14);
    mx(m);
    return 0;
}
static int b_pthread_mutex_destroy(void *m) {
    BObj *o = m;
    pthread_mutex_t *r = atomic_exchange(&o->real, NULL);
    if (r) { pthread_mutex_destroy(r); free(r); }
    return 0;
}
static int b_pthread_mutex_lock(void *m) { return pthread_mutex_lock(mx(m)); }
static int b_pthread_mutex_trylock(void *m) { return pthread_mutex_trylock(mx(m)); }
static int b_pthread_mutex_unlock(void *m) { return pthread_mutex_unlock(mx(m)); }

static pthread_cond_t *cv(void *c) {
    BObj *o = c;
    pthread_cond_t *r = atomic_load(&o->real);
    if (r) return r;
    pthread_cond_t *n = malloc(sizeof *n);
    pthread_cond_init(n, NULL);
    void *expected = NULL;
    if (atomic_compare_exchange_strong(&o->real, &expected, n)) return n;
    pthread_cond_destroy(n);
    free(n);
    return expected;
}
static int b_pthread_cond_init(void *c, const void *a) { (void)a; memset(c, 0, 48); cv(c); return 0; }
static int b_pthread_cond_broadcast(void *c) { return pthread_cond_broadcast(cv(c)); }
static int b_pthread_cond_signal(void *c) { return pthread_cond_signal(cv(c)); }
static int b_pthread_cond_wait(void *c, void *m) { return pthread_cond_wait(cv(c), mx(m)); }
static int b_pthread_cond_timedwait(void *c, void *m, const struct timespec *t) {
    int r = pthread_cond_timedwait(cv(c), mx(m), t);
    return r == ETIMEDOUT ? 110 : r;
}

static pthread_rwlock_t *rw(void *l) {
    BObj *o = l;
    pthread_rwlock_t *r = atomic_load(&o->real);
    if (r) return r;
    pthread_rwlock_t *n = malloc(sizeof *n);
    pthread_rwlock_init(n, NULL);
    void *expected = NULL;
    if (atomic_compare_exchange_strong(&o->real, &expected, n)) return n;
    pthread_rwlock_destroy(n);
    free(n);
    return expected;
}
static int b_pthread_rwlock_rdlock(void *l) { return pthread_rwlock_rdlock(rw(l)); }
static int b_pthread_rwlock_wrlock(void *l) { return pthread_rwlock_wrlock(rw(l)); }
static int b_pthread_rwlock_unlock(void *l) { return pthread_rwlock_unlock(rw(l)); }

static int b_pthread_once(_Atomic int *once, void (*fn)(void)) {
    int s = atomic_load(once);
    if (s == 2) return 0;
    int expected = 0;
    if (atomic_compare_exchange_strong(once, &expected, 1)) {
        fn();
        atomic_store(once, 2);
        return 0;
    }
    while (atomic_load(once) != 2) sched_yield();
    return 0;
}

static int b_pthread_key_create(int *k, void (*d)(void *)) { pthread_key_t key; int r = pthread_key_create(&key, d); *k = (int)key; return r; }
static int b_pthread_key_delete(int k) { return pthread_key_delete((pthread_key_t)k); }
static void *b_pthread_getspecific(int k) { return pthread_getspecific((pthread_key_t)k); }
static int b_pthread_setspecific(int k, const void *v) { return pthread_setspecific((pthread_key_t)k, v); }

typedef struct { uint32_t flags; void *stack_base; size_t stack_size, guard_size; int32_t policy, priority; char reserved[16]; } BAttr;
static int b_pthread_attr_init(BAttr *a) { memset(a, 0, sizeof *a); a->stack_size = 1 << 20; return 0; }
static int b_pthread_attr_destroy(BAttr *a) { (void)a; return 0; }
static int b_pthread_attr_setdetachstate(BAttr *a, int s) { if (s == 1) a->flags |= 1; else a->flags &= ~1u; return 0; }
static int b_pthread_attr_setstacksize(BAttr *a, size_t s) { a->stack_size = s; return 0; }

typedef struct { void *(*fn)(void *); void *arg; } Start;
static void *thread_start(void *p) {
    Start s = *(Start *)p;
    free(p);
    elf_tls_thread_init();
    return s.fn(s.arg);
}
static int b_pthread_create(pthread_t *t, const BAttr *ba, void *(*fn)(void *), void *arg) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    size_t stack = ba && ba->stack_size ? ba->stack_size : (1 << 20);
    if (stack < (512 << 10)) stack = 512 << 10;
    pthread_attr_setstacksize(&a, (stack + 16383) & ~16383UL);
    if (ba && (ba->flags & 1)) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    Start *s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    int r = pthread_create(t, &a, thread_start, s);
    pthread_attr_destroy(&a);
    return r;
}

// Semaphores: Darwin has no unnamed POSIX semaphores. bionic sem_t is 16 bytes; offset 8 holds ours.
typedef struct { pthread_mutex_t m; pthread_cond_t c; int count; } Sem;
static Sem *sm(void *s) { return *(Sem **)((char *)s + 8); }
static int b_sem_init(void *s, int pshared, unsigned v) {
    (void)pshared;
    Sem *n = calloc(1, sizeof *n);
    pthread_mutex_init(&n->m, NULL);
    pthread_cond_init(&n->c, NULL);
    n->count = (int)v;
    *(Sem **)((char *)s + 8) = n;
    return 0;
}
static int b_sem_post(void *s) {
    Sem *x = sm(s);
    pthread_mutex_lock(&x->m);
    x->count++;
    pthread_cond_signal(&x->c);
    pthread_mutex_unlock(&x->m);
    return 0;
}
static int b_sem_wait(void *s) {
    Sem *x = sm(s);
    pthread_mutex_lock(&x->m);
    while (x->count <= 0) pthread_cond_wait(&x->c, &x->m);
    x->count--;
    pthread_mutex_unlock(&x->m);
    return 0;
}
static int b_sem_trywait(void *s) {
    Sem *x = sm(s);
    pthread_mutex_lock(&x->m);
    int ok = x->count > 0;
    if (ok) x->count--;
    pthread_mutex_unlock(&x->m);
    if (!ok) { errno = 11; return -1; }
    return 0;
}
static int b_sem_timedwait(void *s, const struct timespec *t) {
    Sem *x = sm(s);
    pthread_mutex_lock(&x->m);
    int r = 0;
    while (x->count <= 0 && r != ETIMEDOUT) r = pthread_cond_timedwait(&x->c, &x->m, t);
    int ok = x->count > 0;
    if (ok) x->count--;
    pthread_mutex_unlock(&x->m);
    if (!ok) { errno = 110; return -1; }
    return 0;
}
static int b_sem_getvalue(void *s, int *v) { *v = sm(s)->count; return 0; }

static int b_sched_get_priority_max(int p) { (void)p; return 0; }
static int b_sched_get_priority_min(int p) { (void)p; return 0; }

// ---------------------------------------------------------------- signals, epoll (not needed on iOS)
static int b_sigaction(int sig, const void *act, void *old) {
    LOG("sigaction(%d) ignored", sig);
    if (old) memset(old, 0, 32);
    (void)act;
    return 0;
}
static void *b_signal(int sig, void *h) { LOG("signal(%d) ignored", sig); (void)h; return NULL; }
static int b_sigaltstack(const void *ss, void *old) { (void)ss; if (old) memset(old, 0, 24); return 0; }
static int b_sigemptyset(uint64_t *set) { *set = 0; return 0; }
static int b_epoll_create(int n) { (void)n; LOG("epoll_create"); errno = ENOSYS; return -1; }
static int b_epoll_ctl(int a, int b, int c, void *d) { (void)a; (void)b; (void)c; (void)d; errno = ENOSYS; return -1; }
static int b_epoll_wait(int a, void *b, int c, int d) { (void)a; (void)b; (void)c; (void)d; errno = ENOSYS; return -1; }

// ---------------------------------------------------------------- dl*
#define GL_HANDLE ((void *)0x60001)
static char g_dlerr[256];
static void *b_dlopen(const char *name, int flags) {
    (void)flags;
    if (!name) return GL_HANDLE;
    ElfImage *img = elf_find(name);
    if (img) return img;
    if (strstr(name, "GLES") || strstr(name, "EGL") || strstr(name, "libc.so") || strstr(name, "libm.so") || strstr(name, "libdl"))
        return GL_HANDLE;
    snprintf(g_dlerr, sizeof g_dlerr, "dlopen %s: not available", name);
    LOG("%s", g_dlerr);
    return NULL;
}
static void *b_dlsym(void *h, const char *name) {
    void *p = NULL;
    if (h && h != GL_HANDLE && (uintptr_t)h > 0x100000) p = elf_sym((ElfImage *)h, name);
    if (!p) p = bionic_resolve(name);
    if (!p) { snprintf(g_dlerr, sizeof g_dlerr, "dlsym %s: not found", name); LOG("%s", g_dlerr); }
    return p;
}
static char *b_dlerror(void) { return g_dlerr[0] ? g_dlerr : NULL; }
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } BDlInfo;
static int b_dladdr(const void *addr, BDlInfo *info) {
    ElfImage *img = elf_containing(addr);
    if (img) {
        // An Android-style path: the runtime opens libspidermanarm32.so next to its own library.
        static char names[8][128];
        static int n;
        int slot = n++ & 7;
        snprintf(names[slot], sizeof names[slot], "/data/app/smtm/lib/arm64/%s", elf_name(img));
        info->dli_fname = names[slot];
        info->dli_fbase = (void *)elf_base(img);
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
        return 1;
    }
    Dl_info d;
    if (!dladdr(addr, &d)) return 0;
    info->dli_fname = d.dli_fname; info->dli_fbase = d.dli_fbase; info->dli_sname = d.dli_sname; info->dli_saddr = d.dli_saddr;
    return 1;
}

// ---------------------------------------------------------------- locale & wide chars
// bionic mbstate_t is 8 bytes, Darwin's 128: conversions run with a private state per call.
static size_t b_mbrtowc(wchar_t *w, const char *s, size_t n, void *ps) { (void)ps; mbstate_t st = {0}; return mbrtowc(w, s, n, &st); }
static size_t b_mbrlen(const char *s, size_t n, void *ps) { (void)ps; mbstate_t st = {0}; return mbrlen(s, n, &st); }
static size_t b_wcrtomb(char *s, wchar_t w, void *ps) { (void)ps; mbstate_t st = {0}; return wcrtomb(s, w, &st); }
static size_t b_mbsrtowcs(wchar_t *d, const char **s, size_t n, void *ps) { (void)ps; mbstate_t st = {0}; return mbsrtowcs(d, s, n, &st); }
static size_t b_mbsnrtowcs(wchar_t *d, const char **s, size_t nms, size_t n, void *ps) { (void)ps; mbstate_t st = {0}; return mbsnrtowcs(d, s, nms, n, &st); }
static size_t b_wcsnrtombs(char *d, const wchar_t **s, size_t nwc, size_t n, void *ps) { (void)ps; mbstate_t st = {0}; return wcsnrtombs(d, s, nwc, n, &st); }
static locale_t b_newlocale(int mask, const char *name, locale_t base) { (void)mask; (void)base; (void)name; return newlocale(LC_ALL_MASK, "C", NULL); }
// strtold_l returns a binary128 long double in q0: compute a double and hand back its quad bits.
typedef struct { uint64_t lo, hi; } Q128;
static __attribute__((naked)) void b_strtold_l(void) {
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n mov x29, sp\n"
        "bl _bionic_strtold_bits\n"          // x0/x1 = quad bits
        "fmov d0, x0\n mov v0.d[1], x1\n"
        "ldp x29, x30, [sp], #16\n ret\n");
}
Q128 bionic_strtold_bits(const char *s, char **end, locale_t l) {
    (void)l;
    uint64_t q[2];
    double_to_quad(strtod(s, end), q);
    return (Q128){q[0], q[1]};
}

// ---------------------------------------------------------------- the table
#define D(n) {#n, (void *)n}
#define W(n) {#n, (void *)b_##n}
#define A(n) {#n, (void *)av_##n}
typedef struct { const char *name; void *addr; } Entry;

extern int av_snprintf(), av_sprintf(), av_printf(), av_fprintf(), av_sscanf(), av_syslog(), av_open(), av_fcntl(), av_prctl(), av_syscall(), av_android_log_print();

static const Entry g_libc[] = {
    W(__android_log_write), {"__android_log_print", (void *)av_android_log_print}, W(__ctype_get_mb_cur_max), W(__cxa_atexit), W(__cxa_finalize),
    W(__cxa_thread_atexit_impl), W(__errno), W(__memcpy_chk), W(__pread_chk), W(__read_chk), W(__register_atfork),
    {"__sF", (void *)g_sF}, W(__stack_chk_fail), W(__strchr_chk), W(__strcpy_chk), W(__strncpy_chk2), W(__system_property_get),
    W(__umask_chk), W(__vsnprintf_chk), W(__write_chk), D(abort), W(access), D(acos), D(acosf), W(android_set_abort_message),
    D(asctime_r), D(asin), D(asinf), D(atan2), D(atan2f), D(atan), D(atanf), D(atoi), D(atoll), W(chdir), W(chmod), W(clearerr),
    D(clock), W(clock_gettime), D(close), W(closedir), W(closelog), D(cos), D(cosf), D(cosh), {"dl_iterate_phdr", (void *)elf_dl_iterate_phdr},
    W(dladdr), W(dlerror), W(dlopen), W(dlsym), W(epoll_create), W(epoll_ctl), W(epoll_wait), D(exit), D(exp), D(expf),
    W(fclose), A(fcntl), D(fdopen), W(fflush), W(fgetc), W(fgets), W(fgetwc), D(fmod), D(fmodf), W(fopen), A(fprintf), W(fputc),
    W(fputs), W(fputwc), W(fread), D(free), D(freelocale), D(frexp), W(fseek), W(fstat), D(fsync), W(ftell), D(ftruncate), W(fwrite),
    W(getauxval), D(getcwd), D(getegid), D(getenv), D(geteuid), D(getgid), D(getpid), W(gettid), D(gettimeofday), D(getuid),
    D(gmtime_r), D(inet_addr), D(inet_ntoa), D(iswctype), D(iswlower_l), D(ldexp), D(localtime_r), D(log10), D(log10f), D(log),
    D(logf), D(lrand48), D(lseek), D(malloc), W(mbrlen), W(mbrtowc), W(mbsnrtowcs), W(mbsrtowcs), D(mbstowcs), D(mbtowc), D(memchr),
    D(memcmp), D(memcpy), D(memmem), D(memmove), D(memset), W(mkdir), D(mkstemp), D(mktime), W(mmap), D(modf), D(modff), W(mprotect),
    D(munmap), D(nanosleep), W(newlocale), A(open), W(openlog), W(opendir), D(pipe), D(poll), D(posix_memalign), D(pow), D(powf),
    A(prctl), W(pthread_attr_destroy), W(pthread_attr_init), W(pthread_attr_setdetachstate), W(pthread_attr_setstacksize),
    W(pthread_cond_broadcast), W(pthread_cond_init), W(pthread_cond_signal), W(pthread_cond_timedwait), W(pthread_cond_wait),
    W(pthread_create), D(pthread_detach), W(pthread_getspecific), D(pthread_join), W(pthread_key_create), W(pthread_key_delete),
    W(pthread_mutex_destroy), W(pthread_mutex_init), W(pthread_mutex_lock), W(pthread_mutex_trylock), W(pthread_mutex_unlock),
    W(pthread_mutexattr_destroy), W(pthread_mutexattr_init), W(pthread_mutexattr_settype), W(pthread_once), W(pthread_rwlock_rdlock),
    W(pthread_rwlock_unlock), W(pthread_rwlock_wrlock), D(pthread_self), W(pthread_setspecific), W(putchar), W(puts), D(read),
    W(readdir), D(realloc), W(remove), W(rename), W(rewind), W(rmdir), W(sched_get_priority_max), W(sched_get_priority_min),
    D(sched_yield), W(sem_getvalue), W(sem_init), W(sem_post), W(sem_timedwait), W(sem_trywait), W(sem_wait), W(sendfile), D(setenv),
    W(sigaction), W(sigaltstack), W(sigemptyset), W(signal), D(sin), D(sinf), D(sinh), A(snprintf), {"sprintf", (void *)av_sprintf},
    {"printf", (void *)av_printf}, D(srand48), A(sscanf), W(stat), W(statfs), D(strcasecmp), D(strcat), D(strchr), D(strcmp), D(strcpy),
    D(strcspn), D(strerror), D(strerror_r), D(strftime), D(strftime_l), D(strlcat), D(strlen), D(strncasecmp), D(strncmp), D(strncpy),
    D(strpbrk), D(strptime), D(strrchr), D(strstr), D(strtod), D(strtof), D(strtok_r), W(strtold_l), D(strtoll), D(strtoll_l), D(strtoul),
    D(strtoull), D(strtoull_l), A(syscall), W(sysconf), A(syslog), D(tan), D(tanf), D(tanh), D(time), D(times), D(towlower), D(towupper),
    W(uname), W(ungetc), W(ungetwc), W(unlink), D(uselocale), D(usleep), W(utime), W(vasprintf), W(vfprintf), W(vsnprintf), W(vsscanf),
    W(wcrtomb), D(wcscmp), D(wcscpy), D(wcsftime), D(wcslen), D(wcsncpy), W(wcsnrtombs), D(wcstoll), D(wcstoull), D(wctype), D(wmemchr),
    D(wmemcmp), D(wmemcpy), D(wmemmove), D(wmemset), D(writev), D(write), D(calloc), D(strdup), D(qsort), D(bsearch), D(abs), D(labs),
    D(floor), D(floorf), D(ceil), D(ceilf), D(sqrt), D(sqrtf), D(fabs), D(fabsf), D(rand), D(srand), D(atof), D(strtol), D(isalpha),
    D(isdigit), D(isspace), D(isupper), D(islower), D(tolower), D(toupper), D(isxdigit), D(isprint), D(ispunct), D(isalnum),
};

void *bionic_resolve(const char *name) {
    static int inited;
    if (!inited) {
        inited = 1;
        g_stdin = stdin;
        g_stdout = stdout;
        g_stderr = stderr;
    }
    if (!strcmp(name, "stdin")) return &g_stdin;
    if (!strcmp(name, "stdout")) return &g_stdout;
    if (!strcmp(name, "stderr")) return &g_stderr;
    for (size_t i = 0; i < sizeof g_libc / sizeof g_libc[0]; i++)
        if (!strcmp(g_libc[i].name, name)) return g_libc[i].addr;
    void *gl = bionic_gl_resolve(name);
    if (gl) return gl;
    return NULL;
}
