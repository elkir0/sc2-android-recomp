/* sc2_ucontext_aarch64.S — getcontext / setcontext / swapcontext pour bionic AArch64 (passe 7a).
 *
 * Bionic n'en fournit aucune ; runtime/win32_compat.c (fibres Win32) en a besoin. Seul l'état
 * qu'exige l'AAPCS64 à un point d'appel est sauvé : x19..x30, sp, d8..d15 — swapcontext est un
 * appel de fonction, l'appelant a déjà rangé le reste. Le masque de signaux n'est PAS commuté
 * (il est le même pour toutes les fibres d'un thread ; économise un appel système par
 * SwitchToFiber). x18 (registre plate-forme, shadow call stack) n'est jamais touché.
 */
#include "sc2_ucontext_offsets.h"

    .text
    .p2align 4

/* Range l'état callee-saved de l'appelant dans le contexte x0 ; reprise = retour de l'appel avec 0. */
.macro SAUVER
    stp x19, x20, [x0, #UC_REGS + 19*8]
    stp x21, x22, [x0, #UC_REGS + 21*8]
    stp x23, x24, [x0, #UC_REGS + 23*8]
    stp x25, x26, [x0, #UC_REGS + 25*8]
    stp x27, x28, [x0, #UC_REGS + 27*8]
    stp x29, x30, [x0, #UC_REGS + 29*8]
    str xzr,      [x0, #UC_REGS + 0]       /* x0 à la reprise : 0 */
    mov x9, sp
    str x9,       [x0, #UC_SP]
    str x30,      [x0, #UC_PC]             /* reprise : juste après l'appel */
    add x10, x0, #UC_FPR                   /* stp : décalage immédiat ≤ 504 */
    stp d8,  d9,  [x10, #0]
    stp d10, d11, [x10, #16]
    stp d12, d13, [x10, #32]
    stp d14, d15, [x10, #48]
.endm

    .globl getcontext
    .type  getcontext, %function
getcontext:
    SAUVER
    mov x0, #0
    ret
    .size getcontext, . - getcontext

    .globl setcontext
    .type  setcontext, %function
setcontext:
    mov x9, x0
    add x10, x9, #UC_FPR
    ldp d8,  d9,  [x10, #0]
    ldp d10, d11, [x10, #16]
    ldp d12, d13, [x10, #32]
    ldp d14, d15, [x10, #48]
    ldp x19, x20, [x9, #UC_REGS + 19*8]
    ldp x21, x22, [x9, #UC_REGS + 21*8]
    ldp x23, x24, [x9, #UC_REGS + 23*8]
    ldp x25, x26, [x9, #UC_REGS + 25*8]
    ldp x27, x28, [x9, #UC_REGS + 27*8]
    ldp x29, x30, [x9, #UC_REGS + 29*8]
    ldr x10,      [x9, #UC_SP]
    mov sp, x10
    ldr x16,      [x9, #UC_PC]
    ldp x0, x1,   [x9, #UC_REGS + 0]       /* arguments posés par makecontext (x0..x7) */
    ldp x2, x3,   [x9, #UC_REGS + 16]
    ldp x4, x5,   [x9, #UC_REGS + 32]
    ldp x6, x7,   [x9, #UC_REGS + 48]
    br  x16
    .size setcontext, . - setcontext

    .globl swapcontext
    .type  swapcontext, %function
swapcontext:
    SAUVER
    mov x0, x1
    b   setcontext
    .size swapcontext, . - swapcontext

/* Retour de la fonction d'un contexte makecontext : x19 = uc_link (posé par makecontext). */
    .globl sc2_ucontext_fin
    .hidden sc2_ucontext_fin
    .type  sc2_ucontext_fin, %function
sc2_ucontext_fin:
    cbz x19, 1f
    mov x0, x19
    b   setcontext
1:  mov x0, #0
    bl  exit                               /* comme la glibc : uc_link NULL → fin du processus */
    brk #0
    .size sc2_ucontext_fin, . - sc2_ucontext_fin

    .section .note.GNU-stack, "", %progbits
