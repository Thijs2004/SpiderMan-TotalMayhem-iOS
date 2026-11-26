// bionic (Android libc) on Darwin: symbol table for the ELF loader.
#pragma once
#include <stddef.h>

/// Address for an Android import (libc, libm, libdl, liblog, GLES, EGL), or NULL.
void *bionic_resolve(const char *name);
/// GLES 1/2 + EGL imports (gl.m).
void *bionic_gl_resolve(const char *name);
/// Maps an Android path (/sdcard/..., /data/data/...) to the app container (host.m).
const char *bionic_path(const char *path);
/// Android system property value, or NULL (host.m).
const char *bionic_property(const char *name);
