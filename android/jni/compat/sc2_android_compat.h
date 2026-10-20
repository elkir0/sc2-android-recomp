/* sc2_android_compat.h — inclus de force (-include) dans les sources du runtime compilées pour
 * Android (passe 7a). Bionic DÉCLARE ucontext_t mais n'implémente ni getcontext ni makecontext ni
 * swapcontext : les fibres de runtime/win32_compat.c les trouvent dans sc2_ucontext_aarch64.S.
 * Aucune modification de recomp-sc2/runtime/ (zone de la passe 5d).
 */
#ifndef SC2_ANDROID_COMPAT_H
#define SC2_ANDROID_COMPAT_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ucontext.h>

int  getcontext(ucontext_t *ucp);
int  setcontext(const ucontext_t *ucp);
void makecontext(ucontext_t *ucp, void (*fn)(void), int argc, ...);
int  swapcontext(ucontext_t *oucp, const ucontext_t *ucp);

#endif
