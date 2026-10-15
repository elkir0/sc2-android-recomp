/* sc2_ucontext.c — makecontext pour bionic AArch64 (passe 7a) ; cf. sc2_ucontext_aarch64.S. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "compat/sc2_android_compat.h"
#include "sc2_ucontext_offsets.h"

_Static_assert(offsetof(ucontext_t, uc_link) == UC_LINK, "uc_link");
_Static_assert(offsetof(ucontext_t, uc_mcontext.regs) == UC_REGS, "regs");
_Static_assert(offsetof(ucontext_t, uc_mcontext.sp) == UC_SP, "sp");
_Static_assert(offsetof(ucontext_t, uc_mcontext.pc) == UC_PC, "pc");
_Static_assert(offsetof(ucontext_t, uc_mcontext.__reserved) == UC_FPR, "__reserved");

void sc2_ucontext_fin(void);

/* Comme la glibc : la fonction démarre sur uc_stack avec au plus 8 arguments entiers (x0..x7) ;
 * à son retour, sc2_ucontext_fin reprend uc_link (ou termine le processus). */
void makecontext(ucontext_t *ucp, void (*fn)(void), int argc, ...)
{
    uintptr_t sp = ((uintptr_t)ucp->uc_stack.ss_sp + ucp->uc_stack.ss_size) & ~(uintptr_t)15;
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc && i < 8; i++) ucp->uc_mcontext.regs[i] = va_arg(ap, uint64_t);
    va_end(ap);
    ucp->uc_mcontext.regs[19] = (uint64_t)(uintptr_t)ucp->uc_link;
    ucp->uc_mcontext.regs[29] = 0;                               /* fin de la chaîne de cadres */
    ucp->uc_mcontext.regs[30] = (uint64_t)(uintptr_t)sc2_ucontext_fin;
    ucp->uc_mcontext.sp = sp;
    ucp->uc_mcontext.pc = (uint64_t)(uintptr_t)fn;
}
