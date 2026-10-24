/* test_gestes.c — reconnaissance des gestes tactiles (android/jni/sc2_gestes.h), sur l'hôte :
 *   cc -std=gnu17 -Wall -Wextra -I android/jni -o /tmp/test_gestes android/jni/tests/test_gestes.c && /tmp/test_gestes */
#include <stdio.h>
#include <string.h>
#include "sc2_gestes.h"

static int echecs;
static char trace[512];
static void noter(const gestes *g)          /* concatène les actions du dernier appel : « P0(10,20) A0 R0 … » */
{
    for (int i = 0; i < g->na; i++) {
        char b[48]; const geste_action *a = &g->a[i];
        if (a->type == GACT_POSITION) snprintf(b, sizeof b, "P%d(%.0f,%.0f) ", a->bouton, a->x, a->y);
        else snprintf(b, sizeof b, "%c%d ", a->type == GACT_APPUI ? 'A' : 'R', a->bouton);
        strncat(trace, b, sizeof trace - strlen(trace) - 1);
    }
}
static void verifier(const char *nom, const char *attendu)
{
    size_t n = strlen(trace); while (n && trace[n - 1] == ' ') trace[--n] = 0;
    if (strcmp(trace, attendu)) { printf("ÉCHEC %s\n  obtenu  : %s\n  attendu : %s\n", nom, trace, attendu); echecs++; }
    else printf("ok   %s\n", nom);
    trace[0] = 0;
}

int main(void)
{
    gestes g;
    /* 1. tape d'un doigt : clic gauche à l'origine, livré au relâché */
    gestes_init(&g);
    gestes_appui(&g, 100, 200, 0); noter(&g);
    gestes_releve(&g, 101, 201, 60, 0); noter(&g);
    verifier("tape = clic gauche", "P0(100,200) A0 R0");

    /* 2. deux doigts posés dans le délai puis levés : clic droit au barycentre, une seule fois */
    gestes_init(&g);
    gestes_appui(&g, 100, 200, 0); noter(&g);
    gestes_appui_autre(&g, 140, 240, 50); noter(&g);
    gestes_releve_autre(&g, 140, 240, 150); noter(&g);
    gestes_bouge(&g, 300, 300, 160); noter(&g);
    gestes_releve(&g, 100, 200, 170, 0); noter(&g);
    verifier("deux doigts = clic droit", "P1(120,220) A1 R1");

    /* 3. glisser : l'appui part à l'origine dès le seuil, les déplacements suivent */
    gestes_init(&g);
    gestes_appui(&g, 100, 100, 0); noter(&g);
    gestes_bouge(&g, 110, 100, 10); noter(&g);              /* sous le seuil : rien */
    gestes_bouge(&g, 140, 100, 20); noter(&g);              /* au-delà : appui puis position */
    gestes_bouge(&g, 200, 150, 30); noter(&g);
    gestes_releve(&g, 210, 160, 40, 0); noter(&g);
    verifier("glisser = cadre", "P0(100,100) A0 P0(140,100) P0(200,150) P0(210,160) R0");

    /* 4. doigt immobile maintenu : l'appui part à l'échéance (tick), 2e doigt ensuite ignoré */
    gestes_init(&g);
    gestes_appui(&g, 50, 60, 0); noter(&g);
    gestes_tick(&g, 100); noter(&g);                        /* avant l'échéance : rien */
    if (!gestes_attend(&g)) { printf("ÉCHEC attente perdue\n"); echecs++; }
    gestes_tick(&g, 130); noter(&g);                        /* échéance : appui gauche */
    gestes_appui_autre(&g, 90, 90, 200); noter(&g);
    gestes_releve_autre(&g, 90, 90, 250); noter(&g);
    gestes_releve(&g, 50, 60, 300, 0); noter(&g);
    verifier("maintien puis 2e doigt tardif", "P0(50,60) A0 P0(50,60) R0");

    /* 5. deux doigts levés ensemble (ACTION_UP direct) : clic droit */
    gestes_init(&g);
    gestes_appui(&g, 10, 10, 0); noter(&g);
    gestes_appui_autre(&g, 30, 50, 40); noter(&g);
    gestes_releve(&g, 10, 10, 90, 0); noter(&g);
    verifier("deux doigts levés ensemble", "P1(20,30) A1 R1");

    /* 6. annulation pendant l'attente : rien ; pendant un glisser : relâche (bouton jamais coincé) */
    gestes_init(&g);
    gestes_appui(&g, 10, 10, 0); noter(&g);
    gestes_releve(&g, 10, 10, 30, 1); noter(&g);
    verifier("annulé en attente", "");
    gestes_appui(&g, 10, 10, 0); noter(&g);
    gestes_tick(&g, 200); noter(&g);
    gestes_releve(&g, 15, 15, 300, 1); noter(&g);
    verifier("annulé en glisser", "P0(10,10) A0 R0");

    printf("%s (%d échec(s)) ; bilan : %u clic(s) gauche(s), %u droit(s) sur le dernier geste suivi\n",
           echecs ? "ÉCHEC" : "TOUT OK", echecs, g.n_clics_gauches, g.n_clics_droits);
    return echecs != 0;
}
