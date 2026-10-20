/* sc2_iat.h — forme d'un stub d'import (partagée par sc2_iat.c et sc2_iat_p4.c).
 * fn reçoit a = arguments invités (esp+4) ; rend eax. conv STD : l'appelé dépile
 * 4*argc octets ; CDECL : l'appelant. */
#ifndef RECOMP_SC2_IAT_H
#define RECOMP_SC2_IAT_H
#include "sc2_host.h"

typedef uint32_t (*imp_fn)(CPU *c, const uint32_t *a);
enum { STD, CDECL, DATA };
typedef struct { const char *nom; int argc; int conv; imp_fn fn; } imp_def;

/* Stubs ajoutés en passe 4 (sc2_iat_p4.c) : NULL si absent. */
const imp_def *sc2_iat_p4_find(const char *nom);
void sc2_iat_p4_install(void);

/* Battle.net FACTICE (passe 4) : module renvoyé par LoadLibrary("Battle.net.dll"),
 * ordinaux 1..4 → stubs « Battle.net.dll!#k … » de sc2_iat_p4.c. */
#define SC2_BNET_MODULE 0x7FE80000u
uint32_t sc2_bnet_ordinal(uint32_t ord);
/* VA du stub d'un import implémenté (par nom) ou d'une entrée hors IAT. */
uint32_t sc2_stub_va(const char *nom);

#endif
