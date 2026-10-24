/* sc2_vars_regle.h — règle de sauvegarde du Variables.txt de /sdcard (23/10) : les réglages d'un run DE
 * TEST (banc scripté, préréglage forcé par la séance du Fold, progression de test) ne doivent JAMAIS y
 * être recopiés au lancement suivant — le 23/10, le Medium de l'étape 4 de la séance a remplacé le
 * Variables.txt du mainteneur —, et un fichier posé par le lanceur Java ENTRE deux runs ne doit pas non plus
 * être écrasé par la copie plus ancienne du run précédent. Pur calcul, sans Android :
 *   - sc2_vars_raison_test : CE lancement est-il un run de test ? (configurer pose alors le marqueur
 *     SC2_VARS_MARQUE_TEST à la racine du sc2-c neuf, avec la raison en 1re ligne ; le lancement SUIVANT
 *     le lit dans sauver_vars AVANT d'effacer sc2-c, puis le marqueur disparaît avec lui) ;
 *   - sc2_vars_decision : le Variables.txt laissé dans sc2-c par le run précédent peut-il être recopié
 *     vers /sdcard ? Ordre des refus : FIGE, ABSENT, TAILLE, SANS_GRAPHICSOPTION, RUN_DE_TEST,
 *     DST_PLUS_RECENT ; sinon COPIER.
 * Testé sur l'hôte (android/jni/tests/test_vars_regle.c). */
#ifndef SC2_VARS_REGLE_H
#define SC2_VARS_REGLE_H

#include <string.h>

/* Marqueur d'un run de test : à la racine de sc2-c, effacé avec lui au lancement suivant. */
#define SC2_VARS_MARQUE_TEST ".sc2-run-de-test"

enum {
    SC2_VARS_COPIER,                    /* réglages du run précédent à recopier vers /sdcard */
    SC2_VARS_FIGE,                      /* SC2_VARIABLES_FIGE=1 au lancement courant */
    SC2_VARS_ABSENT,                    /* pas de Variables.txt dans le sc2-c du run précédent */
    SC2_VARS_TAILLE,                    /* taille hors bornes (1 o à 64 Kio, bornes de sc2_documents) */
    SC2_VARS_SANS_GRAPHICSOPTION,       /* pas la section des réglages graphiques du jeu */
    SC2_VARS_RUN_DE_TEST,               /* marqueur présent : le run précédent était un test */
    SC2_VARS_DST_PLUS_RECENT            /* /sdcard modifié APRÈS la copie du run précédent (lanceur ?) */
};

/* Ce lancement est-il un run de test ? Arguments : valeurs brutes de getenv (peuvent être NULL). Rend
 * NULL pour un lancement utilisateur normal, sinon une courte raison française (celle du marqueur). */
static inline const char *sc2_vars_raison_test(const char *banc_entrees, const char *documents,
                                               const char *variables, const char *fige)
{
    if (banc_entrees && *banc_entrees) return "SC2_BANC_ENTREES posé";
    if (documents && strstr(documents, "/test-docs-")) return "SC2_DOCUMENTS de test (/test-docs-)";
    if (variables && *variables) return "SC2_VARIABLES posé";
    if (fige && !strcmp(fige, "1")) return "SC2_VARIABLES_FIGE=1";
    return NULL;
}

/* Le Variables.txt du run précédent (src) peut-il être recopié vers /sdcard (dst) ? Voir l'ordre en tête
 * de fichier ; mtimes en secondes, comparaison STRICTE (égaux → COPIER : le run reprend la main). */
static inline int sc2_vars_decision(int fige_courant, int src_present, long src_taille,
                                    int src_a_graphicsoption, int marque_test_presente,
                                    int dst_present, long long src_mtime, long long dst_mtime)
{
    if (fige_courant) return SC2_VARS_FIGE;
    if (!src_present) return SC2_VARS_ABSENT;
    if (src_taille < 1 || src_taille > 64 * 1024) return SC2_VARS_TAILLE;
    if (!src_a_graphicsoption) return SC2_VARS_SANS_GRAPHICSOPTION;
    if (marque_test_presente) return SC2_VARS_RUN_DE_TEST;
    if (dst_present && dst_mtime > src_mtime) return SC2_VARS_DST_PLUS_RECENT;
    return SC2_VARS_COPIER;
}

/* Texte français de chaque décision (journaux [VARS] de sauver_vars). */
static inline const char *sc2_vars_decision_texte(int d)
{
    switch (d) {
    case SC2_VARS_FIGE:                return "SC2_VARIABLES_FIGE=1 : préréglage figé";
    case SC2_VARS_ABSENT:              return "aucun Variables.txt dans le C:\\ du run précédent";
    case SC2_VARS_TAILLE:              return "taille hors bornes (1 o à 64 Kio)";
    case SC2_VARS_SANS_GRAPHICSOPTION: return "sans la section « GraphicsOption »";
    case SC2_VARS_RUN_DE_TEST:         return "run de test au lancement précédent";
    case SC2_VARS_DST_PLUS_RECENT:     return "/sdcard plus récent que ceux du run précédent";
    default:                           return "réglages du run précédent à recopier vers /sdcard";
    }
}

#endif
