/* test_vars_regle.c — règle de sauvegarde du Variables.txt de /sdcard (android/jni/sc2_vars_regle.h),
 * sur l'hôte :
 *   cc -std=gnu17 -Wall -Wextra -I android/jni -o /tmp/test_vars_regle android/jni/tests/test_vars_regle.c && /tmp/test_vars_regle */
#include <stdio.h>
#include <string.h>
#include "sc2_vars_regle.h"

static int echecs;
static void verifier(const char *nom, int obtenu, int attendu)
{
    if (obtenu != attendu) {
        printf("ÉCHEC %s\n  obtenu  : %d (%s)\n  attendu : %d (%s)\n", nom, obtenu, sc2_vars_decision_texte(obtenu),
               attendu, sc2_vars_decision_texte(attendu));
        echecs++;
    } else printf("ok   %s\n", nom);
}
static void verifier_raison(const char *nom, const char *obtenu, const char *attendu)
{
    if (obtenu != attendu) {                                     /* NULL attendu ↔ NULL obtenu, sinon chaînes */
        if (!obtenu || !attendu || strcmp(obtenu, attendu)) {
            printf("ÉCHEC %s\n  obtenu  : %s\n  attendu : %s\n", nom, obtenu ? obtenu : "(NULL)", attendu ? attendu : "(NULL)");
            echecs++;
            return;
        }
    }
    printf("ok   %s\n", nom);
}

int main(void)
{
    /* run de test : les 4 déclencheurs, puis un lancement utilisateur normal (tout NULL / vide) */
    verifier_raison("banc d'entrées", sc2_vars_raison_test("@/sdcard/SC2Partout/jeu-actif.txt", NULL, NULL, NULL),
                    "SC2_BANC_ENTREES posé");
    verifier_raison("documents de test", sc2_vars_raison_test(NULL, "/sdcard/SC2Partout/test-docs-seance", NULL, NULL),
                    "SC2_DOCUMENTS de test (/test-docs-)");
    verifier_raison("préréglage explicite", sc2_vars_raison_test(NULL, NULL, "/sdcard/SC2Partout/test-variables.txt", NULL),
                    "SC2_VARIABLES posé");
    verifier_raison("figé", sc2_vars_raison_test(NULL, NULL, NULL, "1"), "SC2_VARIABLES_FIGE=1");
    verifier_raison("variables=0 aussi un test", sc2_vars_raison_test(NULL, NULL, "0", NULL), "SC2_VARIABLES posé");
    verifier_raison("lancement normal", sc2_vars_raison_test(NULL, NULL, NULL, NULL), NULL);
    verifier_raison("chaînes vides = normal", sc2_vars_raison_test("", "", "", ""), NULL);
    verifier_raison("figé=0 = normal", sc2_vars_raison_test(NULL, NULL, NULL, "0"), NULL);

    /* décision : chaque valeur de l'énumération */
    verifier("copier", sc2_vars_decision(0, 1, 4096, 1, 0, 1, 1000, 1000), SC2_VARS_COPIER);
    verifier("figé", sc2_vars_decision(1, 1, 4096, 1, 1, 1, 1000, 999), SC2_VARS_FIGE);
    verifier("absent", sc2_vars_decision(0, 0, 0, 0, 0, 0, 0, 0), SC2_VARS_ABSENT);
    verifier("taille nulle", sc2_vars_decision(0, 1, 0, 1, 0, 0, 0, 0), SC2_VARS_TAILLE);
    verifier("taille 64 Kio + 1", sc2_vars_decision(0, 1, 64 * 1024 + 1, 1, 0, 0, 0, 0), SC2_VARS_TAILLE);
    verifier("taille 64 Kio pile = copier", sc2_vars_decision(0, 1, 64 * 1024, 1, 0, 0, 0, 0), SC2_VARS_COPIER);
    verifier("sans GraphicsOption", sc2_vars_decision(0, 1, 4096, 0, 0, 0, 0, 0), SC2_VARS_SANS_GRAPHICSOPTION);
    verifier("run de test précédent", sc2_vars_decision(0, 1, 4096, 1, 1, 0, 1000, 999), SC2_VARS_RUN_DE_TEST);
    verifier("sdcard plus récent", sc2_vars_decision(0, 1, 4096, 1, 0, 1, 1000, 1001), SC2_VARS_DST_PLUS_RECENT);
    verifier("mtimes égaux = copier", sc2_vars_decision(0, 1, 4096, 1, 0, 1, 1000, 1000), SC2_VARS_COPIER);
    verifier("sdcard plus ancien = copier", sc2_vars_decision(0, 1, 4096, 1, 0, 1, 1000, 999), SC2_VARS_COPIER);
    verifier("sdcard absent = copier", sc2_vars_decision(0, 1, 4096, 1, 0, 0, 1000, 0), SC2_VARS_COPIER);

    /* ordre des refus : chacun gagne sur les suivants */
    verifier("figé avant run de test", sc2_vars_decision(1, 1, 4096, 1, 1, 0, 1000, 999), SC2_VARS_FIGE);
    verifier("absent avant taille", sc2_vars_decision(0, 0, 999999, 0, 1, 1, 1000, 1001), SC2_VARS_ABSENT);
    verifier("taille avant GraphicsOption", sc2_vars_decision(0, 1, 999999, 0, 1, 1, 1000, 1001), SC2_VARS_TAILLE);
    verifier("GraphicsOption avant run de test", sc2_vars_decision(0, 1, 4096, 0, 1, 1, 1000, 1001), SC2_VARS_SANS_GRAPHICSOPTION);
    verifier("run de test avant sdcard récent", sc2_vars_decision(0, 1, 4096, 1, 1, 1, 1000, 1001), SC2_VARS_RUN_DE_TEST);

    /* le texte est distinct pour chaque valeur (une raison collée au mauvais refus se verrait au journal) */
    const char *textes[7];
    int i, j, doublons = 0;
    textes[SC2_VARS_COPIER] = sc2_vars_decision_texte(SC2_VARS_COPIER);
    textes[SC2_VARS_FIGE] = sc2_vars_decision_texte(SC2_VARS_FIGE);
    textes[SC2_VARS_ABSENT] = sc2_vars_decision_texte(SC2_VARS_ABSENT);
    textes[SC2_VARS_TAILLE] = sc2_vars_decision_texte(SC2_VARS_TAILLE);
    textes[SC2_VARS_SANS_GRAPHICSOPTION] = sc2_vars_decision_texte(SC2_VARS_SANS_GRAPHICSOPTION);
    textes[SC2_VARS_RUN_DE_TEST] = sc2_vars_decision_texte(SC2_VARS_RUN_DE_TEST);
    textes[SC2_VARS_DST_PLUS_RECENT] = sc2_vars_decision_texte(SC2_VARS_DST_PLUS_RECENT);
    for (i = 0; i < 7; i++)
        for (j = i + 1; j < 7; j++)
            if (!strcmp(textes[i], textes[j])) { printf("ÉCHEC texte doublon : %s = %s\n", textes[i], textes[j]); doublons++; }
    verifier("textes deux à deux distincts", doublons, 0);

    printf("%s (%d échec(s))\n", echecs ? "ÉCHEC" : "TOUT OK", echecs);
    return echecs != 0;
}
