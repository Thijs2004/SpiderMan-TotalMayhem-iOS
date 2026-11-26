// Assembly glue between Android (AAPCS64) code and Darwin.
	.text

// ---- ELF TLS: static TLSDESC resolver. In: x0 = &GOT pair {resolver, offset}. Out: x0 = offset from the
// thread pointer (TPIDRRO_EL0 after the elf2macho patch); the block lives in consecutive TSD slots.
	.globl _elf_tlsdesc_static
	.p2align 2
_elf_tlsdesc_static:
	ldr	x0, [x0, #8]
	ret

// ---- Stubs for imports nobody provides: stub i calls elf_missing_called(i), which logs the name.
	.globl _elf_missing_stub_base
	.p2align 3
_elf_missing_stub_base:
	.set	stubidx, 0
	.rept	256
	mov	w0, #stubidx
	b	_elf_missing_called
	.set	stubidx, stubidx + 1
	.endr

// ---- Variadic calls from Android code. AAPCS64 passes the variadic arguments in registers (Darwin puts
// them on the stack), so these entry points spill x0-x7 / q0-q7, build an AAPCS va_list
// {__stack, __gr_top, __vr_top, __gr_offs, __vr_offs} and call impl(fixed args..., va_list *).
// ngp / nfp: number of named integer / floating arguments.
.macro VTRAMP name, impl, ngp, nfp
	.globl \name
	.p2align 2
\name:
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	sub	sp, sp, #224
	stp	q0, q1, [sp, #32]
	stp	q2, q3, [sp, #64]
	stp	q4, q5, [sp, #96]
	stp	q6, q7, [sp, #128]
	stp	x0, x1, [sp, #160]
	stp	x2, x3, [sp, #176]
	stp	x4, x5, [sp, #192]
	stp	x6, x7, [sp, #208]
	add	x9, x29, #16
	str	x9, [sp, #0]
	add	x9, sp, #224
	str	x9, [sp, #8]
	add	x9, sp, #160
	str	x9, [sp, #16]
	mov	w9, #((\ngp - 8) * 8)
	str	w9, [sp, #24]
	mov	w9, #((\nfp - 8) * 16)
	str	w9, [sp, #28]
	mov	x\ngp, sp
	bl	\impl
	mov	sp, x29
	ldp	x29, x30, [sp], #16
	ret
.endm

	VTRAMP _av_snprintf, _aimpl_snprintf, 3, 0
	VTRAMP _av_sprintf, _aimpl_sprintf, 2, 0
	VTRAMP _av_printf, _aimpl_printf, 1, 0
	VTRAMP _av_fprintf, _aimpl_fprintf, 2, 0
	VTRAMP _av_sscanf, _aimpl_sscanf, 2, 0
	VTRAMP _av_syslog, _aimpl_syslog, 2, 0
	VTRAMP _av_open, _aimpl_open, 2, 0
	VTRAMP _av_fcntl, _aimpl_fcntl, 2, 0
	VTRAMP _av_prctl, _aimpl_prctl, 1, 0
	VTRAMP _av_syscall, _aimpl_syscall, 1, 0
	VTRAMP _av_android_log_print, _aimpl_android_log_print, 3, 0

// JNI variadic entries: (env, obj|cls, methodID, ...) and CallNonvirtual*(env, obj, cls, methodID, ...)
.macro JNIV3 n
	VTRAMP _jniv_\n, _jnia_\n, 3, 0
.endm
.macro JNIV4 n
	VTRAMP _jniv_\n, _jnia_\n, 4, 0
.endm
	JNIV3 CallObjectMethod
	JNIV3 CallBooleanMethod
	JNIV3 CallByteMethod
	JNIV3 CallCharMethod
	JNIV3 CallShortMethod
	JNIV3 CallIntMethod
	JNIV3 CallLongMethod
	JNIV3 CallFloatMethod
	JNIV3 CallDoubleMethod
	JNIV3 CallVoidMethod
	JNIV3 CallStaticObjectMethod
	JNIV3 CallStaticBooleanMethod
	JNIV3 CallStaticByteMethod
	JNIV3 CallStaticCharMethod
	JNIV3 CallStaticShortMethod
	JNIV3 CallStaticIntMethod
	JNIV3 CallStaticLongMethod
	JNIV3 CallStaticFloatMethod
	JNIV3 CallStaticDoubleMethod
	JNIV3 CallStaticVoidMethod
	JNIV3 NewObject
	JNIV4 CallNonvirtualVoidMethod
	JNIV4 CallNonvirtualObjectMethod
	JNIV4 CallNonvirtualIntMethod
	JNIV4 CallNonvirtualBooleanMethod

// Calls an AAPCS function with up to 8 integer arguments: Darwin and AAPCS agree for non-variadic calls,
// so plain C function pointers work; nothing needed here.
