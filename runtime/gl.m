// GLES 1/2 imports go straight to Apple's OpenGL ES; EGL queries are answered from the app's GL view.
#import <OpenGLES/ES1/gl.h>
#import <OpenGLES/ES2/gl.h>
#import <dlfcn.h>
#import <string.h>
#import <stdio.h>
#import "bionic.h"
#import "elfload.h"

int smtm_surface_width = 1, smtm_surface_height = 1;

static void *b_eglGetCurrentDisplay(void) { return (void *)1; }
static void *b_eglGetCurrentSurface(int which) { (void)which; return (void *)1; }
static unsigned b_eglQuerySurface(void *dpy, void *surf, int attr, int *value) {
    (void)dpy; (void)surf;
    if (attr == 0x3057) { *value = smtm_surface_width; return 1; }    // EGL_WIDTH
    if (attr == 0x3056) { *value = smtm_surface_height; return 1; }   // EGL_HEIGHT
    *value = 0;
    return 1;
}
static void *b_eglGetProcAddress(const char *name) { return bionic_gl_resolve(name); }

void *bionic_gl_resolve(const char *name) {
    if (!strcmp(name, "eglGetCurrentDisplay")) return (void *)b_eglGetCurrentDisplay;
    if (!strcmp(name, "eglGetCurrentSurface")) return (void *)b_eglGetCurrentSurface;
    if (!strcmp(name, "eglQuerySurface")) return (void *)b_eglQuerySurface;
    if (!strcmp(name, "eglGetProcAddress")) return (void *)b_eglGetProcAddress;
    if (strncmp(name, "gl", 2) != 0) return NULL;
    void *p = dlsym(RTLD_DEFAULT, name);
    if (!p) {   // OES-suffixed ES1 extensions on iOS
        char alt[128];
        snprintf(alt, sizeof alt, "%sOES", name);
        p = dlsym(RTLD_DEFAULT, alt);
    }
    if (!p) elf_log("gl: %s not available", name);
    return p;
}
