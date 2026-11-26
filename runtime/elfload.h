// Links the Android ELF images that elf2macho baked into signed dylibs (code is never written, only __ELFW).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct ElfImage ElfImage;

/// Resolves an imported symbol name (bionic / GLES / EGL); NULL = not provided.
typedef void *(*ElfImportResolver)(const char *name);

/// Registers an image at `base` (vaddr 0 of the ELF) under `soname` (e.g. "libspiderman.so").
ElfImage *elf_register(const char *soname, void *base);
/// Applies relocations, resolving imports through `resolver` and then the other registered images.
int elf_link(ElfImage *img, ElfImportResolver resolver);
/// Runs DT_INIT / DT_INIT_ARRAY.
void elf_run_init(ElfImage *img);

ElfImage *elf_find(const char *soname);
void *elf_sym(ElfImage *img, const char *name);
uintptr_t elf_base(ElfImage *img);
const char *elf_name(ElfImage *img);
/// Image containing `addr`, or NULL.
ElfImage *elf_containing(const void *addr);

/// bionic-compatible dl_iterate_phdr over the registered images (libunwind needs it for exceptions).
struct elf_dl_phdr_info {
    uintptr_t dlpi_addr;
    const char *dlpi_name;
    const void *dlpi_phdr;
    uint16_t dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};
int elf_dl_iterate_phdr(int (*cb)(struct elf_dl_phdr_info *, size_t, void *), void *data);

/// This thread's PT_TLS block of an image (inside the Darwin TSD array).
void *elf_tls_block(ElfImage *img);
/// Writes the TLS initial data for the calling thread; call on every thread before it runs ELF code.
void elf_tls_thread_init(void);

void elf_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
