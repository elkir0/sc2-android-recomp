/* sc2_attentes.h — SC2_ATTENTES=1 : temps BLOQUÉ par fil et par cause.
 *
 * Instrumentation légère des primitives d'attente du runtime (Sleep, SleepEx,
 * WaitForSingleObject(Ex), MsgWait, section critique, fence Vulkan). Inactive par
 * défaut : sc2_att_debut() rend 0 tant que SC2_ATTENTES≠1 (lu une fois).
 *
 * Les fonctions sont FAIBLES pour le consommateur : sc2_d3d9v.c, aussi lié SEUL
 * dans test_d3d9v, doit compiler et lier sans le module. Motif sûr côté appelant,
 * coût nul quand inactif :
 *   uint64_t t0 = sc2_att_debut ? sc2_att_debut() : 0;  … ;  if (t0) sc2_att_fin(…);
 * runtime/sc2_attentes.c définit SC2_ATTENTES_IMPL avant d'inclure ce titre : sa
 * définition est alors FORTE (l'attribut faible ne fuirait pas dans le module).
 */
#ifndef SC2_ATTENTES_H
#define SC2_ATTENTES_H

#include <stdint.h>

/* Causes d'attente — une entrée par primitive bloquante du runtime. */
enum { ATT_SLEEP, ATT_SLEEPEX, ATT_WAIT, ATT_MSGWAIT, ATT_CS, ATT_VK_FENCE, ATT_N };

#ifdef SC2_ATTENTES_IMPL
#define ATT_API
#else
#define ATT_API __attribute__((weak))
#endif

ATT_API uint64_t sc2_att_debut(void);  /* 0 si SC2_ATTENTES≠1 ; sinon ns CLOCK_MONOTONIC */
ATT_API void sc2_att_fin(int cause, uint32_t ms_demandes, uint64_t t0, uint32_t ret_va);
ATT_API void sc2_att_present(void);    /* 1×/image, fil du jeu : marque CE fil, ligne périodique */
ATT_API void sc2_att_bilan(void);      /* bilan (aussi enregistré par atexit à la 1re activation) */

#endif
