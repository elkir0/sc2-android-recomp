/* sc2_input.h — entrées clavier/souris de recomp-sc2 : état injectable (passe 5f).
 *
 * Ce que le jeu lit (inventaire TRACE-PASSE5F §2) :
 *   - la FILE DE MESSAGES, une fois par frame : PeekMessageW(PM_NOREMOVE) en 0x0099A3C6 ; s'il y a un
 *     message, GetMessageW → TranslateMessage → DispatchMessageW → wndproc du jeu (0x0099A600) ;
 *   - le curseur, deux fois par frame : GetCursorPos (0x009998C2) puis ScreenToClient (0x009998D3) ;
 *   - GetAsyncKeyState (0x00999861, 0x00999E04), GetKeyState, GetKeyboardState.
 * Ce module tient un état (touches, curseur, boutons) et une file de messages Win32, servis à ces
 * imports. Sources d'événements :
 *   - SC2_INPUT_SIM=<fichier> : script rejoué, lignes
 *         frame <n> touche <vk> down|up
 *         frame <n> souris <x> <y> bouton <g|m|d> down|up
 *         frame <n> souris <x> <y>                       (déplacement seul)
 *         frame <n> molette <delta> [<x> <y>]            (passe 8h : WM_MOUSEWHEEL, ±120 = 1 cran)
 *     (« frame n » = n-ième tour de la pompe à messages du jeu = n-ième frame au boot ; x, y en pixels
 *      CLIENT ; vk décimal ou 0x… ; # = commentaire) ;
 *   - l'API ci-dessous (glue d'une plateforme : tactile Android, clavier/souris d'un bureau).
 * Inactif (ni fichier ni sc2_input_activer) : sc2_input_find rend NULL et les lectures rendent l'état
 * neutre — comportement d'avant, bit à bit.
 */
#ifndef RECOMP_SC2_INPUT_H
#define RECOMP_SC2_INPUT_H
#include <stdint.h>
#include "sc2_iat.h"

const imp_def *sc2_input_find(const char *nom);      /* imports d'entrée servis ici (NULL : inactif) */
int      sc2_input_actif(void);
void     sc2_input_activer(void);                     /* pour une glue de plateforme, sans fichier */
void     sc2_input_touche(uint32_t vk, int bas);         /* 8h : VK_L/RSHIFT, L/RCONTROL, L/RMENU acceptés (→ VK générique) */
void     sc2_input_molette(int32_t delta, int horizontale);   /* passe 8h : ±120 par cran, au curseur courant */
void     sc2_input_souris(int32_t x, int32_t y);      /* pixels client */
void     sc2_input_bouton(int bouton, int bas);       /* 0 gauche, 1 milieu, 2 droit */
uint32_t sc2_input_async(uint32_t vk);                /* GetAsyncKeyState (SHORT : bit 15 = enfoncé) */
uint32_t sc2_input_etat(uint32_t vk);                 /* GetKeyState (bit 15 enfoncé, bit 0 bascule) */

/* V3 (M1, 22/10) : glue d'une plate-forme qui fournit ses CARACTÈRES (Mac : NSEvent.characters). Tous les fils (I.mu
 * récursif). Rien ne change pour SC2_INPUT_SIM, SC2_BANC_ENTREES ni Android tant qu'elles ne sont pas appelées. */
void sc2_input_touche_plateforme(uint32_t vk, int bas); /* = sc2_input_touche, message MARQUÉ : i_TranslateMessage n'en
                                                         * synthétise aucun WM_CHAR (la synthèse US reste pour le banc,
                                                         * SC2_BANC_ENTREES et Android) ; mémorise le lParam du KEYDOWN */
void sc2_input_caractere(uint32_t cp);                  /* UTF-32 → WM_CHAR (WM_SYSCHAR si VK_MENU tenue) ; deux unités
                                                         * UTF-16 hors BMP ; lParam = celui du dernier KEYDOWN plate-forme */
void sc2_input_activation(int actif);                   /* 0 : WM_ACTIVATEAPP(0) + WM_ACTIVATE(WA_INACTIVE) postés,
                                                         * activation() SUSPENDUE ; 1 : reprise + WM_ACTIVATEAPP(1) +
                                                         * WM_ACTIVATE(WA_ACTIVE). Module inactif : état mémorisé, rien de
                                                         * posté ; SC2_ACTIVATION=0 : rien de posté ; état inchangé : rien.
                                                         * Journal « [ACTIVATION] hôte : actif|inactif (tour N) … » */
void sc2_input_tout_relacher(void);                     /* KEYUP (ou SYSKEYUP) de chaque I.bas[vk], vk ∉ {1, 2, 4}, puis
                                                         * boutons relâchés (sc2_input_bouton(b, 0)) ; état bascule intact */

#endif
