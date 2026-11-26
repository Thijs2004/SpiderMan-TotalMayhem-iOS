// Links the Android ELF images baked into the app by elf2macho: relocations (RELR, RELA, JMPREL), imports,
// ELF TLS (TLSDESC) and init arrays. Code pages are signed and never written; every relocation target is in
// the writable __ELFW copy (the libraries have no text relocations).
#include "elfload.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { int64_t tag; uint64_t val; } Dyn;
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Sym;
typedef struct { uint64_t offset, info; int64_t addend; } Rela;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7 };
enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_INIT = 12,
       DT_SONAME = 14, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35, DT_RELR = 36 };
#define DT_GNU_HASH 0x6ffffef5
#define DT_ANDROID_RELR 0x6fffe000
#define DT_ANDROID_RELRSZ 0x6fffe001
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027, R_TLS_TPREL = 1030, R_TLSDESC = 1031 };

struct ElfImage {
    char soname[64];
    uintptr_t base;
    const Phdr *phdr;
    uint16_t phnum;
    const Dyn *dyn;
    const Sym *symtab;
    const char *strtab;
    const uint32_t *gnuhash;
    const Phdr *tls;
    pthread_key_t tlskey0;   // first of the consecutive TSD slots holding this image's TLS block
    int tlsslots;
    int linked, inited;
};

#define MAX_IMAGES 8
static ElfImage g_images[MAX_IMAGES];
static int g_nimages;

static FILE *g_logf;
void elf_log(const char *fmt, ...) {
    va_list ap;
    char line[1024];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[smtm] %s\n", line);
    if (!g_logf) {
        const char *home = getenv("HOME");
        char path[1024];
        snprintf(path, sizeof path, "%s/Documents/smtm.log", home ? home : "/tmp");
        g_logf = fopen(path, "w");
    }
    if (g_logf) { fprintf(g_logf, "%s\n", line); fflush(g_logf); }
}

ElfImage *elf_register(const char *soname, void *base) {
    if (g_nimages == MAX_IMAGES) return NULL;
    ElfImage *img = &g_images[g_nimages++];
    memset(img, 0, sizeof *img);
    strlcpy(img->soname, soname, sizeof img->soname);
    img->base = (uintptr_t)base;
    const Ehdr *eh = (const Ehdr *)base;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) != 0) { elf_log("%s: no ELF header at %p", soname, base); return NULL; }
    img->phdr = (const Phdr *)(img->base + eh->phoff);
    img->phnum = eh->phnum;
    for (int i = 0; i < img->phnum; i++) {
        if (img->phdr[i].type == PT_DYNAMIC) img->dyn = (const Dyn *)(img->base + img->phdr[i].vaddr);
        if (img->phdr[i].type == PT_TLS) img->tls = &img->phdr[i];
    }
    for (const Dyn *d = img->dyn; d && d->tag != DT_NULL; d++) {
        if (d->tag == DT_SYMTAB) img->symtab = (const Sym *)(img->base + d->val);
        if (d->tag == DT_STRTAB) img->strtab = (const char *)(img->base + d->val);
        if (d->tag == DT_GNU_HASH) img->gnuhash = (const uint32_t *)(img->base + d->val);
    }
    if (img->tls) {
        // The block lives inside the thread's Darwin TSD array: consecutive pthread keys, addressed from
        // TPIDRRO_EL0 (which the patched code reads as its thread pointer).
        img->tlsslots = (int)((img->tls->memsz + 7) / 8);
        for (int i = 0; i < img->tlsslots; i++) {
            pthread_key_t k;
            pthread_key_create(&k, NULL);
            if (i == 0) img->tlskey0 = k;
            else if (k != img->tlskey0 + i) { elf_log("%s: TSD keys not consecutive", soname); abort(); }
        }
        elf_tls_thread_init();
    }
    elf_log("%s at %p (tls %s)", soname, base, img->tls ? "yes" : "no");
    return img;
}

ElfImage *elf_find(const char *soname) {
    for (int i = 0; i < g_nimages; i++) {
        const char *s = strrchr(soname, '/');
        if (!strcmp(g_images[i].soname, s ? s + 1 : soname)) return &g_images[i];
    }
    return NULL;
}

uintptr_t elf_base(ElfImage *img) { return img->base; }
const char *elf_name(ElfImage *img) { return img->soname; }

ElfImage *elf_containing(const void *addr) {
    uintptr_t a = (uintptr_t)addr;
    for (int i = 0; i < g_nimages; i++) {
        ElfImage *img = &g_images[i];
        for (int p = 0; p < img->phnum; p++) {
            const Phdr *ph = &img->phdr[p];
            if (ph->type == PT_LOAD && a >= img->base + ph->vaddr && a < img->base + ph->vaddr + ph->memsz) return img;
        }
    }
    return NULL;
}

static uint32_t gnu_hash(const char *s) {
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (uint8_t)*s;
    return h;
}

static const Sym *lookup(ElfImage *img, const char *name) {
    const uint32_t *gh = img->gnuhash;
    if (!gh) return NULL;
    uint32_t nbuckets = gh[0], symoffset = gh[1], bloom_size = gh[2];
    const uint64_t *bloom = (const uint64_t *)(gh + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t h = gnu_hash(name);
    uint32_t idx = buckets[h % nbuckets];
    if (idx < symoffset) return NULL;
    for (;; idx++) {
        uint32_t ch = chain[idx - symoffset];
        const Sym *s = &img->symtab[idx];
        if ((ch | 1) == (h | 1) && !strcmp(img->strtab + s->name, name) && s->shndx != 0) return s;
        if (ch & 1) return NULL;
    }
}

void *elf_sym(ElfImage *img, const char *name) {
    const Sym *s = lookup(img, name);
    return s ? (void *)(img->base + s->value) : NULL;
}

// Calls to imports nobody provides land on one of these stubs (assembly), which reports the name.
extern char elf_missing_stub_base[];   // 256 stubs of 8 bytes (asm.S)
#define elf_missing_stub_count 256
static const char *g_missing_names[256];
static int g_nmissing;
void elf_missing_called(int idx) {
    elf_log("FATAL: called missing import %s", idx < g_nmissing ? g_missing_names[idx] : "?");
    abort();
}

static void *resolve(ElfImage *img, uint32_t symidx, ElfImportResolver resolver, int *weak) {
    const Sym *s = &img->symtab[symidx];
    const char *name = img->strtab + s->name;
    *weak = (s->info >> 4) == 2;
    if (s->shndx != 0) return (void *)(img->base + s->value);   // defined here
    void *p = resolver ? resolver(name) : NULL;
    if (p) return p;
    for (int i = 0; i < g_nimages; i++) {
        if (&g_images[i] == img) continue;
        if ((p = elf_sym(&g_images[i], name))) return p;
    }
    return NULL;
}

// TLSDESC: the GOT pair is {resolver, argument}. The block sits at TSD slot tlskey0, so the offset from the
// thread pointer is a constant and the resolver only returns its argument.
extern void elf_tlsdesc_static(void);

void *elf_tls_block(ElfImage *img) {
    if (!img->tls) return NULL;
    uint64_t tp;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    return (void *)(tp + 8 * img->tlskey0);
}

void elf_tls_thread_init(void) {
    for (int i = 0; i < g_nimages; i++) {
        ElfImage *img = &g_images[i];
        if (!img->tls) continue;
        uint64_t slots[64] = {0};
        memcpy(slots, (void *)(img->base + img->tls->vaddr), img->tls->filesz);
        for (int s = 0; s < img->tlsslots; s++) pthread_setspecific(img->tlskey0 + s, (void *)slots[s]);
    }
}

static void apply_rela(ElfImage *img, const Rela *r, size_t n, ElfImportResolver resolver, int *missing) {
    for (size_t i = 0; i < n; i++) {
        uint32_t type = (uint32_t)r[i].info, symidx = (uint32_t)(r[i].info >> 32);
        uint64_t *where = (uint64_t *)(img->base + r[i].offset);
        int weak = 0;
        switch (type) {
        case R_RELATIVE:
            *where = img->base + r[i].addend;
            break;
        case R_ABS64: case R_GLOB_DAT: case R_JUMP_SLOT: {
            void *s = resolve(img, symidx, resolver, &weak);
            if (!s && !weak) {
                const char *name = img->strtab + img->symtab[symidx].name;
                if (g_nmissing < elf_missing_stub_count) {
                    g_missing_names[g_nmissing] = name;
                    s = elf_missing_stub_base + 8 * g_nmissing++;
                }
                elf_log("%s: missing import %s", img->soname, name);
                (*missing)++;
            }
            *where = (uint64_t)s + (type == R_JUMP_SLOT ? 0 : r[i].addend);
            break;
        }
        case R_TLSDESC: {   // the image's own TLS (Android libraries keep it local)
            const Sym *s = &img->symtab[symidx];
            where[0] = (uint64_t)elf_tlsdesc_static;
            where[1] = 8 * img->tlskey0 + (symidx ? s->value : 0) + r[i].addend;
            break;
        }
        default:
            elf_log("%s: unsupported relocation type %u at %#llx", img->soname, type, (unsigned long long)r[i].offset);
            (*missing)++;
        }
    }
}

static void apply_relr(ElfImage *img, const uint64_t *relr, size_t n) {
    uint64_t *where = NULL;
    for (size_t i = 0; i < n; i++) {
        uint64_t e = relr[i];
        if (!(e & 1)) {
            where = (uint64_t *)(img->base + e);
            *where++ += img->base;
        } else {
            uint64_t *w = where;
            for (e >>= 1; e; e >>= 1, w++)
                if (e & 1) *w += img->base;
            where += 63;
        }
    }
}

int elf_link(ElfImage *img, ElfImportResolver resolver) {
    if (img->linked) return 0;
    const Rela *rela = NULL, *jmprel = NULL;
    const uint64_t *relr = NULL;
    size_t relasz = 0, pltsz = 0, relrsz = 0;
    for (const Dyn *d = img->dyn; d->tag != DT_NULL; d++) {
        switch (d->tag) {
        case DT_RELA: rela = (const Rela *)(img->base + d->val); break;
        case DT_RELASZ: relasz = d->val; break;
        case DT_JMPREL: jmprel = (const Rela *)(img->base + d->val); break;
        case DT_PLTRELSZ: pltsz = d->val; break;
        case DT_RELR: case DT_ANDROID_RELR: relr = (const uint64_t *)(img->base + d->val); break;
        case DT_RELRSZ: case DT_ANDROID_RELRSZ: relrsz = d->val; break;
        }
    }
    int missing = 0;
    if (relr) apply_relr(img, relr, relrsz / 8);
    if (rela) apply_rela(img, rela, relasz / sizeof(Rela), resolver, &missing);
    if (jmprel) apply_rela(img, jmprel, pltsz / sizeof(Rela), resolver, &missing);
    img->linked = 1;
    elf_log("%s linked (%d unresolved)", img->soname, missing);
    return missing;
}

void elf_run_init(ElfImage *img) {
    if (img->inited) return;
    img->inited = 1;
    void (*init)(void) = NULL;
    void (**arr)(int, char **, char **) = NULL;
    size_t n = 0;
    for (const Dyn *d = img->dyn; d->tag != DT_NULL; d++) {
        if (d->tag == DT_INIT) init = (void (*)(void))(img->base + d->val);
        if (d->tag == DT_INIT_ARRAY) arr = (void (**)(int, char **, char **))(img->base + d->val);
        if (d->tag == DT_INIT_ARRAYSZ) n = d->val / 8;
    }
    static char *argv[] = {"smtm", NULL}, *envp[] = {NULL};
    if (init) init();
    for (size_t i = 0; i < n; i++)
        if (arr[i] && (uintptr_t)arr[i] != (uintptr_t)-1) arr[i](1, argv, envp);
    elf_log("%s: %zu initialisers run", img->soname, n);
}

int elf_dl_iterate_phdr(int (*cb)(struct elf_dl_phdr_info *, size_t, void *), void *data) {
    for (int i = 0; i < g_nimages; i++) {
        struct elf_dl_phdr_info info = {
            .dlpi_addr = g_images[i].base, .dlpi_name = g_images[i].soname,
            .dlpi_phdr = g_images[i].phdr, .dlpi_phnum = g_images[i].phnum,
        };
        int r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return 0;
}
