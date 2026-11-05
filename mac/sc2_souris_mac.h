/* mac/sc2_souris_mac.h — souris et molette macOS → boutons, crans et pixels du runtime, en C pur (V3, lot M2, 22/10).
 *
 * Interface : conception B §2.7 (L6), table NSEvent → appels de l'inventaire v3-entrees §6.3. Aucune dépendance à
 * AppKit : testé sur l'hôte (mac/tests/test_souris_mac.c). Utilisé par mac/sc2_entrees_mac.m (M8), sur le fil
 * principal ; l'état (boutons, cumuls) appartient à l'appelant, remis à zéro à la perte du focus.
 *
 * Chaîne (M8) :
 *   *MouseDown : b = sc2s_bouton_appui(&bt, buttonNumber, ctrl_clic) ; b ≥ 0 → sc2a_bouton(b, 1, x, y) ;
 *   *MouseUp   : b = sc2s_bouton_relache(&bt, buttonNumber) ; b ≥ 0 → sc2a_bouton(b, 0, x, y) (le bouton JOUÉ à
 *                l'appui : un Ctrl+clic relâché sans Ctrl reste un clic droit) ;
 *   scrollWheel: d = sc2s_crans(&cu, scrollingDeltaY, 0, hasPreciseScrollingDeltas, SC2_MAC_MOLETTE_PTS, inverse) ;
 *                d ≠ 0 → sc2a_molette(d, 0, x, y) ; de même scrollingDeltaX avec horiz = 1 ;
 *   position   : p = [vue convertPoint:locationInWindow fromView:nil] (vue isFlipped = YES : origine en haut à
 *                gauche), sc2s_points_vers_pixels(p.x, p.y, contentsScale, &fx, &fy), puis sc2_geo_vers_jeu. */
#ifndef SC2_SOURIS_MAC_H
#define SC2_SOURIS_MAC_H
#include <stdint.h>

/* NSEvent.buttonNumber → bouton du runtime (sc2_input_bouton : 0 gauche, 1 milieu, 2 droit) :
 * 0 → 0, 1 (droit) → 2, 2 (milieu) → 1 ; ≥ 3 ou < 0 → -1 (aucun WM_XBUTTON* dans le runtime). */
int sc2s_bouton(int ns_bouton);

/* Boutons tenus : le bouton du runtime joué à l'appui, rejoué tel quel au relâché. */
typedef struct {
    int8_t   joue[3];          /* par bouton NSEvent 0..2 : bouton runtime + 1, 0 = relâché */
    uint64_t appuis, ctrl_clics, ignores, relaches_orphelins;
} sc2s_boutons;
/* Appui : ctrl_clic ≠ 0 (Ctrl PHYSIQUE tenu, NSEventModifierFlagControl) fait d'un clic gauche un clic droit (2) ;
 * sans effet sur les autres boutons. Rend le bouton runtime, ou -1 (bouton ≥ 3 : ignoré et compté). */
int  sc2s_bouton_appui(sc2s_boutons *b, int ns_bouton, int ctrl_clic);
/* Relâché : le bouton runtime de l'appui (jamais relu des modificateurs), ou -1 sans appui connu (compté). */
int  sc2s_bouton_relache(sc2s_boutons *b, int ns_bouton);
int  sc2s_boutons_tenus(const sc2s_boutons *b);
void sc2s_boutons_reinitialiser(sc2s_boutons *b);       /* perte du focus (compteurs gardés) */

/* Molette : cumul par axe, en unités Win32 (120 = un cran). */
typedef struct { double v, h; } sc2s_cumul;
/* Ajoute d (scrollingDeltaY ou X) au cumul de l'axe et rend le nombre de crans ENTIERS, × 120 (WHEEL_DELTA) ; le reste
 * attend l'événement suivant. precis = 0 (molette à crans) : d crans ; precis ≠ 0 (trackpad, Magic Mouse) : d points,
 * points_par_cran points par cran (≤ 0 : 12, défaut de SC2_MAC_MOLETTE_PTS). inverse ≠ 0 : signe changé (choix de
 * l'appelant pour le défilement naturel ; signe : HYPOTHÈSE de l'inventaire, d > 0 = vers l'avant = Win32 > 0).
 * Borné à ±273 crans par appel (le delta de WM_MOUSEWHEEL est un SHORT) ; l'excédent est abandonné. */
int  sc2s_crans(sc2s_cumul *c, double d, int horiz, int precis, double points_par_cran, int inverse);

/* Points de la vue (retournée : y vers le bas) → pixel de backing qui CONTIENT le point (entiers, en double), à
 * passer tel quel à sc2_geo_vers_jeu (qui vise le centre, + 0,5). Hors de la vue : négatif ou au-delà, borné ensuite
 * par sc2_geo_vers_jeu (défilement au bord). echelle ≤ 0 : 1. */
void sc2s_points_vers_pixels(double px, double py, double echelle, double *fx, double *fy);
/* Inverse, pour un événement synthétique : le CENTRE du pixel (fx, fy) en points ; sc2s_points_vers_pixels le ramène
 * exactement à (fx, fy), à toute échelle. */
void sc2s_pixels_vers_points(double fx, double fy, double echelle, double *px, double *py);

#endif
