/* sc2_trace_present.h — passe 7p3 : temps par frame du chemin de présentation (SC2_TRACE_PRESENT=1).
 *
 * Sans la variable : chaque appel rend tout de suite (un test d'entier), aucune ligne journalisée.
 * Avec : chaque frame est découpée en phases mesurées (horloge monotone) :
 *   JEU         fin du Present précédent → entrée dans le Present du backend (moteur + émission des draws)
 *   BACKEND     d3dv_present du backend : soumission de la frame + vkQueueWaitIdle (+ PPM éventuel)
 *   ACQUISITION vkAcquireNextImageKHR                       ┐
 *   ENREG       enregistrement du tampon (effacement + blit) │ présentateur swapchain
 *   SOUMISSION  vkQueueSubmit                                │ (android/jni/sc2_vk_presente.c)
 *   PRESENT     vkQueuePresentKHR                            │
 *   BARRIERE    vkWaitForFences (le blit a lu le back buffer)┘
 *   RESTE       le reste du Present de la plate-forme (copie CPU de repli, entrées, phases, FPS)
 * et CYCLE = d'un Present au suivant (= 1 / cadence). Toutes les SC2_TRACE_PRESENT_FENETRE frames (défaut
 * 600) : une ligne « [PRESENT] » (moyennes par phase, p50/p95/max du cycle, images/s de la fenêtre) ; à la
 * sortie, la même chose sur tout le run (sc2tp_bilan). */
#ifndef SC2_TRACE_PRESENT_H
#define SC2_TRACE_PRESENT_H
#include <stddef.h>

enum { TP_JEU, TP_BACKEND, TP_ACQUISITION, TP_ENREG, TP_SOUMISSION, TP_PRESENT, TP_BARRIERE, TP_RESTE, TP_N };

int  sc2tp_actif(void);
void sc2tp_debut(int phase);             /* horodate le début de la phase */
void sc2tp_fin(int phase);               /* ajoute la durée écoulée depuis sc2tp_debut(phase) */
void sc2tp_entree_present(void);         /* entrée du Present de la plate-forme : clôt JEU, ouvre BACKEND */
void sc2tp_sortie_present(void);         /* sortie : clôt la frame (CYCLE, RESTE), journal par fenêtre */
void sc2tp_bilan(char *o, size_t cap);   /* tout le run ; o[0] = 0 si inactif */

#endif
