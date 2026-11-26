// Replaces dynarmic's A32 JIT (Dynarmic::A32::Jit::Run is hooked to arm32_run): iOS can't execute generated
// code, so the few guest paths the recompiled engine doesn't cover run on this interpreter instead.
// Guest = the 2010 Gameloft engine: ARMv5TE, ARM + Thumb-1, soft float (no VFP/NEON, no Thumb-2).
// It works on dynarmic's own state (registers, CPSR, halt flags) and hands every SVC to the runtime's
// Cpu::CallSVC, which runs imports (libc, GL, JNI) and enters recompiled functions; afterwards it continues
// wherever the runtime left the PC.
#include "elfload.h"
#include "host_syms.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LOG elf_log

static uintptr_t g_host;
static uint8_t *g_mem;   // guest address 0

// dynarmic layout (Jit -> Impl): see the disassembly of Jit::Regs / Cpsr / HaltExecution.
#define IMPL(jit) (*(uint8_t **)((uint8_t *)(jit) + 8))
#define STATE(impl) ((impl) + 0x180)
#define REGS(impl) ((uint32_t *)((impl) + 0x198))
#define HALT(impl) ((_Atomic uint32_t *)((impl) + 0x77c))

typedef uint32_t (*CpsrFn)(void *state);
typedef void (*SetCpsrFn)(void *state, uint32_t);
typedef void *(*CurrentFn)(void);
typedef void (*SvcFn)(void *cpu, uint32_t imm);
typedef void (*ExcFn)(void *cpu, uint32_t pc, uint32_t exc);
typedef void *(*EntryFn)(uint32_t addr);
typedef void (*AotFn)(void *ctx);
static EntryFn direct_entry;

static CpsrFn get_cpsr;
static SetCpsrFn set_cpsr;
static CurrentFn cpu_current;
static SvcFn call_svc;
static ExcFn exception_raised;

uint32_t arm32_run(void *jit);

void arm32_install(uintptr_t hostBase) {
    g_host = hostBase;
    get_cpsr = (CpsrFn)(hostBase + ELF_host__ZNK8Dynarmic7Backend5Arm6411A32JitState4CpsrEv);
    set_cpsr = (SetCpsrFn)(hostBase + ELF_host__ZN8Dynarmic7Backend5Arm6411A32JitState7SetCpsrEj);
    cpu_current = (CurrentFn)(hostBase + ELF_host__ZN3Cpu7currentEv);
    call_svc = (SvcFn)(hostBase + ELF_host__ZN3Cpu7CallSVCEj);
    exception_raised = (ExcFn)(hostBase + ELF_host__ZN3Cpu15ExceptionRaisedEjN8Dynarmic3A329ExceptionE);
    direct_entry = (EntryFn)(hostBase + ELF_host__ZN3aot12direct_entryEj);
    *(void **)(hostBase + HOOK_host__ZN8Dynarmic3A323Jit3RunEv) = (void *)arm32_run;
}

// ---------------------------------------------------------------- state
typedef struct {
    uint32_t *r;          // dynarmic's register array (r15 = pc)
    uint8_t *impl;
    uint32_t n, z, c, v, q, t;
    uint32_t cpsr_rest;   // mode and other bits kept as they were
    uint64_t steps;
    union { float f[32]; double d[16]; uint32_t u[32]; } vfp;   // used by the runtime's float helpers
    uint32_t fpnzcv;
} S;

static inline void load_cpsr(S *s) {
    uint32_t p = get_cpsr(STATE(s->impl));
    s->n = p >> 31; s->z = (p >> 30) & 1; s->c = (p >> 29) & 1; s->v = (p >> 28) & 1; s->q = (p >> 27) & 1;
    s->t = (p >> 5) & 1;
    s->cpsr_rest = p & 0x07ffffdf;
}
static inline uint32_t make_cpsr(S *s) {
    return s->n << 31 | s->z << 30 | s->c << 29 | s->v << 28 | s->q << 27 | s->t << 5 | s->cpsr_rest;
}
static inline void store_cpsr(S *s) { set_cpsr(STATE(s->impl), make_cpsr(s)); }

// guest memory: one flat 4 GB reservation
static inline uint8_t *M(uint32_t a) { return g_mem + a; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, M(a), 4); return v; }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, M(a), 2); return v; }
static inline uint8_t rd8(uint32_t a) { return *M(a); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(M(a), &v, 4); }
static inline void wr16(uint32_t a, uint16_t v) { memcpy(M(a), &v, 2); }
static inline void wr8(uint32_t a, uint8_t v) { *M(a) = v; }

static inline int cond_ok(S *s, uint32_t c) {
    switch (c) {
    case 0: return s->z;
    case 1: return !s->z;
    case 2: return s->c;
    case 3: return !s->c;
    case 4: return s->n;
    case 5: return !s->n;
    case 6: return s->v;
    case 7: return !s->v;
    case 8: return s->c && !s->z;
    case 9: return !s->c || s->z;
    case 10: return s->n == s->v;
    case 11: return s->n != s->v;
    case 12: return !s->z && s->n == s->v;
    case 13: return s->z || s->n != s->v;
    default: return 1;
    }
}

static inline uint32_t add_flags(S *s, uint32_t a, uint32_t b, uint32_t cin, int set) {
    uint64_t u = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)u;
    if (set) {
        s->n = r >> 31; s->z = r == 0; s->c = (uint32_t)(u >> 32);
        s->v = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    }
    return r;
}
static inline void nz(S *s, uint32_t r) { s->n = r >> 31; s->z = r == 0; }

// barrel shifter; *co = carry out
static inline uint32_t shift_imm(S *s, uint32_t val, uint32_t type, uint32_t amt, uint32_t *co) {
    *co = s->c;
    switch (type) {
    case 0: if (amt) { *co = (val >> (32 - amt)) & 1; val <<= amt; } return val;
    case 1: if (!amt) amt = 32;
        if (amt == 32) { *co = val >> 31; return 0; } *co = (val >> (amt - 1)) & 1; return val >> amt;
    case 2: if (!amt) amt = 32;
        if (amt == 32) { *co = val >> 31; return (uint32_t)((int32_t)val >> 31); } *co = (val >> (amt - 1)) & 1; return (uint32_t)((int32_t)val >> amt);
    default:
        if (!amt) { *co = val & 1; return (s->c << 31) | (val >> 1); }   // RRX
        *co = (val >> (amt - 1)) & 1; return (val >> amt) | (val << (32 - amt));
    }
}
static inline uint32_t shift_reg(S *s, uint32_t val, uint32_t type, uint32_t amt, uint32_t *co) {
    amt &= 0xff;
    *co = s->c;
    if (!amt) return val;
    switch (type) {
    case 0: if (amt < 32) { *co = (val >> (32 - amt)) & 1; return val << amt; } *co = amt == 32 ? val & 1 : 0; return 0;
    case 1: if (amt < 32) { *co = (val >> (amt - 1)) & 1; return val >> amt; } *co = amt == 32 ? val >> 31 : 0; return 0;
    case 2: if (amt < 32) { *co = (val >> (amt - 1)) & 1; return (uint32_t)((int32_t)val >> amt); } *co = val >> 31; return (uint32_t)((int32_t)val >> 31);
    default: amt &= 31; if (!amt) { *co = val >> 31; return val; } *co = (val >> (amt - 1)) & 1; return (val >> amt) | (val << (32 - amt));
    }
}

static inline int32_t sat32(S *s, int64_t v) {
    if (v > INT32_MAX) { s->q = 1; return INT32_MAX; }
    if (v < INT32_MIN) { s->q = 1; return INT32_MIN; }
    return (int32_t)v;
}

// Branch with interworking (BX semantics): bit 0 selects Thumb.
static inline void bx(S *s, uint32_t a) { s->t = a & 1; s->r[15] = a & ~1u; }

// Returns 1 when the run must stop (halt requested).
static int svc(S *s, uint32_t imm, uint32_t next) {
    s->r[15] = next;
    store_cpsr(s);
    call_svc(cpu_current(), imm);
    load_cpsr(s);
    return atomic_load(HALT(s->impl)) != 0;
}

static void undefined(S *s, uint32_t pc, uint32_t insn) {
    LOG("arm32: undefined %s instruction %08x at %08x", s->t ? "Thumb" : "ARM", insn, pc);
    store_cpsr(s);
    exception_raised(cpu_current(), pc, 0);   // Exception::UndefinedInstruction
    load_cpsr(s);
}

// ---------------------------------------------------------------- VFP (the subset the runtime's
// __aeabi float/double helpers use: vmov, vadd/vsub/vmul/vdiv, vcmp, vmrs, vcvt)
static inline uint32_t sreg(uint32_t v, uint32_t bit) { return v * 2 + bit; }      // Vx:X
static inline uint32_t dreg(uint32_t v, uint32_t bit) { return bit * 16 + v; }     // X:Vx
static void fcmp(S *s, double a, double b) {
    if (a != a || b != b) s->fpnzcv = 0x3;
    else if (a == b) s->fpnzcv = 0x6;
    else if (a < b) s->fpnzcv = 0x8;
    else s->fpnzcv = 0x2;
}
static int32_t to_s32(double x, int trunc) {
    if (x != x) return 0;
    double t = trunc ? (x < 0 ? -__builtin_floor(-x) : __builtin_floor(x)) : __builtin_nearbyint(x);
    if (t >= 2147483647.0) return INT32_MAX;
    if (t <= -2147483648.0) return INT32_MIN;
    return (int32_t)t;
}
static uint32_t to_u32(double x, int trunc) {
    if (x != x || x <= 0) return 0;
    double t = trunc ? __builtin_floor(x) : __builtin_nearbyint(x);
    if (t >= 4294967295.0) return UINT32_MAX;
    return (uint32_t)t;
}
// Returns 0 if handled, -1 if not a VFP instruction we know.
static int vfp(S *s, uint32_t i, uint32_t pc) {
    uint32_t *r = s->r;
    if ((i & 0x0fe00f7f) == 0x0e000a10) {   // VMOV Sn <-> Rt
        uint32_t sn = sreg((i >> 16) & 15, (i >> 7) & 1), rt = (i >> 12) & 15;
        if (i & (1 << 20)) r[rt] = s->vfp.u[sn]; else s->vfp.u[sn] = r[rt];
        return 0;
    }
    if ((i & 0x0fe00fd0) == 0x0c400b10) {   // VMOV Dm <-> Rt, Rt2
        uint32_t dm = dreg(i & 15, (i >> 5) & 1), rt = (i >> 12) & 15, rt2 = (i >> 16) & 15;
        if (i & (1 << 20)) { r[rt] = s->vfp.u[dm * 2]; r[rt2] = s->vfp.u[dm * 2 + 1]; }
        else { s->vfp.u[dm * 2] = r[rt]; s->vfp.u[dm * 2 + 1] = r[rt2]; }
        return 0;
    }
    if ((i & 0x0fffffff) == 0x0ef1fa10) {   // VMRS APSR_nzcv, FPSCR
        s->n = s->fpnzcv >> 3; s->z = (s->fpnzcv >> 2) & 1; s->c = (s->fpnzcv >> 1) & 1; s->v = s->fpnzcv & 1;
        return 0;
    }
    if ((i & 0x0f000e10) == 0x0d000a00) {   // VLDR / VSTR
        uint32_t dbl = (i >> 8) & 1, U = (i >> 23) & 1, rn = (i >> 16) & 15;
        uint32_t base = rn == 15 ? (pc + 8) & ~3u : r[rn];
        uint32_t a = U ? base + (i & 0xff) * 4 : base - (i & 0xff) * 4;
        uint32_t vd = (i >> 12) & 15, D = (i >> 22) & 1;
        uint32_t idx = dbl ? dreg(vd, D) * 2 : sreg(vd, D);
        if (i & (1 << 20)) { s->vfp.u[idx] = rd32(a); if (dbl) s->vfp.u[idx + 1] = rd32(a + 4); }
        else { wr32(a, s->vfp.u[idx]); if (dbl) wr32(a + 4, s->vfp.u[idx + 1]); }
        return 0;
    }
    if ((i & 0x0f000e10) != 0x0e000a00) return -1;   // VFP data processing only
    uint32_t dbl = (i >> 8) & 1;
    uint32_t vd = (i >> 12) & 15, vn = (i >> 16) & 15, vm = i & 15;
    uint32_t D = (i >> 22) & 1, N = (i >> 7) & 1, Mb = (i >> 5) & 1;
    uint32_t opc1 = ((i >> 20) & 0xb), opc3 = (i >> 6) & 1;
#define SD(x) s->vfp.f[x]
#define DD(x) s->vfp.d[x]
    uint32_t d = dbl ? dreg(vd, D) : sreg(vd, D), n = dbl ? dreg(vn, N) : sreg(vn, N), m = dbl ? dreg(vm, Mb) : sreg(vm, Mb);
    switch (opc1) {
    case 0x2:   // VMUL (opc3 0) / VNMUL
        if (dbl) DD(d) = opc3 ? -(DD(n) * DD(m)) : DD(n) * DD(m); else SD(d) = opc3 ? -(SD(n) * SD(m)) : SD(n) * SD(m);
        return 0;
    case 0x3:   // VADD / VSUB
        if (dbl) DD(d) = opc3 ? DD(n) - DD(m) : DD(n) + DD(m); else SD(d) = opc3 ? SD(n) - SD(m) : SD(n) + SD(m);
        return 0;
    case 0x8:   // VDIV
        if (dbl) DD(d) = DD(n) / DD(m); else SD(d) = SD(n) / SD(m);
        return 0;
    case 0xb: break;
    default: return -1;
    }
    // other VFP data-processing instructions (opc1 = 1x11): opc2 in Vn
    switch (vn) {
    case 0:   // VMOV reg (opc3 1) / VABS (opc3 1 with bit7)
        if (((i >> 6) & 3) == 1) { if (dbl) DD(d) = DD(m); else SD(d) = SD(m); return 0; }
        if (((i >> 6) & 3) == 3) { if (dbl) DD(d) = __builtin_fabs(DD(m)); else SD(d) = __builtin_fabsf(SD(m)); return 0; }
        return -1;
    case 1:   // VNEG / VSQRT
        if (((i >> 6) & 3) == 1) { if (dbl) DD(d) = -DD(m); else SD(d) = -SD(m); return 0; }
        if (dbl) DD(d) = __builtin_sqrt(DD(m)); else SD(d) = __builtin_sqrtf(SD(m));
        return 0;
    case 4: fcmp(s, dbl ? DD(d) : SD(d), dbl ? DD(m) : SD(m)); return 0;      // VCMP
    case 5: fcmp(s, dbl ? DD(d) : SD(d), 0.0); return 0;                       // VCMP #0
    case 7: {   // VCVT between double and single
        if (dbl) { uint32_t sd = sreg(vd, D); SD(sd) = (float)DD(m); }
        else { uint32_t dd = dreg(vd, D); DD(dd) = (double)SD(m); }
        return 0;
    }
    case 8: {   // VCVT from integer (source is a single register)
        uint32_t sm = sreg(vm, Mb), sgn = N;
        double v = sgn ? (double)(int32_t)s->vfp.u[sm] : (double)s->vfp.u[sm];
        if (dbl) DD(d) = v; else SD(d) = (float)v;
        return 0;
    }
    case 12: case 13: {   // VCVT to integer (result in a single register)
        uint32_t sd = sreg(vd, D), trunc = N;
        double v = dbl ? DD(m) : SD(m);
        s->vfp.u[sd] = vn == 13 ? (uint32_t)to_s32(v, trunc) : to_u32(v, trunc);
        return 0;
    }
    }
#undef SD
#undef DD
    return -1;
}

// ---------------------------------------------------------------- ARM
// Returns 1 to stop the run loop.
static int arm_step(S *s) {
    uint32_t *r = s->r;
    uint32_t pc = r[15];
    uint32_t i = rd32(pc);
    uint32_t cond = i >> 28;
    r[15] = pc + 4;
#define RV(n) ((n) == 15 ? pc + 8 : r[n])
    if (cond == 0xF) {
        if ((i & 0x0e000000) == 0x0a000000) {   // BLX imm
            int32_t off = ((int32_t)(i << 8) >> 6) | ((i >> 23) & 2);
            r[14] = pc + 4;
            s->t = 1;
            r[15] = pc + 8 + off;
            return 0;
        }
        if ((i & 0x0d70f000) == 0x0550f000) return 0;   // PLD
        undefined(s, pc, i);
        return 1;
    }
    if (!cond_ok(s, cond)) return 0;
    uint32_t op = (i >> 25) & 7;
    switch (op) {
    case 0: case 1: {
        if (op == 0 && (i & 0x90) == 0x90) {
            // multiplies, swap, extra loads/stores
            uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
            if ((i & 0x60) == 0) {
                if ((i & 0x0fc000f0) == 0x00000090) {   // MUL/MLA
                    uint32_t res = r[rm] * r[rs] + ((i >> 21) & 1 ? r[rd] : 0);
                    r[rn] = res;
                    if (i & (1 << 20)) nz(s, res);
                    return 0;
                }
                if ((i & 0x0f8000f0) == 0x00800090) {   // UMULL/UMLAL/SMULL/SMLAL
                    uint64_t res;
                    if (i & (1 << 22)) res = (uint64_t)((int64_t)(int32_t)r[rm] * (int32_t)r[rs]);
                    else res = (uint64_t)r[rm] * r[rs];
                    if (i & (1 << 21)) res += ((uint64_t)r[rn] << 32) | r[rd];
                    r[rd] = (uint32_t)res; r[rn] = (uint32_t)(res >> 32);
                    if (i & (1 << 20)) { s->n = (uint32_t)(res >> 63); s->z = res == 0; }
                    return 0;
                }
                if ((i & 0x0f900ff0) == 0x01800f90 || (i & 0x0f900fff) == 0x01900f9f) {   // LDREX/STREX (B/H/D)
                    uint32_t sz = (i >> 21) & 3, a = r[rn];
                    if (i & (1 << 20)) {
                        if (sz == 0) r[rd] = rd32(a);
                        else if (sz == 1) { r[rd] = rd32(a); r[rd + 1] = rd32(a + 4); }
                        else if (sz == 2) r[rd] = rd8(a);
                        else r[rd] = rd16(a);
                    } else {
                        uint32_t rt = rm;
                        if (sz == 0) wr32(a, r[rt]);
                        else if (sz == 1) { wr32(a, r[rt]); wr32(a + 4, r[rt + 1]); }
                        else if (sz == 2) wr8(a, (uint8_t)r[rt]);
                        else wr16(a, (uint16_t)r[rt]);
                        r[rd] = 0;   // single core view: always succeeds
                    }
                    return 0;
                }
                if ((i & 0x0ff000f0) == 0x00600090) {   // MLS (ARMv7, used by the runtime's helpers)
                    r[rn] = r[(i >> 12) & 15] - r[rm] * r[rs];
                    return 0;
                }
                if ((i & 0x0fb00ff0) == 0x01000090) {   // SWP/SWPB
                    uint32_t a = r[rn];
                    if (i & (1 << 22)) { uint32_t t = rd8(a); wr8(a, (uint8_t)r[rm]); r[rd] = t; }
                    else { uint32_t t = rd32(a); wr32(a, r[rm]); r[rd] = t; }
                    return 0;
                }
                undefined(s, pc, i);
                return 1;
            }
            // LDRH/STRH/LDRSB/LDRSH/LDRD/STRD
            uint32_t P = (i >> 24) & 1, U = (i >> 23) & 1, I = (i >> 22) & 1, W = (i >> 21) & 1, L = (i >> 20) & 1;
            uint32_t off = I ? (((i >> 4) & 0xf0) | (i & 15)) : r[rm];
            uint32_t base = RV(rn);
            uint32_t addr = P ? (U ? base + off : base - off) : base;
            uint32_t sh = (i >> 5) & 3;
            if (L) {
                if (sh == 1) r[rd] = rd16(addr);
                else if (sh == 2) r[rd] = (uint32_t)(int32_t)(int8_t)rd8(addr);
                else r[rd] = (uint32_t)(int32_t)(int16_t)rd16(addr);
            } else {
                if (sh == 1) wr16(addr, (uint16_t)r[rd]);
                else if (sh == 2) { r[rd] = rd32(addr); r[rd + 1] = rd32(addr + 4); }       // LDRD
                else { wr32(addr, r[rd]); wr32(addr + 4, r[rd + 1]); }                         // STRD
            }
            if (!P) addr = U ? base + off : base - off;
            if ((!P || W) && !(L && rn == rd)) r[rn] = addr;
            if (L && rd == 15) bx(s, r[15]);
            return 0;
        }
        if (op == 0 && (i & 0x01900000) == 0x01000000) {
            // miscellaneous: MRS, MSR reg, BX, BLX reg, CLZ, QADD.., BKPT, SMLAxy..
            if ((i & 0x0fbf0fff) == 0x010f0000) { r[(i >> 12) & 15] = make_cpsr(s); return 0; }   // MRS
            if ((i & 0x0fb0fff0) == 0x0120f000) {   // MSR reg
                uint32_t v = r[i & 15];
                if (i & (1 << 19)) { s->n = v >> 31; s->z = (v >> 30) & 1; s->c = (v >> 29) & 1; s->v = (v >> 28) & 1; s->q = (v >> 27) & 1; }
                return 0;
            }
            if ((i & 0x0ffffff0) == 0x012fff10) { bx(s, r[i & 15]); return 0; }                    // BX
            if ((i & 0x0ffffff0) == 0x012fff30) { uint32_t t = r[i & 15]; r[14] = pc + 4; bx(s, t); return 0; }   // BLX reg
            if ((i & 0x0fff0ff0) == 0x016f0f10) { uint32_t v = r[i & 15]; r[(i >> 12) & 15] = v ? __builtin_clz(v) : 32; return 0; }   // CLZ
            if ((i & 0x0f900ff0) == 0x01000050) {   // QADD/QSUB/QDADD/QDSUB
                int32_t a = (int32_t)r[i & 15], b = (int32_t)r[(i >> 16) & 15];
                uint32_t o = (i >> 21) & 3;
                if (o & 2) b = sat32(s, (int64_t)b * 2);
                r[(i >> 12) & 15] = (uint32_t)sat32(s, (o & 1) ? (int64_t)a - b : (int64_t)a + b);
                return 0;
            }
            if ((i & 0x0ff000f0) == 0x01200070) { undefined(s, pc, i); return 1; }                 // BKPT
            if ((i & 0x0f900090) == 0x01000080) {   // SMLAxy / SMLAWy / SMULWy / SMLALxy / SMULxy
                uint32_t rd = (i >> 16) & 15, rn = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
                int x = (i >> 5) & 1, y = (i >> 6) & 1;
                int32_t a = x ? (int16_t)(r[rm] >> 16) : (int16_t)r[rm];
                int32_t b = y ? (int16_t)(r[rs] >> 16) : (int16_t)r[rs];
                switch ((i >> 21) & 3) {
                case 0: { int64_t p = (int64_t)a * b + (int32_t)r[rn]; if (p != (int32_t)p) s->q = 1; r[rd] = (uint32_t)p; break; }
                case 1: {
                    int32_t m = (int32_t)r[rm];
                    int64_t p = ((int64_t)m * b) >> 16;
                    if (x) { int64_t acc = p + (int32_t)r[rn]; if (acc != (int32_t)acc) s->q = 1; r[rd] = (uint32_t)acc; }   // SMLAWy
                    else r[rd] = (uint32_t)p;                                                                            // SMULWy
                    break;
                }
                case 2: { int64_t acc = ((int64_t)r[rd] << 32 | r[rn]) + (int64_t)a * b; r[rn] = (uint32_t)acc; r[rd] = (uint32_t)((uint64_t)acc >> 32); break; }
                default: r[rd] = (uint32_t)(a * b); break;
                }
                return 0;
            }
            if ((i & 0x0fb00000) == 0x01200000 && (i & 0xf0) == 0) { return 0; }   // MSR (other fields): ignore
        }
        if (op == 1 && (i & 0x0fb00000) == 0x03000000) {   // MOVW / MOVT (ARMv7, runtime helpers)
            uint32_t imm = ((i >> 4) & 0xf000) | (i & 0xfff), rd = (i >> 12) & 15;
            if (i & (1 << 22)) r[rd] = (r[rd] & 0xffff) | (imm << 16); else r[rd] = imm;
            return 0;
        }
        if (op == 1 && (i & 0x01b00000) == 0x01200000) {   // MSR imm
            uint32_t rot = ((i >> 8) & 15) * 2, imm = i & 0xff;
            uint32_t v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
            if (i & (1 << 19)) { s->n = v >> 31; s->z = (v >> 30) & 1; s->c = (v >> 29) & 1; s->v = (v >> 28) & 1; s->q = (v >> 27) & 1; }
            return 0;
        }
        // data processing
        uint32_t opc = (i >> 21) & 15, S_ = (i >> 20) & 1, rn = (i >> 16) & 15, rd = (i >> 12) & 15;
        uint32_t op2, co;
        if (op == 1) {
            uint32_t rot = ((i >> 8) & 15) * 2, imm = i & 0xff;
            op2 = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
            co = rot ? op2 >> 31 : s->c;
        } else if (i & 0x10) {
            uint32_t rm = i & 15, rs = (i >> 8) & 15;
            uint32_t vm = rm == 15 ? pc + 12 : r[rm];
            op2 = shift_reg(s, vm, (i >> 5) & 3, r[rs], &co);
        } else {
            op2 = shift_imm(s, RV(i & 15), (i >> 5) & 3, (i >> 7) & 31, &co);
        }
        uint32_t a = (op == 0 && (i & 0x10) && rn == 15) ? pc + 12 : RV(rn), res = 0;
        int logical = 0, write = 1;
        switch (opc) {
        case 0: res = a & op2; logical = 1; break;
        case 1: res = a ^ op2; logical = 1; break;
        case 2: res = add_flags(s, a, ~op2, 1, S_); break;
        case 3: res = add_flags(s, op2, ~a, 1, S_); break;
        case 4: res = add_flags(s, a, op2, 0, S_); break;
        case 5: res = add_flags(s, a, op2, s->c, S_); break;
        case 6: res = add_flags(s, a, ~op2, s->c, S_); break;
        case 7: res = add_flags(s, op2, ~a, s->c, S_); break;
        case 8: res = a & op2; logical = 1; write = 0; break;
        case 9: res = a ^ op2; logical = 1; write = 0; break;
        case 10: add_flags(s, a, ~op2, 1, 1); write = 0; break;
        case 11: add_flags(s, a, op2, 0, 1); write = 0; break;
        case 12: res = a | op2; logical = 1; break;
        case 13: res = op2; logical = 1; break;
        case 14: res = a & ~op2; logical = 1; break;
        case 15: res = ~op2; logical = 1; break;
        }
        if (logical && S_) { nz(s, res); s->c = co; }
        if (write) {
            r[rd] = res;
            if (rd == 15) {
                if (S_) { LOG("arm32: data processing with S into pc at %08x", pc); }
                r[15] = res & ~3u;
            }
        }
        return 0;
    }
    case 2: case 3: {   // LDR/STR/LDRB/STRB
        if (op == 3 && (i & 0x10)) {   // ARMv6/v7 media instructions (the runtime's own helpers use a few)
            uint32_t rd = (i >> 12) & 15, rn = (i >> 16) & 15, rm = i & 15;
            if ((i & 0x0fd0f0f0) == 0x0710f010) {   // SDIV / UDIV (Rd = Rn / Rm; x/0 = 0)
                uint32_t d = (i >> 16) & 15, n = i & 15, m = (i >> 8) & 15;
                if (!r[m]) r[d] = 0;
                else if (i & (1 << 21)) r[d] = r[n] / r[m];
                else if ((int32_t)r[n] == INT32_MIN && (int32_t)r[m] == -1) r[d] = (uint32_t)INT32_MIN;
                else r[d] = (uint32_t)((int32_t)r[n] / (int32_t)r[m]);
                return 0;
            }
            if ((i & 0x0f8003f0) == 0x06800070) {   // SXTB/SXTH/UXTB/UXTH (+ add forms)
                uint32_t rot = ((i >> 10) & 3) * 8, v = rot ? (r[rm] >> rot) | (r[rm] << (32 - rot)) : r[rm], x;
                switch ((i >> 20) & 7) {
                case 2: x = (uint32_t)(int32_t)(int8_t)v; break;
                case 3: x = (uint32_t)(int32_t)(int16_t)v; break;
                case 6: x = v & 0xff; break;
                case 7: x = v & 0xffff; break;
                case 4: x = v & 0x00ff00ff; break;                                     // UXTB16
                default: x = ((uint32_t)(int32_t)(int8_t)v & 0xffff) | ((uint32_t)(int8_t)(v >> 16) << 16); break;   // SXTB16
                }
                r[rd] = rn == 15 ? x : r[rn] + x;
                return 0;
            }
            if ((i & 0x0fff0ff0) == 0x06bf0f30) { r[rd] = __builtin_bswap32(r[rm]); return 0; }                       // REV
            if ((i & 0x0fff0ff0) == 0x06bf0fb0) { uint32_t v = r[rm]; r[rd] = ((v & 0x00ff00ff) << 8) | ((v >> 8) & 0x00ff00ff); return 0; }   // REV16
            if ((i & 0x0fff0ff0) == 0x06ff0fb0) { r[rd] = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)r[rm]); return 0; }   // REVSH
            if ((i & 0x0fa00070) == 0x07a00050) {   // UBFX / SBFX
                uint32_t lsb = (i >> 7) & 31, w = ((i >> 16) & 31) + 1, v = r[rm] >> lsb;
                uint32_t mask = w == 32 ? 0xffffffffu : (1u << w) - 1;
                v &= mask;
                if (!(i & (1 << 22)) && w < 32 && (v >> (w - 1)) & 1) v |= ~mask;   // SBFX sign-extends
                r[rd] = v;
                return 0;
            }
            if ((i & 0x0fe00070) == 0x07c00010) {   // BFI / BFC
                uint32_t lsb = (i >> 7) & 31, msb = (i >> 16) & 31;
                if (msb >= lsb) {
                    uint32_t w = msb - lsb + 1, mask = (w == 32 ? 0xffffffffu : (1u << w) - 1) << lsb;
                    uint32_t src = rm == 15 ? 0 : (r[rm] << lsb);
                    r[rd] = (r[rd] & ~mask) | (src & mask);
                }
                return 0;
            }
            undefined(s, pc, i);
            return 1;
        }
        uint32_t P = (i >> 24) & 1, U = (i >> 23) & 1, B = (i >> 22) & 1, W = (i >> 21) & 1, L = (i >> 20) & 1;
        uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15, off, co;
        if (op == 2) off = i & 0xfff;
        else off = shift_imm(s, r[i & 15], (i >> 5) & 3, (i >> 7) & 31, &co);
        uint32_t base = RV(rn);
        uint32_t addr = P ? (U ? base + off : base - off) : base;
        if (L) {
            uint32_t v = B ? rd8(addr) : rd32(addr);
            if (!P) r[rn] = U ? base + off : base - off;
            else if (W) r[rn] = addr;
            r[rd] = v;
            if (rd == 15) bx(s, v);
        } else {
            uint32_t v = rd == 15 ? pc + 8 : r[rd];
            if (B) wr8(addr, (uint8_t)v); else wr32(addr, v);
            if (!P) r[rn] = U ? base + off : base - off;
            else if (W) r[rn] = addr;
        }
        return 0;
    }
    case 4: {   // LDM/STM
        uint32_t P = (i >> 24) & 1, U = (i >> 23) & 1, W = (i >> 21) & 1, L = (i >> 20) & 1, rn = (i >> 16) & 15;
        uint32_t list = i & 0xffff, cnt = __builtin_popcount(list);
        uint32_t base = r[rn];
        uint32_t start = U ? base + (P ? 4 : 0) : base - cnt * 4 + (P ? 0 : 4);
        uint32_t nb = U ? base + cnt * 4 : base - cnt * 4;
        uint32_t a = start;
        if (L) {
            uint32_t newpc = 0;
            int haspc = 0;
            for (int k = 0; k < 16; k++) {
                if (!(list & (1 << k))) continue;
                uint32_t v = rd32(a); a += 4;
                if (k == 15) { newpc = v; haspc = 1; } else r[k] = v;
            }
            if (W && !(list & (1 << rn))) r[rn] = nb;
            if (haspc) bx(s, newpc);
        } else {
            for (int k = 0; k < 16; k++) {
                if (!(list & (1 << k))) continue;
                wr32(a, k == 15 ? pc + 8 : r[k]); a += 4;
            }
            if (W) r[rn] = nb;
        }
        return 0;
    }
    case 5: {   // B/BL
        int32_t off = (int32_t)(i << 8) >> 6;
        if (i & (1 << 24)) r[14] = pc + 4;
        r[15] = pc + 8 + off;
        return 0;
    }
    case 6:
        if (vfp(s, i, pc) == 0) return 0;
        undefined(s, pc, i);
        return 1;
    case 7:
        if (i & (1 << 24)) return svc(s, i & 0xffffff, pc + 4);
        if (vfp(s, i, pc) == 0) return 0;
        if ((i & 0x0f100f10) == 0x0e100f10 && ((i >> 16) & 15) == 13) {   // MRC p15, c13 (TLS register)
            LOG("arm32: MRC p15 c13 at %08x", pc);
            r[(i >> 12) & 15] = 0;
            return 0;
        }
        undefined(s, pc, i);
        return 1;
    default:
        undefined(s, pc, i);
        return 1;
    }
#undef RV
}

// ---------------------------------------------------------------- Thumb
static int thumb_step(S *s) {
    uint32_t *r = s->r;
    uint32_t pc = r[15];
    uint32_t i = rd16(pc);
    r[15] = pc + 2;
    uint32_t co;
    switch (i >> 11) {
    case 0: case 1: case 2: {   // LSL/LSR/ASR imm
        uint32_t rd = i & 7, rm = (i >> 3) & 7, amt = (i >> 6) & 31;
        uint32_t res = shift_imm(s, r[rm], i >> 11, amt, &co);
        r[rd] = res; nz(s, res); s->c = co;
        return 0;
    }
    case 3: {   // ADD/SUB reg/imm3
        uint32_t rd = i & 7, rn = (i >> 3) & 7, x = (i >> 6) & 7;
        uint32_t b = (i & (1 << 10)) ? x : r[x];
        r[rd] = (i & (1 << 9)) ? add_flags(s, r[rn], ~b, 1, 1) : add_flags(s, r[rn], b, 0, 1);
        return 0;
    }
    case 4: { uint32_t rd = (i >> 8) & 7; r[rd] = i & 0xff; nz(s, r[rd]); return 0; }        // MOV imm
    case 5: { add_flags(s, r[(i >> 8) & 7], ~(i & 0xff), 1, 1); return 0; }                  // CMP imm
    case 6: { uint32_t rd = (i >> 8) & 7; r[rd] = add_flags(s, r[rd], i & 0xff, 0, 1); return 0; }       // ADD imm
    case 7: { uint32_t rd = (i >> 8) & 7; r[rd] = add_flags(s, r[rd], ~(i & 0xff), 1, 1); return 0; }    // SUB imm
    case 8: {
        if (!(i & 0x400)) {   // ALU
            uint32_t rd = i & 7, rm = (i >> 3) & 7, a = r[rd], b = r[rm], res;
            switch ((i >> 6) & 15) {
            case 0: res = a & b; r[rd] = res; nz(s, res); break;
            case 1: res = a ^ b; r[rd] = res; nz(s, res); break;
            case 2: res = shift_reg(s, a, 0, b, &co); r[rd] = res; nz(s, res); s->c = co; break;
            case 3: res = shift_reg(s, a, 1, b, &co); r[rd] = res; nz(s, res); s->c = co; break;
            case 4: res = shift_reg(s, a, 2, b, &co); r[rd] = res; nz(s, res); s->c = co; break;
            case 5: r[rd] = add_flags(s, a, b, s->c, 1); break;
            case 6: r[rd] = add_flags(s, a, ~b, s->c, 1); break;
            case 7: res = shift_reg(s, a, 3, b, &co); r[rd] = res; nz(s, res); s->c = co; break;
            case 8: nz(s, a & b); break;
            case 9: r[rd] = add_flags(s, 0, ~b, 1, 1); break;
            case 10: add_flags(s, a, ~b, 1, 1); break;
            case 11: add_flags(s, a, b, 0, 1); break;
            case 12: res = a | b; r[rd] = res; nz(s, res); break;
            case 13: res = a * b; r[rd] = res; nz(s, res); break;
            case 14: res = a & ~b; r[rd] = res; nz(s, res); break;
            default: res = ~b; r[rd] = res; nz(s, res); break;
            }
            return 0;
        }
        // hi register ops / BX / BLX
        uint32_t rd = (i & 7) | ((i >> 4) & 8), rm = (i >> 3) & 15;
        uint32_t vm = rm == 15 ? pc + 4 : r[rm];
        switch ((i >> 8) & 3) {
        case 0: { uint32_t a = rd == 15 ? pc + 4 : r[rd]; r[rd] = a + vm; if (rd == 15) r[15] &= ~1u; break; }
        case 1: add_flags(s, rd == 15 ? pc + 4 : r[rd], ~vm, 1, 1); break;
        case 2: r[rd] = vm; if (rd == 15) r[15] &= ~1u; break;
        default:
            if (i & 0x80) { r[14] = (pc + 2) | 1; }   // BLX reg
            bx(s, vm);
            break;
        }
        return 0;
    }
    case 9: { uint32_t a = ((pc + 4) & ~3u) + (i & 0xff) * 4; r[(i >> 8) & 7] = rd32(a); return 0; }   // LDR literal
    case 10: case 11: {   // load/store register offset
        uint32_t rd = i & 7, a = r[(i >> 3) & 7] + r[(i >> 6) & 7];
        switch ((i >> 9) & 7) {
        case 0: wr32(a, r[rd]); break;
        case 1: wr16(a, (uint16_t)r[rd]); break;
        case 2: wr8(a, (uint8_t)r[rd]); break;
        case 3: r[rd] = (uint32_t)(int32_t)(int8_t)rd8(a); break;
        case 4: r[rd] = rd32(a); break;
        case 5: r[rd] = rd16(a); break;
        case 6: r[rd] = rd8(a); break;
        default: r[rd] = (uint32_t)(int32_t)(int16_t)rd16(a); break;
        }
        return 0;
    }
    case 12: { uint32_t a = r[(i >> 3) & 7] + ((i >> 6) & 31) * 4; wr32(a, r[i & 7]); return 0; }
    case 13: { uint32_t a = r[(i >> 3) & 7] + ((i >> 6) & 31) * 4; r[i & 7] = rd32(a); return 0; }
    case 14: { uint32_t a = r[(i >> 3) & 7] + ((i >> 6) & 31); wr8(a, (uint8_t)r[i & 7]); return 0; }
    case 15: { uint32_t a = r[(i >> 3) & 7] + ((i >> 6) & 31); r[i & 7] = rd8(a); return 0; }
    case 16: { uint32_t a = r[(i >> 3) & 7] + ((i >> 6) & 31) * 2; wr16(a, (uint16_t)r[i & 7]); return 0; }
    case 17: { uint32_t a = r[(i >> 3) & 7] + ((i >> 6) & 31) * 2; r[i & 7] = rd16(a); return 0; }
    case 18: { wr32(r[13] + (i & 0xff) * 4, r[(i >> 8) & 7]); return 0; }
    case 19: { r[(i >> 8) & 7] = rd32(r[13] + (i & 0xff) * 4); return 0; }
    case 20: { r[(i >> 8) & 7] = ((pc + 4) & ~3u) + (i & 0xff) * 4; return 0; }   // ADD rd, pc
    case 21: { r[(i >> 8) & 7] = r[13] + (i & 0xff) * 4; return 0; }              // ADD rd, sp
    case 22: case 23: {
        if ((i & 0xff00) == 0xb000) {   // ADD/SUB sp
            uint32_t imm = (i & 0x7f) * 4;
            r[13] = (i & 0x80) ? r[13] - imm : r[13] + imm;
            return 0;
        }
        if ((i & 0xf600) == 0xb400) {   // PUSH/POP
            uint32_t list = i & 0xff, R = (i >> 8) & 1;
            if (!(i & 0x800)) {
                uint32_t cnt = __builtin_popcount(list) + R;
                uint32_t a = r[13] - cnt * 4;
                r[13] = a;
                for (int k = 0; k < 8; k++) if (list & (1 << k)) { wr32(a, r[k]); a += 4; }
                if (R) wr32(a, r[14]);
            } else {
                uint32_t a = r[13];
                for (int k = 0; k < 8; k++) if (list & (1 << k)) { r[k] = rd32(a); a += 4; }
                if (R) { uint32_t v = rd32(a); a += 4; r[13] = a; bx(s, v); return 0; }
                r[13] = a;
            }
            return 0;
        }
        if ((i & 0xff00) == 0xbe00) { undefined(s, pc, i); return 1; }   // BKPT
        undefined(s, pc, i);
        return 1;
    }
    case 24: case 25: {   // STMIA/LDMIA
        uint32_t rn = (i >> 8) & 7, list = i & 0xff, a = r[rn];
        if (!(i & 0x800)) {
            for (int k = 0; k < 8; k++) if (list & (1 << k)) { wr32(a, r[k]); a += 4; }
            r[rn] = a;
        } else {
            for (int k = 0; k < 8; k++) if (list & (1 << k)) { r[k] = rd32(a); a += 4; }
            if (!(list & (1 << rn))) r[rn] = a;
        }
        return 0;
    }
    case 26: case 27: {   // B<cond> / SVC
        uint32_t c = (i >> 8) & 15;
        if (c == 15) return svc(s, i & 0xff, pc + 2);
        if (c == 14) { undefined(s, pc, i); return 1; }
        if (cond_ok(s, c)) r[15] = pc + 4 + ((int32_t)(int8_t)(i & 0xff) << 1);
        return 0;
    }
    case 28: { r[15] = pc + 4 + ((int32_t)(i << 21) >> 20); return 0; }   // B
    case 30: {   // BL/BLX prefix: both halves are read together
        uint32_t lo = rd16(pc + 2);
        int32_t off = ((int32_t)(i << 21) >> 9) + ((lo & 0x7ff) << 1);
        if ((lo >> 11) == 31) { r[14] = (pc + 4) | 1; r[15] = pc + 4 + off; return 0; }    // BL
        if ((lo >> 11) == 29) { r[14] = (pc + 4) | 1; s->t = 0; r[15] = (pc + 4 + off) & ~3u; return 0; }   // BLX
        // a lone prefix (rare): emulate the architectural LR update
        r[14] = pc + 4 + ((int32_t)(i << 21) >> 9);
        return 0;
    }
    case 31: { uint32_t t = r[14] + ((i & 0x7ff) << 1); r[14] = (pc + 2) | 1; r[15] = t & ~1u; return 0; }   // BL suffix alone
    case 29: { uint32_t t = r[14] + ((i & 0x7ff) << 1); r[14] = (pc + 2) | 1; s->t = 0; r[15] = t & ~3u; return 0; }  // BLX suffix alone
    default:
        undefined(s, pc, i);
        return 1;
    }
}

// ---------------------------------------------------------------- Jit::Run replacement
// Back to native code: the guest jumped to an address the recompiled engine has an entry for. Same steps
// as Cpu::run_function: flags go in through the context (Cpu+0x3c), the function runs on dynarmic's register
// array, and the context tells where guest execution continues (Cpu+0x38) with which flags.
static _Atomic uint64_t g_native;
static void enter_native(S *s, AotFn fn) {
    uint8_t *cpu = cpu_current();
    store_cpsr(s);
    *(uint32_t *)(cpu + 0x3c) = make_cpsr(s) & 0xf0000000u;
    fn(cpu + 0x20);
    uint32_t next = *(uint32_t *)(cpu + 0x38), flags = *(uint32_t *)(cpu + 0x3c);
    load_cpsr(s);
    s->n = flags >> 31; s->z = (flags >> 30) & 1; s->c = (flags >> 29) & 1; s->v = (flags >> 28) & 1;
    s->t = next & 1;
    s->r[15] = next & ~1u;
    atomic_fetch_add(&g_native, 1);
}

static _Atomic int g_runs;
static _Atomic uint64_t g_steps, g_lastReport;
#include <time.h>
static void account(S *st) {
    uint64_t total = atomic_fetch_add(&g_steps, st->steps) + st->steps;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    uint64_t last = atomic_load(&g_lastReport);
    if (now - last > 5000000000ull && atomic_compare_exchange_strong(&g_lastReport, &last, now)) {
        LOG("arm32: %llu instructions interpreted in the last 5 s (%d runs, %llu returns to native code so far)",
            (unsigned long long)total, atomic_load(&g_runs), (unsigned long long)atomic_load(&g_native));
        atomic_store(&g_steps, 0);
    }
}
uint32_t arm32_run(void *jit) {
    if (!g_mem) g_mem = *(uint8_t **)(g_host + ELF_host__ZN3mem6g_baseE);
    S st = {0};
    st.impl = IMPL(jit);
    st.r = REGS(st.impl);
    load_cpsr(&st);
    int n = atomic_fetch_add(&g_runs, 1);
    if (n < 20) LOG("arm32: run %d at %08x (%s)", n, st.r[15], st.t ? "Thumb" : "ARM");
    for (;;) {
        uint32_t h = atomic_load(HALT(st.impl));
        if (h) { store_cpsr(&st); account(&st); return h; }
        uint32_t pc = st.r[15], wasT = st.t;
        int stop = wasT ? thumb_step(&st) : arm_step(&st);
        st.steps++;
        if (!stop && st.r[15] != pc + (wasT ? 2 : 4) && !(wasT && st.r[15] == pc + 4)) {
            // a jump or return: continue natively if the recompiled engine can take it from here
            AotFn fn = direct_entry(st.r[15] | st.t);
            if (fn) {
                enter_native(&st, fn);
                if (atomic_load(HALT(st.impl))) { account(&st); return atomic_load(HALT(st.impl)); }
            }
        }
        if (stop) {
            store_cpsr(&st);
            uint32_t hh = atomic_load(HALT(st.impl));
            account(&st);
            if (hh) return hh;
            return 0x80000000u;   // stopped on an undefined instruction
        }
    }
}
