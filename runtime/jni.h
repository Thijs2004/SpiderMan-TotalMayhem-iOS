// A small Java world for libspiderman.so: JavaVM / JNIEnv tables, objects, strings, arrays, and Java
// classes whose methods are implemented natively (java.m). Unknown classes and methods are created on
// demand and every call to them is logged, so missing pieces show up in smtm.log instead of crashing.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef union { uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i; int64_t j; float f; double d; void *l; } JValue;

typedef struct JClass JClass;
typedef struct JObject JObject;
typedef JValue (*JImpl)(JObject *self, const JValue *args);

typedef struct JMethod {
    JClass *cls;
    char name[64];
    char sig[192];
    bool isStatic;
    JImpl impl;          // our Java implementation, or NULL
    void *native;        // function registered through RegisterNatives
    int nargs;
    char argtypes[32];   // one letter per argument: Z B C S I J F D L
    char ret;
} JMethod;

struct JClass {
    char name[160];
    JMethod *methods[96];
    int nmethods;
    JValue statics[32];
    char staticNames[32][48];
    int nstatics;
};

enum { JK_OBJECT, JK_STRING, JK_ARRAY, JK_CLASS };
struct JObject {
    int kind;
    JClass *cls;
    // strings
    char *utf8;
    // arrays
    int length, elemSize;
    void *data;
    // plain objects: a few fields by name
    void *native;
};

void *jni_vm(void);          // JavaVM*
void *jni_env(void);         // JNIEnv*

JClass *jni_class(const char *name);                     // finds or creates
JMethod *jni_method(JClass *cls, const char *name, const char *sig, bool isStatic);
void jni_define(const char *cls, const char *name, const char *sig, bool isStatic, JImpl impl);
/// Native registered by the host for cls.name (sig may be NULL), or NULL.
void *jni_native(const char *cls, const char *name);

JObject *jni_string(const char *utf8);
const char *jni_cstr(JObject *s);
JObject *jni_object(JClass *cls);
JObject *jni_array(int elemSize, int length);
