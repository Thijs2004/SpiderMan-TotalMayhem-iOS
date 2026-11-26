// JavaVM / JNIEnv for libspiderman.so (see jni.h). The table follows the JNI 1.6 layout (233 entries).
#include "jni.h"
#include "elfload.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG elf_log
typedef struct { char *stack; char *gr_top; char *vr_top; int gr_offs; int vr_offs; } AVa;
static uint64_t va_gp(AVa *va) {
    if (va->gr_offs < 0) { uint64_t v = *(uint64_t *)(va->gr_top + va->gr_offs); va->gr_offs += 8; return v; }
    uint64_t v = *(uint64_t *)va->stack; va->stack += 8; return v;
}
static double va_fp(AVa *va) {
    if (va->vr_offs < 0) { double v = *(double *)(va->vr_top + va->vr_offs); va->vr_offs += 16; return v; }
    double v = *(double *)va->stack; va->stack += 8; return v;
}

// ---------------------------------------------------------------- classes and methods
static JClass *g_classes[256];
static int g_nclasses;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

JClass *jni_class(const char *name) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nclasses; i++)
        if (!strcmp(g_classes[i]->name, name)) { pthread_mutex_unlock(&g_lock); return g_classes[i]; }
    JClass *c = calloc(1, sizeof *c);
    strlcpy(c->name, name, sizeof c->name);
    if (g_nclasses < 256) g_classes[g_nclasses++] = c;
    pthread_mutex_unlock(&g_lock);
    return c;
}

static void parse_sig(JMethod *m) {
    const char *p = strchr(m->sig, '(');
    m->nargs = 0;
    if (!p) return;
    for (p++; *p && *p != ')';) {
        char t = *p;
        if (t == '[') { while (*p == '[') p++; if (*p == 'L') p = strchr(p, ';') + 1; else p++; t = 'L'; }
        else if (t == 'L') { p = strchr(p, ';') + 1; }
        else p++;
        if (m->nargs < 31) m->argtypes[m->nargs++] = t;
    }
    m->ret = p[0] == ')' ? (p[1] == '[' ? 'L' : p[1]) : 'V';
}

JMethod *jni_method(JClass *cls, const char *name, const char *sig, bool isStatic) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < cls->nmethods; i++) {
        JMethod *m = cls->methods[i];
        if (!strcmp(m->name, name) && (!sig || !m->sig[0] || !strcmp(m->sig, sig))) {
            if (sig && !m->sig[0]) { strlcpy(m->sig, sig, sizeof m->sig); parse_sig(m); }
            pthread_mutex_unlock(&g_lock);
            return m;
        }
    }
    JMethod *m = calloc(1, sizeof *m);
    m->cls = cls;
    strlcpy(m->name, name, sizeof m->name);
    if (sig) strlcpy(m->sig, sig, sizeof m->sig);
    m->isStatic = isStatic;
    parse_sig(m);
    if (cls->nmethods < 96) cls->methods[cls->nmethods++] = m;
    pthread_mutex_unlock(&g_lock);
    return m;
}

void jni_define(const char *cls, const char *name, const char *sig, bool isStatic, JImpl impl) {
    JMethod *m = jni_method(jni_class(cls), name, sig, isStatic);
    m->impl = impl;
}

void *jni_native(const char *cls, const char *name) {
    JClass *c = jni_class(cls);
    for (int i = 0; i < c->nmethods; i++)
        if (!strcmp(c->methods[i]->name, name) && c->methods[i]->native) return c->methods[i]->native;
    return NULL;
}

// ---------------------------------------------------------------- objects
JObject *jni_object(JClass *cls) {
    JObject *o = calloc(1, sizeof *o);
    o->kind = JK_OBJECT;
    o->cls = cls;
    return o;
}
JObject *jni_string(const char *utf8) {
    JObject *o = jni_object(jni_class("java/lang/String"));
    o->kind = JK_STRING;
    o->utf8 = strdup(utf8 ? utf8 : "");
    return o;
}
const char *jni_cstr(JObject *s) { return s && s->kind == JK_STRING ? s->utf8 : NULL; }
JObject *jni_array(int elemSize, int length) {
    JObject *o = jni_object(jni_class("[array"));
    o->kind = JK_ARRAY;
    o->elemSize = elemSize;
    o->length = length;
    o->data = calloc(length ? length : 1, elemSize);
    return o;
}
static JObject *class_object(JClass *c) {
    // jclass handles are the JClass pointers themselves; mark them so GetObjectClass/IsInstanceOf work.
    return (JObject *)c;
}

// ---------------------------------------------------------------- calls
static int g_unimpl_logged;
static JValue invoke(JMethod *m, JObject *self, const JValue *args) {
    if (m->impl) return m->impl(self, args);
    if (m->native) {   // a Java method implemented by the host itself: call it like Java would
        void *env = jni_env();
        uint64_t a[8] = {0};
        double fa[8] = {0};
        int ni = 0, nf = 0;
        a[ni++] = (uint64_t)env;
        a[ni++] = (uint64_t)(m->isStatic ? (void *)m->cls : self);
        for (int i = 0; i < m->nargs; i++) {
            char t = m->argtypes[i];
            if (t == 'F') { float f = args[i].f; memcpy(&fa[nf++], &f, 4); }
            else if (t == 'D') fa[nf++] = args[i].d;
            else a[ni++] = (uint64_t)args[i].j;
        }
        JValue r = {0};
        // Non-variadic calls use the same registers on AAPCS and Darwin: x0-x7 and v0-v7 (a float sits in
        // the low 32 bits of its vector register, which is where the memcpy above put it).
        typedef uint64_t (*Fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                               double, double, double, double, double, double, double, double);
        r.j = (int64_t)((Fn)m->native)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                                       fa[0], fa[1], fa[2], fa[3], fa[4], fa[5], fa[6], fa[7]);
        return r;
    }
    if (g_unimpl_logged < 4000) {
        g_unimpl_logged++;
        char argbuf[256] = "";
        for (int i = 0; i < m->nargs && strlen(argbuf) < 200; i++) {
            char one[64];
            char t = m->argtypes[i];
            if (t == 'F') snprintf(one, sizeof one, "%g", args[i].f);
            else if (t == 'D') snprintf(one, sizeof one, "%g", args[i].d);
            else if (t == 'L') snprintf(one, sizeof one, "%s", jni_cstr(args[i].l) ? jni_cstr(args[i].l) : "obj");
            else if (t == 'J') snprintf(one, sizeof one, "%lld", (long long)args[i].j);
            else snprintf(one, sizeof one, "%d", args[i].i);
            strlcat(argbuf, i ? ", " : "", sizeof argbuf);
            strlcat(argbuf, one, sizeof argbuf);
        }
        LOG("java: %s.%s%s (%s) not implemented", m->cls->name, m->name, m->sig, argbuf);
    }
    JValue r = {0};
    return r;
}

static void args_from_va(JMethod *m, AVa *va, JValue *out) {
    for (int i = 0; i < m->nargs; i++) {
        char t = m->argtypes[i];
        if (t == 'F') out[i].f = (float)va_fp(va);
        else if (t == 'D') out[i].d = va_fp(va);
        else if (t == 'J' || t == 'L') out[i].j = (int64_t)va_gp(va);
        else out[i].j = (int32_t)va_gp(va);
    }
}

// ---------------------------------------------------------------- JNIEnv functions
typedef void *E;   // JNIEnv*
#define M(mid) ((JMethod *)(mid))

static int32_t f_GetVersion(E e) { (void)e; return 0x10006; }
static void *f_FindClass(E e, const char *name) { (void)e; return jni_class(name); }
static void *f_GetSuperclass(E e, void *c) { (void)e; (void)c; return jni_class("java/lang/Object"); }
static uint8_t f_IsAssignableFrom(E e, void *a, void *b) { (void)e; return a == b; }
static int32_t f_Throw(E e, void *t) { (void)e; (void)t; LOG("java: Throw"); return 0; }
static int32_t f_ThrowNew(E e, void *c, const char *m) { (void)e; LOG("java: ThrowNew %s: %s", ((JClass *)c)->name, m); return 0; }
static void *f_ExceptionOccurred(E e) { (void)e; return NULL; }
static void f_ExceptionDescribe(E e) { (void)e; }
static void f_ExceptionClear(E e) { (void)e; }
static void f_FatalError(E e, const char *m) { (void)e; LOG("FATAL (jni): %s", m); abort(); }
static int32_t f_PushLocalFrame(E e, int32_t n) { (void)e; (void)n; return 0; }
static void *f_PopLocalFrame(E e, void *r) { (void)e; return r; }
static void *f_NewRef(E e, void *o) { (void)e; return o; }
static void f_DeleteRef(E e, void *o) { (void)e; (void)o; }
static uint8_t f_IsSameObject(E e, void *a, void *b) { (void)e; return a == b; }
static int32_t f_EnsureLocalCapacity(E e, int32_t n) { (void)e; (void)n; return 0; }
static void *f_AllocObject(E e, void *c) { (void)e; return jni_object(c); }
static void *f_NewObjectA(E e, void *c, void *mid, const JValue *a) { (void)e; JObject *o = jni_object(c); invoke(M(mid), o, a); return o; }
static void *f_NewObjectV(E e, void *c, void *mid, AVa *va) { JValue a[32]; args_from_va(M(mid), va, a); return f_NewObjectA(e, c, mid, a); }
void *jnia_NewObject(E e, void *c, void *mid, AVa *va) { return f_NewObjectV(e, c, mid, va); }
static void *f_GetObjectClass(E e, JObject *o) {
    (void)e;
    if (!o) return NULL;
    for (int i = 0; i < g_nclasses; i++) if ((void *)g_classes[i] == (void *)o) return jni_class("java/lang/Class");
    return o->cls;
}
static uint8_t f_IsInstanceOf(E e, JObject *o, void *c) { (void)e; return o && o->cls == c; }
static void *f_GetMethodID(E e, void *c, const char *n, const char *s) { (void)e; return jni_method(c, n, s, false); }
static void *f_GetStaticMethodID(E e, void *c, const char *n, const char *s) { (void)e; return jni_method(c, n, s, true); }

// Call<Type>Method{,V,A} / CallStatic... / CallNonvirtual...
#define CALLS(Name, T, field)                                                                              \
    static T f_Call##Name##MethodA(E e, JObject *o, void *mid, const JValue *a) { (void)e; return invoke(M(mid), o, a).field; } \
    static T f_Call##Name##MethodV(E e, JObject *o, void *mid, AVa *va) { JValue a[32]; args_from_va(M(mid), va, a); return invoke(M(mid), o, a).field; } \
    T jnia_Call##Name##Method(E e, JObject *o, void *mid, AVa *va) { return f_Call##Name##MethodV(e, o, mid, va); } \
    static T f_CallStatic##Name##MethodA(E e, void *c, void *mid, const JValue *a) { (void)e; (void)c; return invoke(M(mid), NULL, a).field; } \
    static T f_CallStatic##Name##MethodV(E e, void *c, void *mid, AVa *va) { (void)c; JValue a[32]; args_from_va(M(mid), va, a); return invoke(M(mid), NULL, a).field; } \
    T jnia_CallStatic##Name##Method(E e, void *c, void *mid, AVa *va) { return f_CallStatic##Name##MethodV(e, c, mid, va); } \
    static T f_CallNonvirtual##Name##MethodA(E e, JObject *o, void *c, void *mid, const JValue *a) { (void)c; return f_Call##Name##MethodA(e, o, mid, a); } \
    static T f_CallNonvirtual##Name##MethodV(E e, JObject *o, void *c, void *mid, AVa *va) { (void)c; return f_Call##Name##MethodV(e, o, mid, va); }
CALLS(Object, void *, l)
CALLS(Boolean, uint8_t, z)
CALLS(Byte, int8_t, b)
CALLS(Char, uint16_t, c)
CALLS(Short, int16_t, s)
CALLS(Int, int32_t, i)
CALLS(Long, int64_t, j)
CALLS(Float, float, f)
CALLS(Double, double, d)
static void f_CallVoidMethodA(E e, JObject *o, void *mid, const JValue *a) { (void)e; invoke(M(mid), o, a); }
static void f_CallVoidMethodV(E e, JObject *o, void *mid, AVa *va) { JValue a[32]; args_from_va(M(mid), va, a); invoke(M(mid), o, a); (void)e; }
void jnia_CallVoidMethod(E e, JObject *o, void *mid, AVa *va) { f_CallVoidMethodV(e, o, mid, va); }
static void f_CallStaticVoidMethodA(E e, void *c, void *mid, const JValue *a) { (void)e; (void)c; invoke(M(mid), NULL, a); }
static void f_CallStaticVoidMethodV(E e, void *c, void *mid, AVa *va) { (void)c; JValue a[32]; args_from_va(M(mid), va, a); invoke(M(mid), NULL, a); (void)e; }
void jnia_CallStaticVoidMethod(E e, void *c, void *mid, AVa *va) { f_CallStaticVoidMethodV(e, c, mid, va); }
static void f_CallNonvirtualVoidMethodA(E e, JObject *o, void *c, void *mid, const JValue *a) { (void)c; f_CallVoidMethodA(e, o, mid, a); }
static void f_CallNonvirtualVoidMethodV(E e, JObject *o, void *c, void *mid, AVa *va) { (void)c; f_CallVoidMethodV(e, o, mid, va); }
void jnia_CallNonvirtualVoidMethod(E e, JObject *o, void *c, void *mid, AVa *va) { f_CallNonvirtualVoidMethodV(e, o, c, mid, va); }
void *jnia_CallNonvirtualObjectMethod(E e, JObject *o, void *c, void *mid, AVa *va) { return f_CallNonvirtualObjectMethodV(e, o, c, mid, va); }
int32_t jnia_CallNonvirtualIntMethod(E e, JObject *o, void *c, void *mid, AVa *va) { return f_CallNonvirtualIntMethodV(e, o, c, mid, va); }
uint8_t jnia_CallNonvirtualBooleanMethod(E e, JObject *o, void *c, void *mid, AVa *va) { return f_CallNonvirtualBooleanMethodV(e, o, c, mid, va); }

// Fields: kept per class by name (statics); instance fields are rare here and logged.
static void *f_GetFieldID(E e, void *c, const char *n, const char *s) { (void)e; LOG("java: GetFieldID %s.%s %s", ((JClass *)c)->name, n, s); return jni_method(c, n, s, false); }
static int static_slot(JClass *c, const char *n) {
    for (int i = 0; i < c->nstatics; i++) if (!strcmp(c->staticNames[i], n)) return i;
    if (c->nstatics == 32) return 31;
    strlcpy(c->staticNames[c->nstatics], n, 48);
    return c->nstatics++;
}
static void *f_GetStaticFieldID(E e, void *c, const char *n, const char *s) {
    (void)e; (void)s;
    JClass *cls = c;
    return (void *)(uintptr_t)(0x1000 + static_slot(cls, n));
}
#define FIELD(Name, T, field)                                                                               \
    static T f_Get##Name##Field(E e, JObject *o, void *fid) { (void)e; (void)o; LOG("java: Get%sField %s", #Name, M(fid)->name); return (T)0; } \
    static void f_Set##Name##Field(E e, JObject *o, void *fid, T v) { (void)e; (void)o; (void)v; LOG("java: Set%sField %s", #Name, M(fid)->name); } \
    static T f_GetStatic##Name##Field(E e, JClass *c, void *fid) { (void)e; return c->statics[(uintptr_t)fid - 0x1000].field; } \
    static void f_SetStatic##Name##Field(E e, JClass *c, void *fid, T v) { (void)e; c->statics[(uintptr_t)fid - 0x1000].field = v; }
FIELD(Object, void *, l)
FIELD(Boolean, uint8_t, z)
FIELD(Byte, int8_t, b)
FIELD(Char, uint16_t, c)
FIELD(Short, int16_t, s)
FIELD(Int, int32_t, i)
FIELD(Long, int64_t, j)
FIELD(Float, float, f)
FIELD(Double, double, d)

// Strings
static void *f_NewString(E e, const uint16_t *u, int32_t n) {
    (void)e;
    char *buf = malloc(n * 3 + 1), *p = buf;
    for (int i = 0; i < n; i++) {
        uint16_t c = u[i];
        if (c < 0x80) *p++ = (char)c;
        else if (c < 0x800) { *p++ = 0xc0 | (c >> 6); *p++ = 0x80 | (c & 63); }
        else { *p++ = 0xe0 | (c >> 12); *p++ = 0x80 | ((c >> 6) & 63); *p++ = 0x80 | (c & 63); }
    }
    *p = 0;
    JObject *s = jni_string(buf);
    free(buf);
    return s;
}
static int32_t utf16_len(const char *s) {
    int32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if ((*p & 0xc0) != 0x80) n += (*p >= 0xf0) ? 2 : 1;
    return n;
}
static int32_t f_GetStringLength(E e, JObject *s) { (void)e; return s && s->utf8 ? utf16_len(s->utf8) : 0; }
static const uint16_t *f_GetStringChars(E e, JObject *s, uint8_t *copy) {
    (void)e;
    if (copy) *copy = 1;
    int32_t n = f_GetStringLength(e, s);
    uint16_t *u = calloc(n + 1, 2);
    const unsigned char *p = (const unsigned char *)(s && s->utf8 ? s->utf8 : "");
    int i = 0;
    while (*p && i < n) {
        uint32_t c = *p++;
        if (c >= 0xe0) { c = (c & 15) << 12; c |= (*p++ & 63) << 6; c |= *p++ & 63; }
        else if (c >= 0xc0) { c = (c & 31) << 6; c |= *p++ & 63; }
        u[i++] = (uint16_t)c;
    }
    return u;
}
static void f_ReleaseStringChars(E e, JObject *s, const uint16_t *u) { (void)e; (void)s; free((void *)u); }
static void *f_NewStringUTF(E e, const char *s) { (void)e; return s ? jni_string(s) : NULL; }
static int32_t f_GetStringUTFLength(E e, JObject *s) { (void)e; return s && s->utf8 ? (int32_t)strlen(s->utf8) : 0; }
static const char *f_GetStringUTFChars(E e, JObject *s, uint8_t *copy) { (void)e; if (copy) *copy = 0; return s && s->utf8 ? s->utf8 : ""; }
static void f_ReleaseStringUTFChars(E e, JObject *s, const char *c) { (void)e; (void)s; (void)c; }
static void f_GetStringRegion(E e, JObject *s, int32_t start, int32_t len, uint16_t *buf) {
    const uint16_t *u = f_GetStringChars(e, s, NULL);
    memcpy(buf, u + start, len * 2);
    free((void *)u);
}
static void f_GetStringUTFRegion(E e, JObject *s, int32_t start, int32_t len, char *buf) { (void)e; memcpy(buf, s->utf8 + start, len); buf[len] = 0; }

// Arrays
static int32_t f_GetArrayLength(E e, JObject *a) { (void)e; return a ? a->length : 0; }
static void *f_NewObjectArray(E e, int32_t n, void *c, void *init) {
    (void)e; (void)c;
    JObject *a = jni_array(8, n);
    for (int i = 0; i < n; i++) ((void **)a->data)[i] = init;
    return a;
}
static void *f_GetObjectArrayElement(E e, JObject *a, int32_t i) { (void)e; return ((void **)a->data)[i]; }
static void f_SetObjectArrayElement(E e, JObject *a, int32_t i, void *v) { (void)e; ((void **)a->data)[i] = v; }
static void *f_NewArray1(E e, int32_t n) { (void)e; return jni_array(1, n); }
static void *f_NewArray2(E e, int32_t n) { (void)e; return jni_array(2, n); }
static void *f_NewArray4(E e, int32_t n) { (void)e; return jni_array(4, n); }
static void *f_NewArray8(E e, int32_t n) { (void)e; return jni_array(8, n); }
static void *f_GetArrayElements(E e, JObject *a, uint8_t *copy) { (void)e; if (copy) *copy = 0; return a ? a->data : NULL; }
static void f_ReleaseArrayElements(E e, JObject *a, void *p, int32_t mode) { (void)e; (void)a; (void)p; (void)mode; }
static void f_GetArrayRegion(E e, JObject *a, int32_t start, int32_t len, void *buf) { (void)e; memcpy(buf, (char *)a->data + start * a->elemSize, (size_t)len * a->elemSize); }
static void f_SetArrayRegion(E e, JObject *a, int32_t start, int32_t len, const void *buf) { (void)e; memcpy((char *)a->data + start * a->elemSize, buf, (size_t)len * a->elemSize); }
static void *f_GetPrimitiveArrayCritical(E e, JObject *a, uint8_t *copy) { return f_GetArrayElements(e, a, copy); }
static void f_ReleasePrimitiveArrayCritical(E e, JObject *a, void *p, int32_t m) { (void)e; (void)a; (void)p; (void)m; }
static const uint16_t *f_GetStringCritical(E e, JObject *s, uint8_t *copy) { return f_GetStringChars(e, s, copy); }
static void f_ReleaseStringCritical(E e, JObject *s, const uint16_t *u) { f_ReleaseStringChars(e, s, u); }

typedef struct { const char *name; const char *sig; void *fn; } NativeMethod;
static int32_t f_RegisterNatives(E e, JClass *c, const NativeMethod *m, int32_t n) {
    (void)e;
    for (int i = 0; i < n; i++) {
        JMethod *jm = jni_method(c, m[i].name, m[i].sig, true);
        jm->native = m[i].fn;
        LOG("java: native %s.%s%s registered", c->name, m[i].name, m[i].sig);
    }
    return 0;
}
static int32_t f_UnregisterNatives(E e, void *c) { (void)e; (void)c; return 0; }
static int32_t f_Monitor(E e, void *o) { (void)e; (void)o; return 0; }
static int32_t f_GetJavaVM(E e, void **vm) { (void)e; *vm = jni_vm(); return 0; }
static uint8_t f_ExceptionCheck(E e) { (void)e; return 0; }
static void *f_NewDirectByteBuffer(E e, void *addr, int64_t cap) {
    (void)e;
    JObject *o = jni_object(jni_class("java/nio/ByteBuffer"));
    o->data = addr;
    o->length = (int)cap;
    return o;
}
static void *f_GetDirectBufferAddress(E e, JObject *b) { (void)e; return b ? b->data : NULL; }
static int64_t f_GetDirectBufferCapacity(E e, JObject *b) { (void)e; return b ? b->length : 0; }
static int32_t f_GetObjectRefType(E e, void *o) { (void)e; (void)o; return 2; }
static void *f_FromReflected(E e, void *o) { (void)e; return o; }
static void *f_ToReflected(E e, void *c, void *m, uint8_t s) { (void)e; (void)c; (void)s; return m; }
static void *f_DefineClass(E e, const char *n, void *l, const void *b, int32_t len) { (void)e; (void)l; (void)b; (void)len; return jni_class(n); }

// Any slot we did not fill: report which one was called.
#define UNIMPL(i) static void u##i(void) { LOG("FATAL: JNI function %d not implemented", i); abort(); }
UNIMPL(0)

extern char jniv_CallObjectMethod[], jniv_CallBooleanMethod[], jniv_CallByteMethod[], jniv_CallCharMethod[], jniv_CallShortMethod[],
    jniv_CallIntMethod[], jniv_CallLongMethod[], jniv_CallFloatMethod[], jniv_CallDoubleMethod[], jniv_CallVoidMethod[],
    jniv_CallStaticObjectMethod[], jniv_CallStaticBooleanMethod[], jniv_CallStaticByteMethod[], jniv_CallStaticCharMethod[],
    jniv_CallStaticShortMethod[], jniv_CallStaticIntMethod[], jniv_CallStaticLongMethod[], jniv_CallStaticFloatMethod[],
    jniv_CallStaticDoubleMethod[], jniv_CallStaticVoidMethod[], jniv_NewObject[], jniv_CallNonvirtualVoidMethod[],
    jniv_CallNonvirtualObjectMethod[], jniv_CallNonvirtualIntMethod[], jniv_CallNonvirtualBooleanMethod[];

static void *g_table[233];
static void *g_env_ptr = g_table;   // JNIEnv* points at a pointer to the table
static void *g_invoke[8];
static void *g_vm_ptr = g_invoke;

static int32_t vm_Destroy(void *vm) { (void)vm; return 0; }
static int32_t vm_Attach(void *vm, void **env, void *args) { (void)vm; (void)args; *env = &g_env_ptr; elf_tls_thread_init(); return 0; }
static int32_t vm_Detach(void *vm) { (void)vm; return 0; }
static int32_t vm_GetEnv(void *vm, void **env, int32_t ver) { (void)vm; (void)ver; *env = &g_env_ptr; return 0; }

void *jni_env(void) { return &g_env_ptr; }
void *jni_vm(void) { return &g_vm_ptr; }

__attribute__((constructor)) static void jni_setup(void) {
    for (int i = 0; i < 233; i++) g_table[i] = (void *)u0;
    void **t = g_table;
    t[4] = f_GetVersion; t[5] = f_DefineClass; t[6] = f_FindClass; t[7] = f_FromReflected; t[8] = f_FromReflected;
    t[9] = f_ToReflected; t[10] = f_GetSuperclass; t[11] = f_IsAssignableFrom; t[12] = f_ToReflected; t[13] = f_Throw;
    t[14] = f_ThrowNew; t[15] = f_ExceptionOccurred; t[16] = f_ExceptionDescribe; t[17] = f_ExceptionClear; t[18] = f_FatalError;
    t[19] = f_PushLocalFrame; t[20] = f_PopLocalFrame; t[21] = f_NewRef; t[22] = f_DeleteRef; t[23] = f_DeleteRef;
    t[24] = f_IsSameObject; t[25] = f_NewRef; t[26] = f_EnsureLocalCapacity; t[27] = f_AllocObject; t[28] = jniv_NewObject;
    t[29] = f_NewObjectV; t[30] = f_NewObjectA; t[31] = f_GetObjectClass; t[32] = f_IsInstanceOf; t[33] = f_GetMethodID;
#define TRIPLE(base, V, Vv, A) t[base] = V; t[base + 1] = Vv; t[base + 2] = A;
    TRIPLE(34, jniv_CallObjectMethod, f_CallObjectMethodV, f_CallObjectMethodA)
    TRIPLE(37, jniv_CallBooleanMethod, f_CallBooleanMethodV, f_CallBooleanMethodA)
    TRIPLE(40, jniv_CallByteMethod, f_CallByteMethodV, f_CallByteMethodA)
    TRIPLE(43, jniv_CallCharMethod, f_CallCharMethodV, f_CallCharMethodA)
    TRIPLE(46, jniv_CallShortMethod, f_CallShortMethodV, f_CallShortMethodA)
    TRIPLE(49, jniv_CallIntMethod, f_CallIntMethodV, f_CallIntMethodA)
    TRIPLE(52, jniv_CallLongMethod, f_CallLongMethodV, f_CallLongMethodA)
    TRIPLE(55, jniv_CallFloatMethod, f_CallFloatMethodV, f_CallFloatMethodA)
    TRIPLE(58, jniv_CallDoubleMethod, f_CallDoubleMethodV, f_CallDoubleMethodA)
    TRIPLE(61, jniv_CallVoidMethod, f_CallVoidMethodV, f_CallVoidMethodA)
    TRIPLE(64, jniv_CallNonvirtualObjectMethod, f_CallNonvirtualObjectMethodV, f_CallNonvirtualObjectMethodA)
    TRIPLE(67, jniv_CallNonvirtualBooleanMethod, f_CallNonvirtualBooleanMethodV, f_CallNonvirtualBooleanMethodA)
    TRIPLE(70, u0, f_CallNonvirtualByteMethodV, f_CallNonvirtualByteMethodA)
    TRIPLE(73, u0, f_CallNonvirtualCharMethodV, f_CallNonvirtualCharMethodA)
    TRIPLE(76, u0, f_CallNonvirtualShortMethodV, f_CallNonvirtualShortMethodA)
    TRIPLE(79, jniv_CallNonvirtualIntMethod, f_CallNonvirtualIntMethodV, f_CallNonvirtualIntMethodA)
    TRIPLE(82, u0, f_CallNonvirtualLongMethodV, f_CallNonvirtualLongMethodA)
    TRIPLE(85, u0, f_CallNonvirtualFloatMethodV, f_CallNonvirtualFloatMethodA)
    TRIPLE(88, u0, f_CallNonvirtualDoubleMethodV, f_CallNonvirtualDoubleMethodA)
    TRIPLE(91, jniv_CallNonvirtualVoidMethod, f_CallNonvirtualVoidMethodV, f_CallNonvirtualVoidMethodA)
    t[94] = f_GetFieldID;
    t[95] = f_GetObjectField; t[96] = f_GetBooleanField; t[97] = f_GetByteField; t[98] = f_GetCharField; t[99] = f_GetShortField;
    t[100] = f_GetIntField; t[101] = f_GetLongField; t[102] = f_GetFloatField; t[103] = f_GetDoubleField;
    t[104] = f_SetObjectField; t[105] = f_SetBooleanField; t[106] = f_SetByteField; t[107] = f_SetCharField; t[108] = f_SetShortField;
    t[109] = f_SetIntField; t[110] = f_SetLongField; t[111] = f_SetFloatField; t[112] = f_SetDoubleField;
    t[113] = f_GetStaticMethodID;
    TRIPLE(114, jniv_CallStaticObjectMethod, f_CallStaticObjectMethodV, f_CallStaticObjectMethodA)
    TRIPLE(117, jniv_CallStaticBooleanMethod, f_CallStaticBooleanMethodV, f_CallStaticBooleanMethodA)
    TRIPLE(120, jniv_CallStaticByteMethod, f_CallStaticByteMethodV, f_CallStaticByteMethodA)
    TRIPLE(123, jniv_CallStaticCharMethod, f_CallStaticCharMethodV, f_CallStaticCharMethodA)
    TRIPLE(126, jniv_CallStaticShortMethod, f_CallStaticShortMethodV, f_CallStaticShortMethodA)
    TRIPLE(129, jniv_CallStaticIntMethod, f_CallStaticIntMethodV, f_CallStaticIntMethodA)
    TRIPLE(132, jniv_CallStaticLongMethod, f_CallStaticLongMethodV, f_CallStaticLongMethodA)
    TRIPLE(135, jniv_CallStaticFloatMethod, f_CallStaticFloatMethodV, f_CallStaticFloatMethodA)
    TRIPLE(138, jniv_CallStaticDoubleMethod, f_CallStaticDoubleMethodV, f_CallStaticDoubleMethodA)
    TRIPLE(141, jniv_CallStaticVoidMethod, f_CallStaticVoidMethodV, f_CallStaticVoidMethodA)
    t[144] = f_GetStaticFieldID;
    t[145] = f_GetStaticObjectField; t[146] = f_GetStaticBooleanField; t[147] = f_GetStaticByteField; t[148] = f_GetStaticCharField;
    t[149] = f_GetStaticShortField; t[150] = f_GetStaticIntField; t[151] = f_GetStaticLongField; t[152] = f_GetStaticFloatField;
    t[153] = f_GetStaticDoubleField; t[154] = f_SetStaticObjectField; t[155] = f_SetStaticBooleanField; t[156] = f_SetStaticByteField;
    t[157] = f_SetStaticCharField; t[158] = f_SetStaticShortField; t[159] = f_SetStaticIntField; t[160] = f_SetStaticLongField;
    t[161] = f_SetStaticFloatField; t[162] = f_SetStaticDoubleField;
    t[163] = f_NewString; t[164] = f_GetStringLength; t[165] = f_GetStringChars; t[166] = f_ReleaseStringChars; t[167] = f_NewStringUTF;
    t[168] = f_GetStringUTFLength; t[169] = f_GetStringUTFChars; t[170] = f_ReleaseStringUTFChars; t[171] = f_GetArrayLength;
    t[172] = f_NewObjectArray; t[173] = f_GetObjectArrayElement; t[174] = f_SetObjectArrayElement;
    t[175] = f_NewArray1; t[176] = f_NewArray1; t[177] = f_NewArray2; t[178] = f_NewArray2; t[179] = f_NewArray4;
    t[180] = f_NewArray8; t[181] = f_NewArray4; t[182] = f_NewArray8;
    for (int i = 183; i <= 190; i++) t[i] = f_GetArrayElements;
    for (int i = 191; i <= 198; i++) t[i] = f_ReleaseArrayElements;
    for (int i = 199; i <= 206; i++) t[i] = f_GetArrayRegion;
    for (int i = 207; i <= 214; i++) t[i] = f_SetArrayRegion;
    t[215] = f_RegisterNatives; t[216] = f_UnregisterNatives; t[217] = f_Monitor; t[218] = f_Monitor; t[219] = f_GetJavaVM;
    t[220] = f_GetStringRegion; t[221] = f_GetStringUTFRegion; t[222] = f_GetPrimitiveArrayCritical;
    t[223] = f_ReleasePrimitiveArrayCritical; t[224] = f_GetStringCritical; t[225] = f_ReleaseStringCritical;
    t[226] = f_NewRef; t[227] = f_DeleteRef; t[228] = f_ExceptionCheck; t[229] = f_NewDirectByteBuffer;
    t[230] = f_GetDirectBufferAddress; t[231] = f_GetDirectBufferCapacity; t[232] = f_GetObjectRefType;
    g_invoke[3] = vm_Destroy; g_invoke[4] = vm_Attach; g_invoke[5] = vm_Detach; g_invoke[6] = vm_GetEnv; g_invoke[7] = vm_Attach;
    (void)class_object;
}
