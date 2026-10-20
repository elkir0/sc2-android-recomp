/*
 * cpu.h - runtime for the CPU-struct x86-32 recompilation model
 *         (paired with tools/lift/lift32_cpu.py).
 *
 * Lifted x86 functions operate on an explicit CPU state struct, so several
 * machine states can be live at once - which is what a hybrid build needs when
 * real library code calls back into lifted code while an outer lifted call is
 * still on the stack. See runtime/hybrid/.
 *
 * The recomp runs as a 32-bit program so register values are real 32-bit host
 * pointers: the image's data sections live at their mapped VAs and heap comes
 * from malloc - both inside the 32-bit address space - so memory operands
 * dereference directly.
 *
 * Flags are computed eagerly by helpers after arithmetic/logic ops; Jcc reads
 * the stored bits. This is verbose but mechanical and easy to verify against
 * the hardware. (A lazy-flags optimization can come later.)
 */
#ifndef PCRECOMP_CPU_H
#define PCRECOMP_CPU_H

/* Passe 10r : aides intégrées de force dans les builds optimisés. En -O2, gcc comme clang (NDK) gardaient flags_sub,
 * flags_add, flags_logicz… comme fonctions hors ligne dans ~124 des 132 fichiers liftés (limites de croissance des
 * fonctions géantes) : un appel par instruction arithmétique x86 (12,6 % du binaire au profil sc2prof). -O0 inchangé. */
#if defined(__OPTIMIZE__) && defined(__GNUC__) && !defined(SC2_AIDES_HORS_LIGNE)
#define SC2_EN_LIGNE static inline __attribute__((always_inline))
#else
#define SC2_EN_LIGNE static inline
#endif

#ifndef SC2_GPTR_DEFINED
#define SC2_GPTR_DEFINED
#include <stdint.h>
extern uintptr_t g_hote_delta;   /* 0 sur Linux ; base hote - 0x00800000 sur macOS */
#define GPTR(va) ((void *)((uintptr_t)(uint32_t)(va) + g_hote_delta))
#endif
#include <stdint.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>   /* __readfsdword/__writefsdword: TIB-relative (fs:) access for SEH */
#if defined(__clang__) && defined(_M_IX86)
/* clang-cl has __readfsdword but not __writefsdword on x86: the call links as
 * an undefined ___writefsdword. MSVC has both. */
static __inline void pcrecomp_writefsdword(unsigned long off, unsigned long v)
{
    __asm__ volatile("movl %1, %%fs:(%0)" : : "r"(off), "r"(v) : "memory");
}
#define __writefsdword pcrecomp_writefsdword
#endif
#endif

/* ---- SSE register file ----
 *
 * One storage, several views - which is how the silicon works, and why this is
 * a union rather than eight float[4]. `movaps` moves a bit pattern, `pxor`
 * works on integers and `addss` works on floats, all through the same 128
 * bits; a float-only model corrupts the first two the moment a game stores a
 * mask in an XMM register, which every SSE-era game does.
 *
 * Scalar single precision is `float` here and NOT `double` like the x87 stack
 * below it. That difference is deliberate. An x87 register really is wider
 * than the values it holds, so widening it costs nothing - but SSE single
 * genuinely IS 32-bit, and computing `addss` in double rounds once where the
 * hardware rounds twice. The results differ in the low bits, and in a game
 * that feeds them to a coordinate transform the difference is visible. */
typedef union {
    float    f32[4];
    double   f64[2];
    uint32_t u32[4];
    uint64_t u64[2];
    int32_t  i32[4];
    /* The word lanes pextrw and pinsrw address. Expressing those two through
     * u32 and a shift works and reads like arithmetic; a lane array says what
     * the instruction says. */
    uint16_t u16[8];
    int16_t  i16[8];
    /* The byte lanes pslldq and psrldq shift through. Those two move the whole
     * register by a byte count, not each lane by a bit count, so they are the
     * one SSE shift that cannot be written in terms of any wider lane. */
    uint8_t  u8[16];
    int8_t   i8[16];
    int64_t  i64[2];
} XMM;

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eip;
    /* flags as discrete bits (0/1) */
    uint32_t cf, zf, sf, of, pf, af;
    /* x87: register-stack of doubles. st0 == st[fpu_top]; push pre-decrements. */
    double   st[8];
    int      fpu_top;
    /* SSE: xmm0..xmm7. A 32-bit target has no xmm8-15. */
    XMM      xmm[8];
    uint32_t fpu_sw;   /* status word: only C0/C1/C2/C3 condition bits modelled */
    /* Segment registers: stored, but NOT used in address computation. The
     * memory model below is flat - a register holds a real 32-bit address - so
     * `mov ds, ax` records the selector and does nothing else. That is right
     * for code that only saves and restores them, which is the common case even
     * in segmented 32-bit code, and wrong for code that switches DS to reach
     * another segment's data. The latter needs a selector -> base mapping the
     * runtime does not have; such code should be caught rather than lifted
     * silently. */
    uint16_t cs, ds, es, fs, gs, ss;
    /* Linear base of the TIB that `fs:` points at. Used only by the portable
     * fs: path (FS_* macros below): the host points this at a simulated TIB
     * per thread, and fs:[off] reads ordinary memory at fs_base+off. Left 0 and
     * unused on the MSVC default path, which reads the real segment. */
    uint32_t fs_base;
    /* Passe 7k : mot de contrôle x87, stocké XOR 0x027F (le mot de Windows au démarrage d'un
     * fil : précision 53 bits, arrondi au plus proche, exceptions masquées) pour qu'un CPU
     * remis à zéro par memset ait le mot de Windows. Lu par fist/fistp/frndint (champ RC). */
    uint16_t fpu_cwx;
    /* MMX mm0-mm7 (lift32_cpu, 64 bits each), appended last so no existing offset moves. */
    uint64_t mm[8];
    /* Passe 10r : état du runtime porté par le CPU (un par fil et par fibre invités) au lieu de variables __thread.
     * Dans une .so chargée par dlopen (Android), chaque accès __thread passe par le résolveur TLS dynamique
     * (7,4 % du fil du jeu au profil simpleperf du Fold). Ajoutés en fin : aucun décalage existant ne bouge. */
    uint32_t *saut;    /* saut SC2_SAUT en attente, confié au sc2_dispatch englobant (ex-t_sc2_saut) */
    void     *fantome; /* pile fantôme (sc2_shadow *) de ce fil ou de cette fibre : même objet que g_sc2_ss */
    uint32_t  n_disp;  /* appels de sc2_dispatch sur ce CPU (profondeur de pile échantillonnée 1/64) */
} CPU;

/* ---- partial register access (preserve unaffected bits, like x86) ---- */
#define R8L(r)        ((uint8_t)(r))
#define R8H(r)        ((uint8_t)((r) >> 8))
#define R16(r)        ((uint16_t)(r))
#define SET8L(r, v)   ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define SET8H(r, v)   ((r) = ((r) & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(v) << 8))
#define SET16(r, v)   ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))

/* ---- memory access: register holds a real 32-bit address ----
 *
 * Passe 10s : SC2_ACCES_VOLATILS (posé par cmake/passes_lift.cmake sur les seuls fichiers transformés par
 * scripts/registres-locaux.py) rend chaque accès invité volatile : émis tel quel, dans l'ordre du programme. Avec les
 * registres en variables locales, plus aucun store dans *c ne sépare deux accès invités, et le compilateur pourrait les
 * réordonner (lire un pointeur avant le drapeau « prêt » qui le garde) ou hisser une lecture hors d'une attente active,
 * ce que le x86 (TSO) interdit et dont dépendent les échanges entre fils du jeu. Sans le drapeau : code inchangé. */
#ifdef SC2_ACCES_VOLATILS
#define SC2_VOL volatile
#else
#define SC2_VOL
#endif
SC2_EN_LIGNE uint8_t  rd8 (uint32_t a) { return *(SC2_VOL uint8_t  *)GPTR(a); }
SC2_EN_LIGNE uint16_t rd16(uint32_t a) { return *(SC2_VOL uint16_t *)GPTR(a); }
SC2_EN_LIGNE uint32_t rd32(uint32_t a) { return *(SC2_VOL uint32_t *)GPTR(a); }
SC2_EN_LIGNE void wr8 (uint32_t a, uint8_t  v) { *(SC2_VOL uint8_t  *)GPTR(a) = v; }
SC2_EN_LIGNE void wr16(uint32_t a, uint16_t v) { *(SC2_VOL uint16_t *)GPTR(a) = v; }
SC2_EN_LIGNE void wr32(uint32_t a, uint32_t v) { *(SC2_VOL uint32_t *)GPTR(a) = v; }
/* Copies de taille constante (x87, SSE, MMX) : memcpy d'ordinaire ; accès volatils de 8, 4, 2 puis 1 octet(s) sous
 * SC2_ACCES_VOLATILS (n constant : la boucle se déroule à l'intégration). */
#ifdef SC2_ACCES_VOLATILS
SC2_EN_LIGNE void sc2_lire_vol(void *d, uint32_t a, unsigned n) {
    unsigned char *o = (unsigned char *)d; unsigned k = 0;
    for (; k + 8 <= n; k += 8) { uint64_t v = *(volatile uint64_t *)GPTR(a + k); memcpy(o + k, &v, 8); }
    if (k + 4 <= n) { uint32_t v = *(volatile uint32_t *)GPTR(a + k); memcpy(o + k, &v, 4); k += 4; }
    if (k + 2 <= n) { uint16_t v = *(volatile uint16_t *)GPTR(a + k); memcpy(o + k, &v, 2); k += 2; }
    if (k < n) o[k] = *(volatile uint8_t *)GPTR(a + k);
}
SC2_EN_LIGNE void sc2_ecrire_vol(uint32_t a, const void *s, unsigned n) {
    const unsigned char *i = (const unsigned char *)s; unsigned k = 0;
    for (; k + 8 <= n; k += 8) { uint64_t v; memcpy(&v, i + k, 8); *(volatile uint64_t *)GPTR(a + k) = v; }
    if (k + 4 <= n) { uint32_t v; memcpy(&v, i + k, 4); *(volatile uint32_t *)GPTR(a + k) = v; k += 4; }
    if (k + 2 <= n) { uint16_t v; memcpy(&v, i + k, 2); *(volatile uint16_t *)GPTR(a + k) = v; k += 2; }
    if (k < n) *(volatile uint8_t *)GPTR(a + k) = i[k];
}
#define SC2_LIRE(d, a, n)   sc2_lire_vol((d), (a), (n))
#define SC2_ECRIRE(a, s, n) sc2_ecrire_vol((a), (s), (n))
#else
#define SC2_LIRE(d, a, n)   memcpy((d), GPTR(a), (n))
#define SC2_ECRIRE(a, s, n) memcpy(GPTR(a), (s), (n))
#endif

/* ---- fs:-relative access (TIB / SEH chain) ----
 *
 * The lifter emits these for every `fs:` memory operand (SEH prologues store
 * the exception-registration record through fs:[0]). Two implementations, one
 * chosen at compile time so a single lift builds for either host:
 *
 *   MSVC default        the real segment, via the __readfsN/__writefsN
 *                       intrinsics. Correct when the host is a 1:1 Win32
 *                       process whose own TIB the guest shares.
 *   portable / non-MSVC read ordinary memory at c->fs_base + off. The host
 *                       gives each guest thread a simulated TIB and points
 *                       c->fs_base at it. This is what a non-Windows (Linux)
 *                       host needs; it also works on Windows if the host sets
 *                       c->fs_base to NtCurrentTeb() and defines
 *                       RECOMP_FS_SIMULATED.
 */
#if defined(_MSC_VER) && !defined(RECOMP_FS_SIMULATED)
#define FS_RD8(c, off)      __readfsbyte(off)
#define FS_RD16(c, off)     __readfsword(off)
#define FS_RD32(c, off)     __readfsdword(off)
#define FS_WR8(c, off, v)   __writefsbyte((off), (unsigned char)(v))
#define FS_WR16(c, off, v)  __writefsword((off), (unsigned short)(v))
#define FS_WR32(c, off, v)  __writefsdword((off), (unsigned long)(v))
#else
#define FS_RD8(c, off)      rd8 ((c)->fs_base + (off))
#define FS_RD16(c, off)     rd16((c)->fs_base + (off))
#define FS_RD32(c, off)     rd32((c)->fs_base + (off))
#define FS_WR8(c, off, v)   wr8 ((c)->fs_base + (off), (uint8_t )(v))
#define FS_WR16(c, off, v)  wr16((c)->fs_base + (off), (uint16_t)(v))
#define FS_WR32(c, off, v)  wr32((c)->fs_base + (off), (uint32_t)(v))
#endif
/* movlps/movhps and friends move a lane as bits, not as a number: a double
 * round-trip would be exact for every value except the signalling NaNs a
 * mask-producing compare leaves behind. memcpy, because the address need
 * not be aligned. */
SC2_EN_LIGNE uint64_t rd64(uint32_t a) { uint64_t v; SC2_LIRE(&v, a, 8); return v; }
SC2_EN_LIGNE void wr64(uint32_t a, uint64_t v) { SC2_ECRIRE(a, &v, 8); }

/* ---- EFLAGS pack/unpack (modelled bits only) ---- */
/* `lock`-prefixed read-modify-write.
 *
 * A recompiled game with a worker pool needs these to be real: the guest uses
 * `lock xadd` for every reference count and every queue index, and a
 * non-atomic one is a use-after-free an hour into a session rather than a
 * crash you can find. Nothing else in this header cares about other threads;
 * these do.
 *
 * The x86 semantics are "return the old value" for xadd, and for cmpxchg
 * "return what was there, and the caller compares" - the ZF the instruction
 * sets follows from that. */
#if defined(_MSC_VER)
SC2_EN_LIGNE uint32_t atomic_xadd32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)GPTR(a), (long)v);
}
SC2_EN_LIGNE uint32_t atomic_cmpxchg32(uint32_t a, uint32_t cmp, uint32_t nv) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)GPTR(a),
                                                 (long)nv, (long)cmp);
}
SC2_EN_LIGNE uint32_t atomic_xchg32(uint32_t a, uint32_t v) {
    return (uint32_t)_InterlockedExchange((volatile long *)GPTR(a), (long)v);
}
#else
SC2_EN_LIGNE uint32_t atomic_xadd32(uint32_t a, uint32_t v) {
    return __sync_fetch_and_add((volatile uint32_t *)GPTR(a), v);
}
SC2_EN_LIGNE uint32_t atomic_cmpxchg32(uint32_t a, uint32_t cmp, uint32_t nv) {
    return __sync_val_compare_and_swap((volatile uint32_t *)GPTR(a), cmp, nv);
}
SC2_EN_LIGNE uint32_t atomic_xchg32(uint32_t a, uint32_t v) {
    return __sync_lock_test_and_set((volatile uint32_t *)GPTR(a), v);
}
#endif

SC2_EN_LIGNE uint32_t eflags_pack(CPU *c) {
    return 0x202u | (c->cf) | (c->pf << 2) | (c->af << 4) |
           (c->zf << 6) | (c->sf << 7) | (c->of << 11);
}
SC2_EN_LIGNE void eflags_unpack(CPU *c, uint32_t v) {
    c->cf = v & 1; c->pf = (v >> 2) & 1; c->af = (v >> 4) & 1;
    c->zf = (v >> 6) & 1; c->sf = (v >> 7) & 1; c->of = (v >> 11) & 1;
}

/* ---- stack ----
 *
 * ESP is an address in a flat build, so the base is zero and these compile to
 * exactly what they did before. A segmented build defines STACK_BASE to its
 * SS base instead, because there ESP is an OFFSET: 16-bit code reaching its
 * arguments does `mov bp, sp` and then `[bp+6]`, and truncating a host address
 * to 16 bits produces a pointer to nothing. */
#ifndef STACK_BASE
#define STACK_BASE(c) 0u
#endif
SC2_EN_LIGNE void push32(CPU *c, uint32_t v) { c->esp -= 4; wr32(STACK_BASE(c) + c->esp, v); }
SC2_EN_LIGNE uint32_t pop32(CPU *c) { uint32_t v = rd32(STACK_BASE(c) + c->esp); c->esp += 4; return v; }

/* An instruction the lifter could not express. Plain abort() by default, so a
 * project that includes only cpu.h is unchanged; a runtime that can say
 * something useful - which guest address, which mnemonic, how it got there -
 * defines RECOMP_TODO before including this. A bare abort() in two million
 * lines becomes __fastfail in a release build, which no handler sees. */
#ifndef RECOMP_TODO
#define RECOMP_TODO(va, text) abort()
#endif

/* ---- absolute image references: abs VA at preferred base -> live address ---- */
extern uint32_t g_image_delta;   /* live_base - PE ImageBase (0 if loaded where it wanted) */
#define GVA(abs) ((uint32_t)((abs) + g_image_delta))

/* ---- flag helpers ---- */
SC2_EN_LIGNE uint32_t parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (~v) & 1;
}

/* logical ops (AND/OR/XOR/TEST): CF=OF=0 */
SC2_EN_LIGNE void flags_logic32(CPU *c, uint32_t r) {
    c->cf = 0; c->of = 0; c->sf = r >> 31; c->zf = (r == 0); c->pf = parity8((uint8_t)r);
}
SC2_EN_LIGNE void flags_logic8(CPU *c, uint8_t r) {
    c->cf = 0; c->of = 0; c->sf = (r >> 7) & 1; c->zf = (r == 0); c->pf = parity8(r);
}
/* cmp/sub (a - b) */
SC2_EN_LIGNE uint32_t flags_sub32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->cf = (a < b);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = (((a ^ b) & (a ^ r)) >> 31) & 1;
    c->af = ((a ^ b ^ r) >> 4) & 1;
    c->pf = parity8((uint8_t)r);
    return r;
}
/* add (a + b) */
SC2_EN_LIGNE uint32_t flags_add32(CPU *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->cf = (r < a);
    c->zf = (r == 0);
    c->sf = r >> 31;
    c->of = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    c->af = ((a ^ b ^ r) >> 4) & 1;
    c->pf = parity8((uint8_t)r);
    return r;
}
/* inc/dec: like add/sub by 1 but CF preserved */
SC2_EN_LIGNE uint32_t flags_inc32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_add32(c, a, 1); c->cf = keepcf; return r;
}
SC2_EN_LIGNE uint32_t flags_dec32(CPU *c, uint32_t a) {
    uint32_t keepcf = c->cf, r = flags_sub32(c, a, 1); c->cf = keepcf; return r;
}

/* ---- width-generic flag helpers (sz in {1,2,4}); used by generated code ---- */
SC2_EN_LIGNE uint32_t mask_sz(int sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
SC2_EN_LIGNE uint32_t sign_sz(int sz) { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }

SC2_EN_LIGNE uint32_t flags_sub(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a - b) & m;
    c->cf = (a < b); c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = (((a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}
SC2_EN_LIGNE uint32_t flags_add(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz);
    a &= m; b &= m; uint32_t r = (a + b) & m;
    c->cf = (r < a); c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}
SC2_EN_LIGNE uint32_t flags_logicz(CPU *c, uint32_t r, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); r &= m;
    c->cf = 0; c->of = 0; c->sf = (r & s) != 0; c->zf = (r == 0);
    c->pf = parity8((uint8_t)r); return r;
}
SC2_EN_LIGNE uint32_t flags_incs(CPU *c, uint32_t a, int sz) {
    uint32_t keep = c->cf, r = flags_add(c, a, 1, sz); c->cf = keep; return r;
}
SC2_EN_LIGNE uint32_t flags_decs(CPU *c, uint32_t a, int sz) {
    uint32_t keep = c->cf, r = flags_sub(c, a, 1, sz); c->cf = keep; return r;
}
SC2_EN_LIGNE uint32_t flags_adc(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), cin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a + b + cin; uint32_t r = (uint32_t)full & m;
    c->cf = (full >> (sz * 8)) & 1; c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = ((~(a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}
SC2_EN_LIGNE uint32_t flags_sbb(CPU *c, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz), bin = c->cf & 1;
    a &= m; b &= m; uint64_t full = (uint64_t)a - b - bin; uint32_t r = (uint32_t)full & m;
    c->cf = (full >> (sz * 8)) & 1; c->zf = (r == 0); c->sf = (r & s) != 0;
    c->of = (((a ^ b) & (a ^ r)) & s) != 0; c->af = ((a ^ b ^ r) & 0x10) != 0;
    c->pf = parity8((uint8_t)r); return r;
}

SC2_EN_LIGNE uint32_t op_shl(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); cnt &= 31; if (!cnt) return v & m;
    c->cf = (v >> (sz * 8 - cnt)) & 1; uint32_t r = (v << cnt) & m;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r);
    c->of = c->sf ^ c->cf;   /* 7k : défini pour un compte de 1 (MSB(r) xor CF), indéfini au-delà */
    return r;
}
SC2_EN_LIGNE uint32_t op_shr(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (cnt - 1)) & 1; uint32_t r = v >> cnt;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r);
    c->of = (v & sign_sz(sz)) != 0;   /* 7k : compte 1 → MSB de l'opérande d'origine */
    return r;
}
SC2_EN_LIGNE uint32_t op_sar(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz), s = sign_sz(sz); v &= m; cnt &= 31; if (!cnt) return v;
    uint32_t ext = (v & s) ? (m << (sz * 8 - cnt)) & m : 0;
    c->cf = (v >> (cnt - 1)) & 1; uint32_t r = ((v >> cnt) | ext) & m;
    c->zf = (r == 0); c->sf = (r & s) != 0; c->pf = parity8((uint8_t)r);
    c->of = 0;   /* 7k : compte 1 → 0 */
    return r;
}

/* ---- rotates. Unlike the shifts these leave ZF/SF/PF alone, which is not an
   omission: x86 does not touch them here, and code that rotates then branches
   on the zero flag is reading the flag the PREVIOUS instruction set. ---- */
SC2_EN_LIGNE uint32_t op_rol(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    cnt &= 31; cnt %= (uint32_t)w; if (!cnt) return v;
    uint32_t r = ((v << cnt) | (v >> (w - cnt))) & m;
    c->cf = r & 1;
    c->of = (((r >> (w - 1)) & 1) ^ (c->cf & 1));
    return r;
}
SC2_EN_LIGNE uint32_t op_ror(CPU *c, uint32_t v, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8; v &= m;
    cnt &= 31; cnt %= (uint32_t)w; if (!cnt) return v;
    uint32_t r = ((v >> cnt) | (v << (w - cnt))) & m;
    c->cf = (r >> (w - 1)) & 1;
    c->of = (((r >> (w - 1)) & 1) ^ ((r >> (w - 2)) & 1));
    return r;
}

/* ---- byte order ----
 *
 * bswap reverses the four bytes of a 32-bit register and touches no flags.
 * It is how a compiler writes htonl/ntohl inline, so it turns up in any code
 * that opens a socket, next to a `ror bx, 8` doing the 16-bit half.
 *
 * The 16-bit form is architecturally undefined - Intel documents it as such
 * and real parts leave the register zeroed - so a compiler never emits it and
 * this deliberately does not pretend to implement it. */
SC2_EN_LIGNE uint32_t op_bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u)
         | ((v << 8) & 0x00FF0000u) | (v << 24);
}

/* ---- double-precision shifts: shift `d`, feeding in bits from `s` ---- */
SC2_EN_LIGNE uint32_t op_shld(CPU *c, uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;      /* count > width is undefined */
    uint32_t r = ((d << cnt) | (s >> (w - cnt))) & m;
    c->cf = (d >> (w - cnt)) & 1;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r);
    c->of = ((r ^ d) & sign_sz(sz)) != 0;   /* 7k : compte 1 → changement de signe */
    return r;
}
/* BSF/BSR: the index of the lowest (or highest) set bit. The flag is the
 * whole interface - ZF says whether there WAS one - and when the source is
 * zero the destination is architecturally undefined, so it is left alone
 * rather than given a value that code might come to depend on. */
SC2_EN_LIGNE uint32_t op_bsf(CPU *c, uint32_t d, uint32_t s, int sz) {
    uint32_t i, n = (uint32_t)sz * 8;
    c->zf = (s == 0);
    if (s == 0) return d;
    for (i = 0; i < n; i++) if (s & (1u << i)) return i;
    return d;
}

SC2_EN_LIGNE uint32_t op_bsr(CPU *c, uint32_t d, uint32_t s, int sz) {
    uint32_t i, n = (uint32_t)sz * 8;
    c->zf = (s == 0);
    if (s == 0) return d;
    for (i = n; i-- > 0; ) if (s & (1u << i)) return i;
    return d;
}

/* BT/BTS/BTR/BTC: the selected bit into CF, then the register form's own
 * change to it. The count is taken modulo the operand width - which is what
 * makes `bt eax, 33` test bit 1 and not read past the register. Only the
 * register form is expressed here; the memory form of these addresses a bit
 * string that can run past the operand, and no lifted site uses it. */
SC2_EN_LIGNE uint32_t op_bittest(CPU *c, uint32_t d, uint32_t bit, int sz, int op) {
    uint32_t mask = 1u << (bit & ((uint32_t)sz * 8 - 1));
    c->cf = (d & mask) ? 1 : 0;
    switch (op) {
        case 1: return d | mask;      /* bts */
        case 2: return d & ~mask;     /* btr */
        case 3: return d ^ mask;      /* btc */
        default: return d;            /* bt  */
    }
}

/* RCL/RCR rotate through CF, so the rotated quantity is one bit wider than
 * the operand and the count is taken modulo that wider width. Writing this as
 * a bit at a time is slower than the closed form and is the version that is
 * obviously right, which for an instruction this rare is the better trade. */
SC2_EN_LIGNE uint32_t op_rcl(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8, i;
    uint32_t mask = (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    cnt %= (n + 1);
    if (cnt == 0) return d;        /* a zero count changes no flag at all */
    for (i = 0; i < cnt; i++) {
        uint32_t top = (d >> (n - 1)) & 1u;
        d = ((d << 1) | c->cf) & mask;
        c->cf = top;
    }
    /* OF is defined only for a count of one; hardware leaves something there
     * for the rest and nothing may read it. */
    if (cnt == 1) c->of = (uint32_t)(((d >> (n - 1)) & 1u) ^ c->cf);
    return d;
}

SC2_EN_LIGNE uint32_t op_rcr(CPU *c, uint32_t d, uint32_t cnt, int sz) {
    uint32_t n = (uint32_t)sz * 8, i;
    uint32_t mask = (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    cnt %= (n + 1);
    if (cnt == 0) return d;
    /* RCR sets OF from the two top bits of the RESULT, and before the rotate
     * rather than after - but only the count-of-one case is defined, and for
     * that one the two are the same thing. */
    if (cnt == 1) c->of = (uint32_t)((((d >> 1) | (c->cf << (n - 1))) >> (n - 1)) & 1u)
                        ^ (uint32_t)((((d >> 1) | (c->cf << (n - 1))) >> (n - 2)) & 1u);
    for (i = 0; i < cnt; i++) {
        uint32_t bot = d & 1u;
        d = ((d >> 1) | (c->cf << (n - 1))) & mask;
        c->cf = bot;
    }
    return d;
}

SC2_EN_LIGNE uint32_t op_shrd(CPU *c, uint32_t d, uint32_t s, uint32_t cnt, int sz) {
    uint32_t m = mask_sz(sz); int w = sz * 8;
    d &= m; s &= m; cnt &= 31;
    if (!cnt || cnt > (uint32_t)w) return d;
    uint32_t r = ((d >> cnt) | (s << (w - cnt))) & m;
    c->cf = (d >> (cnt - 1)) & 1;
    c->zf = (r == 0); c->sf = (r & sign_sz(sz)) != 0; c->pf = parity8((uint8_t)r);
    c->of = ((r ^ d) & sign_sz(sz)) != 0;   /* 7k : compte 1 → changement de signe */
    return r;
}

/* ---- shifts (set flags like x86; count masked to 5 bits) ---- */
SC2_EN_LIGNE uint32_t shr32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (cnt - 1)) & 1;
    uint32_t r = v >> cnt;
    c->zf = (r == 0); c->sf = r >> 31; c->pf = parity8((uint8_t)r);
    return r;
}
SC2_EN_LIGNE uint32_t shl32(CPU *c, uint32_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    c->cf = (v >> (32 - cnt)) & 1;
    uint32_t r = v << cnt;
    c->zf = (r == 0); c->sf = r >> 31; c->pf = parity8((uint8_t)r);
    return r;
}
SC2_EN_LIGNE uint8_t shr8(CPU *c, uint8_t v, uint32_t cnt) {
    cnt &= 31; if (!cnt) return v;
    if (cnt <= 8) c->cf = (v >> (cnt - 1)) & 1;
    uint8_t r = (cnt < 8) ? (uint8_t)(v >> cnt) : 0;
    c->zf = (r == 0); c->sf = (r >> 7) & 1; c->pf = parity8(r);
    return r;
}

/* ---- x87 FPU ---- */
#include <math.h>
SC2_EN_LIGNE void   fpush(CPU *c, double v) { c->fpu_top = (c->fpu_top - 1) & 7; c->st[c->fpu_top] = v; }
SC2_EN_LIGNE double fpop(CPU *c) { double v = c->st[c->fpu_top]; c->fpu_top = (c->fpu_top + 1) & 7; return v; }
SC2_EN_LIGNE double *fst(CPU *c, int i) { return &c->st[(c->fpu_top + i) & 7]; }

/* float/double/int memory operands (memcpy avoids alignment/aliasing issues) */
SC2_EN_LIGNE double rdf32(uint32_t a) { float  f; SC2_LIRE(&f, a, 4); return (double)f; }
SC2_EN_LIGNE double rdf64(uint32_t a) { double d; SC2_LIRE(&d, a, 8); return d; }
SC2_EN_LIGNE double rdi16(uint32_t a) { int16_t i; SC2_LIRE(&i, a, 2); return (double)i; }
SC2_EN_LIGNE double rdi32(uint32_t a) { int32_t i; SC2_LIRE(&i, a, 4); return (double)i; }
SC2_EN_LIGNE double rdi64(uint32_t a) { int64_t i; SC2_LIRE(&i, a, 8); return (double)i; }
SC2_EN_LIGNE void   wrf32(uint32_t a, double v) { float  f = (float)v; SC2_ECRIRE(a, &f, 4); }
SC2_EN_LIGNE void   wrf64(uint32_t a, double v) { SC2_ECRIRE(a, &v, 8); }
/* fist/fistp. Hors plage (et NaN) le x87 écrit l'« integer indefinite » (0x8000, 0x80000000,
 * 0x8000000000000000). Le cast C est indéfini là : x86-64 rend la même valeur par hasard
 * (cvttsd2si), AArch64 SATURE (fcvtzs) — la passe 7k borne explicitement. */
SC2_EN_LIGNE int64_t fpu_i64(double r, double lim) {
    return (r >= -lim && r < lim) ? (int64_t)r : INT64_MIN;
}
SC2_EN_LIGNE void   wri32(uint32_t a, double v) { double r = nearbyint(v); int32_t i = (r >= -2147483648.0 && r < 2147483648.0) ? (int32_t)r : INT32_MIN; SC2_ECRIRE(a, &i, 4); }
SC2_EN_LIGNE void   wri16(uint32_t a, double v) { double r = nearbyint(v); int16_t i = (r >= -32768.0 && r < 32768.0) ? (int16_t)r : INT16_MIN; SC2_ECRIRE(a, &i, 2); }
SC2_EN_LIGNE void   wri64(uint32_t a, double v) { int64_t i = fpu_i64(nearbyint(v), 9223372036854775808.0); SC2_ECRIRE(a, &i, 8); }

/* ---- mot de contrôle x87 (passe 7k ; lifter avec PCRECOMP_FPU_CW=1) ----
 * fldcw/fnstcw/fninit étaient ignorés (fnstcw rendait la constante 0x027F) : la conversion
 * flottant → entier de MSVC (`fnstcw ; or ah, 0x0C ; fldcw ; fistp ; fldcw`, 1 414 fldcw dans
 * SC2.exe) arrondissait au plus proche au lieu de TRONQUER : 2,7 → 3 au lieu de 2, −0,5 → −0
 * au lieu de 0… Seul le champ RC (bits 10-11) agit sur le calcul ; la précision (PC) reste
 * celle du modèle (double = PC 53 bits, celle de Windows) ; les masques d'exception sont
 * mémorisés et rendus tels quels. */
#define FPU_CW_WINDOWS 0x027Fu
SC2_EN_LIGNE uint32_t fpu_fnstcw(CPU *c) { return (uint16_t)(c->fpu_cwx ^ FPU_CW_WINDOWS); }
SC2_EN_LIGNE void fpu_fldcw(CPU *c, uint32_t cw) { c->fpu_cwx = (uint16_t)(cw ^ FPU_CW_WINDOWS); }
/* fnstsw : le mot d'état porte TOP (bits 11-13) ; seuls C0-C3 sont tenus dans fpu_sw. */
SC2_EN_LIGNE uint32_t fpu_fnstsw(CPU *c) {
    return (c->fpu_sw & ~0x3800u & 0xFFFFu) | (((uint32_t)c->fpu_top & 7u) << 11);
}
SC2_EN_LIGNE void fpu_fninit(CPU *c) {
    c->fpu_cwx = (uint16_t)(0x037Fu ^ FPU_CW_WINDOWS); c->fpu_sw = 0; c->fpu_top = 0;
}
SC2_EN_LIGNE double fpu_rndint(CPU *c, double v) {
    switch ((fpu_fnstcw(c) >> 10) & 3) {
    case 1:  return floor(v);       /* vers −∞ */
    case 2:  return ceil(v);        /* vers +∞ */
    case 3:  return trunc(v);       /* vers 0 : le cast C de MSVC */
    default: return nearbyint(v);   /* au plus proche pair (mode hôte par défaut) */
    }
}
SC2_EN_LIGNE void wri32r(CPU *c, uint32_t a, double v) {
    double r = fpu_rndint(c, v); int32_t i = (r >= -2147483648.0 && r < 2147483648.0) ? (int32_t)r : INT32_MIN;
    SC2_ECRIRE(a, &i, 4);
}
SC2_EN_LIGNE void wri16r(CPU *c, uint32_t a, double v) {
    double r = fpu_rndint(c, v); int16_t i = (r >= -32768.0 && r < 32768.0) ? (int16_t)r : INT16_MIN;
    SC2_ECRIRE(a, &i, 2);
}
SC2_EN_LIGNE void wri64r(CPU *c, uint32_t a, double v) {
    int64_t i = fpu_i64(fpu_rndint(c, v), 9223372036854775808.0); SC2_ECRIRE(a, &i, 8);
}


/* fcom/fcomp: set C3/C2/C0 (st0 vs v). Cleared C1. Passe 10t : le calcul vit dans
 * fcompare_sw sur un mot d'état pointé — SC2_X87_FCOM (sc2_lifted.h) l'appelle sur le
 * sw de la pile x87 locale ; fcompare(c, …) = fcompare_sw(&c->fpu_sw, …), à l'identique. */
SC2_EN_LIGNE void fcompare_sw(uint32_t *sw, double a, double b) {
    uint32_t s = *sw & ~0x4700u;
    if      (a > b)  { /* 000 */ }
    else if (a < b)  s |= 0x0100u;          /* C0 */
    else if (a == b) s |= 0x4000u;          /* C3 */
    else             s |= 0x4700u;          /* unordered: C3|C2|C0 */
    *sw = s;
}
SC2_EN_LIGNE void fcompare(CPU *c, double a, double b) { fcompare_sw(&c->fpu_sw, a, b); }
/* fcomi/fucomi: the same comparison, into EFLAGS instead of the status word.
 * The P6 forms exist so a compiler can branch on a float compare without
 * going through `fnstsw ax; test ah`. Note the mapping is not the integer
 * one: "unordered" is ZF=PF=CF=1, which is why `jp` after an fcomi is the
 * NaN test, and OF/SF/AF are cleared. */
SC2_EN_LIGNE void fcompare_eflags(CPU *c, double a, double b) {
    c->of = c->sf = c->af = 0;
    if      (a > b)  { c->zf = 0; c->pf = 0; c->cf = 0; }
    else if (a < b)  { c->zf = 0; c->pf = 0; c->cf = 1; }
    else if (a == b) { c->zf = 1; c->pf = 0; c->cf = 0; }
    else             { c->zf = 1; c->pf = 1; c->cf = 1; }
}
/* sahf: load AH into CF,PF,AF,ZF,SF */
SC2_EN_LIGNE void do_sahf(CPU *c, uint8_t ah) {
    c->cf = ah & 1; c->pf = (ah >> 2) & 1; c->af = (ah >> 4) & 1;
    c->zf = (ah >> 6) & 1; c->sf = (ah >> 7) & 1;
}

/* ---- SSE ----
 *
 * Enough of SSE/SSE2 for a Pentium 4-era game: scalar float and double, the
 * packed moves and bitwise ops, and the compares. Packed arithmetic beyond
 * that is not here yet - it lifts to a TODO rather than silently wrong code.
 */

/* Memory operands. Separate from the x87 rdf32/rdf64 above because those
 * widen to double for the 80-bit stack, and these must not. memcpy because an
 * SSE memory operand is frequently unaligned (movups exists for a reason). */
SC2_EN_LIGNE float  rdss(uint32_t a) { float  f; SC2_LIRE(&f, a, 4); return f; }
SC2_EN_LIGNE double rdsd(uint32_t a) { double d; SC2_LIRE(&d, a, 8); return d; }
SC2_EN_LIGNE XMM    rdxm(uint32_t a) { XMM x;    SC2_LIRE(&x, a, 16); return x; }
SC2_EN_LIGNE void   wrss(uint32_t a, float  v) { SC2_ECRIRE(a, &v, 4); }
SC2_EN_LIGNE void   wrsd(uint32_t a, double v) { SC2_ECRIRE(a, &v, 8); }
SC2_EN_LIGNE void   wrxm(uint32_t a, XMM    v) { SC2_ECRIRE(a, &v, 16); }

/* MOVSS/MOVSD loading from memory zero the rest of the register; the
 * register-to-register forms leave the high lanes alone. Getting that
 * backwards is invisible until something reads the high lanes, which is why
 * it is a named helper and not an open-coded assignment. */
SC2_EN_LIGNE void sse_load_ss(XMM *d, float  v) { d->u64[0] = 0; d->u64[1] = 0; d->f32[0] = v; }
SC2_EN_LIGNE void sse_load_sd(XMM *d, double v) { d->u64[0] = 0; d->u64[1] = 0; d->f64[0] = v; }

/* UCOMISS/COMISS/UCOMISD/COMISD: the comparison lands in ZF/PF/CF, and clears
 * OF/SF/AF. An unordered compare - either operand NaN - sets all three, which
 * is exactly what makes `ucomiss; jp` a NaN test and `ucomiss; jbe` mean
 * "below or equal, or unordered". Note the flags are the UNSIGNED set: the
 * compiler emits `ja`/`jbe` after a float compare, never `jg`/`jle`. */
SC2_EN_LIGNE void sse_compare(CPU *c, double a, double b) {
    c->of = 0; c->sf = 0; c->af = 0;
    if (a != a || b != b) { c->zf = 1; c->pf = 1; c->cf = 1; }   /* unordered */
    else                  { c->zf = (a == b); c->pf = 0; c->cf = (a < b); }
}

/* MINSS/MAXSS are not fmin/fmax. The manual defines them as
 *     if (dst OP src) then dst else src
 * and that "else src" is load-bearing in two cases fmin gets the other way
 * round: when the operands are equal - which includes +0.0 against -0.0, so
 * min(+0,-0) is -0 but min(-0,+0) is +0 - and when either is NaN, where the
 * comparison is false and the SECOND operand wins. Written in that exact
 * shape, C's own "any compare with NaN is false" rule delivers both for free.
 * Writing it the other way round, `(src < dst) ? src : dst`, looks identical
 * and is wrong on both. */
SC2_EN_LIGNE float  sse_minf(float  d, float  s) { return (d < s) ? d : s; }
SC2_EN_LIGNE float  sse_maxf(float  d, float  s) { return (d > s) ? d : s; }
SC2_EN_LIGNE double sse_mind(double d, double s) { return (d < s) ? d : s; }
SC2_EN_LIGNE double sse_maxd(double d, double s) { return (d > s) ? d : s; }

/* CVTTSS2SI and friends yield the "integer indefinite" value when the source
 * does not fit in 32 bits, where the equivalent C cast is undefined behaviour
 * and may trap. One comparison buys a defined answer that matches hardware. */
SC2_EN_LIGNE int32_t sse_cvtt_i32(double v) {
    return (v >= -2147483648.0 && v < 2147483648.0) ? (int32_t)v : (int32_t)0x80000000;
}

/* ---- BT / BTS / BTR / BTC ----
 *
 * With a memory destination and a register bit index, the index is NOT masked
 * to the operand size: it selects a dword away from the base address, which is
 * how a bitmap of arbitrary size gets addressed. Masking it the way the
 * register form does would quietly read and write the wrong word - the kind of
 * wrong that shows up as unrelated corruption much later.
 *
 * op: 0 test, 1 set, 2 reset, 3 complement. CF receives the bit as it was. */
SC2_EN_LIGNE uint32_t bit_string_op(CPU *c, uint32_t addr, int32_t idx, int op)
{
    uint32_t a = addr + 4u * (uint32_t)(idx >> 5);
    unsigned b = (unsigned)(idx & 31);
    uint32_t w = rd32(a);
    c->cf = (w >> b) & 1u;
    if      (op == 1) w |=  (1u << b);
    else if (op == 2) w &= ~(1u << b);
    else if (op == 3) w ^=  (1u << b);
    if (op) wr32(a, w);
    return c->cf;
}

/* The register form, where the index really is taken modulo the width. */
SC2_EN_LIGNE uint32_t bit_reg_op(CPU *c, uint32_t *r, uint32_t idx, int op)
{
    unsigned b = idx & 31u;
    c->cf = (*r >> b) & 1u;
    if      (op == 1) *r |=  (1u << b);
    else if (op == 2) *r &= ~(1u << b);
    else if (op == 3) *r ^=  (1u << b);
    return c->cf;
}

/* ---- 80-bit extended precision ----
 *
 * The x87 stack here is modelled as doubles, which is right for almost
 * everything - but `fld tbyte` and `fstp tbyte` name the hardware's own
 * 80-bit format directly, and a compiler emits them to spill a register
 * without losing precision. Reading one as anything else returns nonsense.
 *
 * The format is a sign bit, a 15-bit exponent biased by 16383, and a 64-bit
 * mantissa whose integer bit is EXPLICIT - unlike float and double, where it
 * is implied. Converting to double loses the extra bits, which is the same
 * loss the rest of this model already accepts.
 */
SC2_EN_LIGNE double rdf80(uint32_t a)
{
    unsigned char b[10];
    SC2_LIRE(b, a, 10);

    uint64_t mant;
    uint16_t se;
    memcpy(&mant, b, 8);
    memcpy(&se, b + 8, 2);

    int sign = (se >> 15) & 1;
    int exp  = se & 0x7FFF;

    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7FFF) {
        if (mant << 1) return (double)NAN;
        return sign ? -(double)INFINITY : (double)INFINITY;
    }
    double m = (double)mant * 5.4210108624275222e-20;   /* 2^-64 */
    return ldexp(sign ? -m : m, exp - 16383 + 1);
}

SC2_EN_LIGNE void wrf80(uint32_t a, double v)
{
    unsigned char b[10];
    uint64_t mant = 0;
    uint16_t se = 0;

    memset(b, 0, sizeof b);
    if (v != v) {                                   /* NaN */
        se = 0x7FFF; mant = 0xC000000000000000ull;
    } else if (v == (double)INFINITY || v == -(double)INFINITY) {
        se = 0x7FFF; mant = 0x8000000000000000ull;
        if (v < 0) se |= 0x8000;
    } else if (v != 0.0) {
        int e;
        double m = frexp(signbit(v) ? -v : v, &e);   /* m in [0.5, 1) */
        mant = (uint64_t)ldexp(m, 64);
        se = (uint16_t)((e - 1 + 16383) & 0x7FFF);
        if (signbit(v)) se |= 0x8000;
    } else if (signbit(v)) {
        se = 0x8000;                                 /* -0.0: mantissa 0, sign set */
    }
    memcpy(b, &mant, 8);
    memcpy(b + 8, &se, 2);
    SC2_ECRIRE(a, b, 10);
}

/* FPREM: the remainder of st0/st1 with the quotient truncated toward zero,
 * which is what fmod computes. The hardware may reduce only partially and set
 * C2 to say "call me again"; computing the whole thing at once means C2 is
 * always clear, and the loop the compiler wrote around it runs once. */
SC2_EN_LIGNE double fprem_op(CPU *c, double a, double b)
{
    c->fpu_sw &= ~0x0400u;              /* C2 = 0: reduction complete */
    return fmod(a, b);
}
/* The packed integer ops that saturate rather than wrap. PADDSW and friends
 * clamp to the lane's own range instead of carrying into the next lane, and
 * the arithmetic has to be done a width up for the clamp to see the overflow
 * at all - which is the whole reason these are functions and not expressions
 * in the generated code. */
SC2_EN_LIGNE int8_t   sse_sat_i8 (int32_t v) { return v >  127 ?  127 : v < -128 ? -128 : (int8_t)v; }
SC2_EN_LIGNE int16_t  sse_sat_i16(int32_t v) { return v >  32767 ?  32767 : v < -32768 ? -32768 : (int16_t)v; }
SC2_EN_LIGNE uint8_t  sse_sat_u8 (int32_t v) { return v >  255 ?  255 : v < 0 ? 0 : (uint8_t)v; }
SC2_EN_LIGNE uint16_t sse_sat_u16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }

/* ---- MMX: mm0-mm7 as 64-bit values (CPU.mm[]), helpers called by lift32_cpu ----
 * Lanes go through a little-endian union (x86 and AArch64 hosts both are). Not
 * aliased onto the x87 stack: see the lifter's MMX section. */
typedef union { uint64_t q; uint32_t d[2]; uint16_t w[4]; uint8_t b[8];
                int32_t sd[2]; int16_t sw[4]; int8_t sb[8]; } MMX;
SC2_EN_LIGNE MMX mmx_u(uint64_t v) { MMX m; m.q = v; return m; }
#define MMX_LANES(nom, n, ch, expr) \
    SC2_EN_LIGNE uint64_t nom(uint64_t x, uint64_t y) { \
        MMX a = mmx_u(x), b = mmx_u(y), r; r.q = 0; \
        for (int i = 0; i < (n); i++) r.ch[i] = (expr); \
        return r.q; }
MMX_LANES(mmx_paddb, 8, b, (uint8_t)(a.b[i] + b.b[i]))
MMX_LANES(mmx_paddw, 4, w, (uint16_t)(a.w[i] + b.w[i]))
MMX_LANES(mmx_paddd, 2, d, a.d[i] + b.d[i])
MMX_LANES(mmx_psubb, 8, b, (uint8_t)(a.b[i] - b.b[i]))
MMX_LANES(mmx_psubw, 4, w, (uint16_t)(a.w[i] - b.w[i]))
MMX_LANES(mmx_psubd, 2, d, a.d[i] - b.d[i])
SC2_EN_LIGNE uint64_t mmx_paddq(uint64_t x, uint64_t y) { return x + y; }
SC2_EN_LIGNE uint64_t mmx_psubq(uint64_t x, uint64_t y) { return x - y; }
MMX_LANES(mmx_paddsb, 8, sb, sse_sat_i8((int32_t)a.sb[i] + b.sb[i]))
MMX_LANES(mmx_paddsw, 4, sw, sse_sat_i16((int32_t)a.sw[i] + b.sw[i]))
MMX_LANES(mmx_paddusb, 8, b, sse_sat_u8((int32_t)a.b[i] + b.b[i]))
MMX_LANES(mmx_paddusw, 4, w, sse_sat_u16((int32_t)a.w[i] + b.w[i]))
MMX_LANES(mmx_psubsb, 8, sb, sse_sat_i8((int32_t)a.sb[i] - b.sb[i]))
MMX_LANES(mmx_psubsw, 4, sw, sse_sat_i16((int32_t)a.sw[i] - b.sw[i]))
MMX_LANES(mmx_psubusb, 8, b, sse_sat_u8((int32_t)a.b[i] - b.b[i]))
MMX_LANES(mmx_psubusw, 4, w, sse_sat_u16((int32_t)a.w[i] - b.w[i]))
MMX_LANES(mmx_pmullw, 4, w, (uint16_t)((int32_t)a.sw[i] * b.sw[i]))
MMX_LANES(mmx_pmulhw, 4, w, (uint16_t)(((int32_t)a.sw[i] * b.sw[i]) >> 16))
MMX_LANES(mmx_pmulhuw, 4, w, (uint16_t)(((uint32_t)a.w[i] * b.w[i]) >> 16))
MMX_LANES(mmx_pmaddwd, 2, d, (uint32_t)((int64_t)a.sw[2 * i] * b.sw[2 * i] + (int64_t)a.sw[2 * i + 1] * b.sw[2 * i + 1]))
MMX_LANES(mmx_pmaxsw, 4, sw, a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i])
MMX_LANES(mmx_pminsw, 4, sw, a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i])
MMX_LANES(mmx_pmaxub, 8, b, a.b[i] > b.b[i] ? a.b[i] : b.b[i])
MMX_LANES(mmx_pminub, 8, b, a.b[i] < b.b[i] ? a.b[i] : b.b[i])
MMX_LANES(mmx_pavgb, 8, b, (uint8_t)(((uint32_t)a.b[i] + b.b[i] + 1) >> 1))
MMX_LANES(mmx_pavgw, 4, w, (uint16_t)(((uint32_t)a.w[i] + b.w[i] + 1) >> 1))
MMX_LANES(mmx_pcmpeqb, 8, b, a.b[i] == b.b[i] ? 0xFFu : 0u)
MMX_LANES(mmx_pcmpeqw, 4, w, a.w[i] == b.w[i] ? 0xFFFFu : 0u)
MMX_LANES(mmx_pcmpeqd, 2, d, a.d[i] == b.d[i] ? 0xFFFFFFFFu : 0u)
MMX_LANES(mmx_pcmpgtb, 8, b, a.sb[i] > b.sb[i] ? 0xFFu : 0u)
MMX_LANES(mmx_pcmpgtw, 4, w, a.sw[i] > b.sw[i] ? 0xFFFFu : 0u)
MMX_LANES(mmx_pcmpgtd, 2, d, a.sd[i] > b.sd[i] ? 0xFFFFFFFFu : 0u)
MMX_LANES(mmx_punpcklbw, 8, b, (i & 1) ? b.b[i >> 1] : a.b[i >> 1])
MMX_LANES(mmx_punpckhbw, 8, b, (i & 1) ? b.b[4 + (i >> 1)] : a.b[4 + (i >> 1)])
MMX_LANES(mmx_punpcklwd, 4, w, (i & 1) ? b.w[i >> 1] : a.w[i >> 1])
MMX_LANES(mmx_punpckhwd, 4, w, (i & 1) ? b.w[2 + (i >> 1)] : a.w[2 + (i >> 1)])
MMX_LANES(mmx_punpckldq, 2, d, i ? b.d[0] : a.d[0])
MMX_LANES(mmx_punpckhdq, 2, d, i ? b.d[1] : a.d[1])
MMX_LANES(mmx_packsswb, 8, sb, sse_sat_i8(i < 4 ? a.sw[i] : b.sw[i - 4]))
MMX_LANES(mmx_packuswb, 8, b, sse_sat_u8(i < 4 ? a.sw[i] : b.sw[i - 4]))
MMX_LANES(mmx_packssdw, 4, sw, sse_sat_i16(i < 2 ? a.sd[i] : b.sd[i - 2]))
SC2_EN_LIGNE uint64_t mmx_pmuludq(uint64_t x, uint64_t y) { return (uint64_t)(uint32_t)x * (uint32_t)y; }
SC2_EN_LIGNE uint64_t mmx_psadbw(uint64_t x, uint64_t y)
{ MMX a = mmx_u(x), b = mmx_u(y); uint32_t s = 0; for (int i = 0; i < 8; i++) s += a.b[i] > b.b[i] ? a.b[i] - b.b[i] : b.b[i] - a.b[i]; return s; }
SC2_EN_LIGNE uint64_t mmx_pand(uint64_t x, uint64_t y)  { return x & y; }
SC2_EN_LIGNE uint64_t mmx_pandn(uint64_t x, uint64_t y) { return ~x & y; }
SC2_EN_LIGNE uint64_t mmx_por(uint64_t x, uint64_t y)   { return x | y; }
SC2_EN_LIGNE uint64_t mmx_pxor(uint64_t x, uint64_t y)  { return x ^ y; }
/* Shifts: the count is the whole 64-bit operand; a count past the lane width
 * clears the lane (logical) or fills it with the sign (arithmetic). */
#define MMX_SHL(nom, n, ch, bits) \
    SC2_EN_LIGNE uint64_t nom(uint64_t x, uint64_t k) { MMX a = mmx_u(x), r; r.q = 0; \
        if (k >= (bits)) return 0; for (int i = 0; i < (n); i++) r.ch[i] = a.ch[i] << k; return r.q; }
#define MMX_SHR(nom, n, ch, bits) \
    SC2_EN_LIGNE uint64_t nom(uint64_t x, uint64_t k) { MMX a = mmx_u(x), r; r.q = 0; \
        if (k >= (bits)) return 0; for (int i = 0; i < (n); i++) r.ch[i] = a.ch[i] >> k; return r.q; }
MMX_SHL(mmx_psllw, 4, w, 16)
MMX_SHL(mmx_pslld, 2, d, 32)
MMX_SHR(mmx_psrlw, 4, w, 16)
MMX_SHR(mmx_psrld, 2, d, 32)
SC2_EN_LIGNE uint64_t mmx_psllq(uint64_t x, uint64_t k) { return k >= 64 ? 0 : x << k; }
SC2_EN_LIGNE uint64_t mmx_psrlq(uint64_t x, uint64_t k) { return k >= 64 ? 0 : x >> k; }
SC2_EN_LIGNE uint64_t mmx_psraw(uint64_t x, uint64_t k)
{ MMX a = mmx_u(x), r; if (k > 15) k = 15; for (int i = 0; i < 4; i++) r.sw[i] = (int16_t)(a.sw[i] >> k); return r.q; }
SC2_EN_LIGNE uint64_t mmx_psrad(uint64_t x, uint64_t k)
{ MMX a = mmx_u(x), r; if (k > 31) k = 31; for (int i = 0; i < 2; i++) r.sd[i] = a.sd[i] >> k; return r.q; }
SC2_EN_LIGNE uint64_t mmx_pshufw(uint64_t x, unsigned imm)
{ MMX a = mmx_u(x), r; for (int i = 0; i < 4; i++) r.w[i] = a.w[(imm >> (2 * i)) & 3]; return r.q; }
SC2_EN_LIGNE uint32_t mmx_pmovmskb(uint64_t x)
{ uint32_t m = 0; for (int i = 0; i < 8; i++) m |= (uint32_t)((x >> (8 * i + 7)) & 1) << i; return m; }
SC2_EN_LIGNE uint32_t mmx_pextrw(uint64_t x, unsigned i) { return (uint16_t)(x >> (16 * (i & 3))); }
SC2_EN_LIGNE uint64_t mmx_pinsrw(uint64_t x, uint16_t v, unsigned i)
{ unsigned s = 16 * (i & 3); return (x & ~((uint64_t)0xFFFF << s)) | ((uint64_t)v << s); }

#endif /* PCRECOMP_CPU_H */
