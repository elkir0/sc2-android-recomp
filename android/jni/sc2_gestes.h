/* sc2_gestes.h — reconnaissance des gestes tactiles (21/10) : 1 doigt = clic gauche, 2 doigts = clic droit.
 *
 * Pur calcul, sans Android : la glue (sc2_android_main.c) lui passe les événements du tactile en pixels de FENÊTRE avec
 * leur heure (ms), et lit en retour une courte liste d'actions souris à injecter (position, bouton gauche ou droit,
 * appui ou relâche). Testé sur l'hôte (android/jni/tests/test_gestes.c).
 *
 *   - Un premier doigt n'appuie PAS tout de suite : il attend GESTES_ATTENTE_MS. Pendant ce délai,
 *       · un 2e doigt → geste à DEUX doigts : clic DROIT au barycentre, livré au premier relâché ; le reste du geste
 *         (doigts encore posés, déplacements) est ignoré jusqu'à ce que tous les doigts soient levés ;
 *       · le doigt se lève → clic gauche (appui + relâche) à sa position ;
 *       · le doigt bouge de plus de GESTES_SEUIL_PX → l'appui gauche part à la position d'ORIGINE, puis les
 *         déplacements suivent (sélection par cadre, glisser) ;
 *     sinon, à l'échéance (gestes_tick), l'appui gauche part à la position d'origine (doigt immobile maintenu).
 *   - Une fois l'appui gauche parti, un 2e doigt est ignoré ; relâche au dernier doigt levé.
 */
#ifndef SC2_GESTES_H
#define SC2_GESTES_H

#include <stdint.h>

#define GESTES_ATTENTE_MS 120          /* délai pendant lequel un 2e doigt fait un clic droit */
#define GESTES_SEUIL_PX   24.0f        /* déplacement (pixels de fenêtre) qui engage le glisser gauche */
#define GESTES_MAX_ACTIONS 8

enum { GESTE_REPOS, GESTE_ATTENTE, GESTE_GAUCHE, GESTE_DEUX };
enum { GACT_POSITION, GACT_APPUI, GACT_RELACHE };        /* bouton : 0 gauche, 1 droit */

typedef struct { int type, bouton; float x, y; } geste_action;
typedef struct {
    int etat;
    int64_t t0;                        /* heure de l'appui du 1er doigt */
    float x0, y0;                      /* position de l'appui du 1er doigt */
    float x2, y2;                      /* position du 2e doigt (geste à deux) */
    int n_doigts;
    geste_action a[GESTES_MAX_ACTIONS]; int na;    /* actions produites par le DERNIER appel */
    unsigned n_clics_droits, n_clics_gauches;      /* bilan */
} gestes;

static inline void gestes_init(gestes *g) { gestes s = { 0 }; *g = s; }
static inline void gestes_pousser(gestes *g, int type, int bouton, float x, float y)
{
    if (g->na < GESTES_MAX_ACTIONS) { geste_action a = { type, bouton, x, y }; g->a[g->na++] = a; }
}
static inline void gestes_appui_gauche(gestes *g)
{
    gestes_pousser(g, GACT_POSITION, 0, g->x0, g->y0);
    gestes_pousser(g, GACT_APPUI, 0, g->x0, g->y0);
    g->etat = GESTE_GAUCHE; g->n_clics_gauches++;
}

/* Premier doigt posé (ACTION_DOWN). */
static inline void gestes_appui(gestes *g, float x, float y, int64_t t)
{
    g->na = 0;
    g->etat = GESTE_ATTENTE; g->t0 = t; g->x0 = x; g->y0 = y; g->n_doigts = 1;
}
/* Doigt supplémentaire posé (ACTION_POINTER_DOWN). */
static inline void gestes_appui_autre(gestes *g, float x, float y, int64_t t)
{
    (void)t;
    g->na = 0; g->n_doigts++;
    if (g->etat == GESTE_ATTENTE) { g->etat = GESTE_DEUX; g->x2 = x; g->y2 = y; }
}
/* Déplacement du 1er doigt (ACTION_MOVE, pointeur d'indice 0). */
static inline void gestes_bouge(gestes *g, float x, float y, int64_t t)
{
    (void)t;
    g->na = 0;
    if (g->etat == GESTE_ATTENTE) {
        float dx = x - g->x0, dy = y - g->y0;
        if (dx * dx + dy * dy > GESTES_SEUIL_PX * GESTES_SEUIL_PX) gestes_appui_gauche(g);
    }
    if (g->etat == GESTE_GAUCHE) gestes_pousser(g, GACT_POSITION, 0, x, y);
}
/* Un doigt levé alors que d'autres restent (ACTION_POINTER_UP). */
static inline void gestes_releve_autre(gestes *g, float x, float y, int64_t t)
{
    (void)x; (void)y; (void)t;
    g->na = 0;
    if (g->n_doigts > 1) g->n_doigts--;
    if (g->etat == GESTE_DEUX && g->n_doigts >= 1) {           /* clic droit dès le premier relâché */
        float cx = (g->x0 + g->x2) / 2, cy = (g->y0 + g->y2) / 2;
        gestes_pousser(g, GACT_POSITION, 1, cx, cy);
        gestes_pousser(g, GACT_APPUI, 1, cx, cy);
        gestes_pousser(g, GACT_RELACHE, 1, cx, cy);
        g->n_clics_droits++;
        g->etat = GESTE_REPOS;                                   /* le reste du geste est ignoré */
    }
}
/* Dernier doigt levé (ACTION_UP) ou geste annulé (cancel = 1 : ACTION_CANCEL, aucun clic). */
static inline void gestes_releve(gestes *g, float x, float y, int64_t t, int cancel)
{
    (void)t;
    g->na = 0;
    if (g->etat == GESTE_ATTENTE && !cancel) {                  /* tape : clic gauche à l'origine */
        gestes_appui_gauche(g);
        gestes_pousser(g, GACT_RELACHE, 0, g->x0, g->y0);
    } else if (g->etat == GESTE_GAUCHE) {
        if (!cancel) gestes_pousser(g, GACT_POSITION, 0, x, y);
        gestes_pousser(g, GACT_RELACHE, 0, x, y);            /* annulé : relâché quand même (bouton jamais coincé) */
    } else if (g->etat == GESTE_DEUX && !cancel) {               /* les deux doigts levés ensemble */
        float cx = (g->x0 + g->x2) / 2, cy = (g->y0 + g->y2) / 2;
        gestes_pousser(g, GACT_POSITION, 1, cx, cy);
        gestes_pousser(g, GACT_APPUI, 1, cx, cy);
        gestes_pousser(g, GACT_RELACHE, 1, cx, cy);
        g->n_clics_droits++;
    }
    g->etat = GESTE_REPOS; g->n_doigts = 0;
}
/* Horloge (boucle de la glue, ~ toutes les 16 ms tant que gestes_attend) : échéance de l'attente → appui gauche. */
static inline void gestes_tick(gestes *g, int64_t t)
{
    g->na = 0;
    if (g->etat == GESTE_ATTENTE && t - g->t0 >= GESTES_ATTENTE_MS) gestes_appui_gauche(g);
}
static inline int gestes_attend(const gestes *g) { return g->etat == GESTE_ATTENTE; }

#endif
