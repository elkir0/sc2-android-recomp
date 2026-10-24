/* mac/sc2_catalogue_mac.h — catalogue du lanceur macOS, en C pur (V3, lot M9, 24/10/2026).
 *
 * Copie mécanique d'android/src/fr/sc2partout/Catalogue.java (passe 10b / V4.10) et d'Options.java (réglages) : les 33
 * cartes de campagne de CARTES_ARCHIVE, les 63 cartes mêlée de CARTES_MELEE avec leur nombre de joueurs, races,
 * difficultés, résolutions ; miroir de Progression.java (V5) pour les sauvegardes du dossier Saves. sc2m_cartes/
 * sc2m_n_cartes sont déclarés dans sc2_mac.h (M9 seul, aucun talon) ; ce module en est la définition, liée dans
 * recomp-sc2 par le GLOB de mac/mac.cmake.
 *
 * Les jeux de clés (sc2m_jeu_cles) traduisent un choix du lanceur en tableau (clé, valeur) pour sc2m_env_ecrire
 * (mac/sc2_config_mac.h : fusion, vals[i] == NULL RETIRE la clé). Android réécrit sc2.env entier à chaque lancement
 * (Catalogue.contenuXxx + Options.composer) ; la fusion Mac doit donc retirer explicitement les clés de l'AUTRE mode
 * de lancement : SC2_ARGS (campagne), SC2_ESCARMOUCHE* (escarmouche), SC2_SAUVEGARDE (chargement, V5).
 *
 * Rien ici n'appelle AppKit ni le runtime : testé sur l'hôte par mac/tests/test_catalogue_mac.c
 * (tests-hote/v3/catalogue.cmake), qui relit Catalogue.java et compare les listes.
 */
#ifndef SC2_CATALOGUE_MAC_H
#define SC2_CATALOGUE_MAC_H
#include <stddef.h>

/* Chemins relatifs à l'install (Catalogue.DOSSIER_CARTES / EXTENSION). */
#define SC2M_DOSSIER_CARTES "Maps/Campaign"
#define SC2M_EXTENSION      ".SC2Map"

extern const char *const sc2m_cartes[];      /* les 33 cartes de Catalogue.CARTES_ARCHIVE, dans l'ordre */
extern const int sc2m_n_cartes;              /* 33 */

extern const char *const sc2m_cartes_melee[];/* les 63 cartes mêlée, ordre de « SC2_ESCARMOUCHE=liste » */
extern const int sc2m_joueurs_melee[];       /* joueurs de chaque carte (2, 3, 4, 6 ou 8) */
extern const int sc2m_n_cartes_melee;        /* 63 */
extern const char *const sc2m_melee_defaut;  /* "SteppesofWar" (n° 13) */

extern const char *const sc2m_races[][2];    /* { valeur SC2_ESCARMOUCHE_RACE / _RACE_IA, libellé } */
extern const int sc2m_n_races;               /* 4 */
extern const char *const sc2m_difficultes[][2]; /* { valeur SC2_ESCARMOUCHE_IA, libellé } */
extern const int sc2m_n_difficultes;         /* 6 */
extern const char *const sc2m_resolutions[][2]; /* { valeur SC2_ECRAN, libellé } (Options.RESOLUTIONS) */
extern const int sc2m_n_resolutions;         /* 5 */

/* T ; « Maps/Campaign/<nom>.SC2Map » dans o (retourne o), NULL si ça ne tient pas. */
const char *sc2m_carte_chemin(const char *nom, char *o, size_t cap);

/* T ; valeur d'une clé dans un fichier sc2.env (DERNIÈRE occurrence, comme Catalogue.valeurEnv ; lignes « # » et
 * lignes sans « = » ignorées). Rend o (valeur, éventuellement vide), NULL si le fichier ne se lit pas ou la clé
 * n'y est pas. La valeur est tronquée à cap-1 octets sans erreur. */
const char *sc2m_env_valeur(const char *fichier, const char *cle, char *o, size_t cap);

/* Une sauvegarde du dossier Saves (Progression.Sauvegarde, V5) : miroir C en C pur. */
#define SC2M_SAUVEGARDES_PROFONDEUR_MAX 4      /* Progression.PROFONDEUR_MAX */
#define SC2M_SAUVEGARDE_TAILLE_MIN      4096LL /* Progression.TAILLE_MIN : archives vides de 1 181 o masquées */

typedef struct {
    char relatif[256];      /* chemin relatif au dossier Saves, séparateurs « / » (nom en tête) */
    long long date;         /* date de modification du fichier (stat st_mtime) */
    long long taille;       /* octets (stat st_size) ; utilisable = taille >= SC2M_SAUVEGARDE_TAILLE_MIN */
} sc2m_sauvegarde;

/* T ; les sauvegardes de saves_dir, de la plus récente à la plus ancienne, au plus max : fichiers *.SC2Save (sans
 * casse) à SC2M_SAUVEGARDES_PROFONDEUR_MAX niveaux sous saves_dir, récursifs ; liens symboliques jamais suivis,
 * dossiers illisibles ignorés, dossier absent → 0 rendu. Chaque trouvaille entre à sa place (ordre total de
 * Progression.ORDRE : date décroissante puis relatif croissant) et le surplus tombe : le rendu est exactement le
 * top-max de la liste complète, comme le subList(0, max) d'Android. Rend le nombre de sauvegardes, -1 si saves_dir
 * est NULL ou max <= 0. */
int sc2m_sauvegardes(const char *saves_dir, sc2m_sauvegarde *s, int max);

/* Vrai si la sauvegarde est chargeable (au moins SC2M_SAUVEGARDE_TAILLE_MIN octets — Progression.utilisable). */
int sc2m_sauvegarde_utilisable(const sc2m_sauvegarde *s);

/* Un choix du lanceur → (clé, valeur) pour sc2m_env_ecrire. vals[i] == NULL : la clé est retirée. args est le
 * tampon de la valeur SC2_ARGS construite (les autres valeurs sont des littéraux). */
#define SC2M_LANCEUR_CLES_MAX 8
typedef struct {
    const char *cles[SC2M_LANCEUR_CLES_MAX];
    const char *vals[SC2M_LANCEUR_CLES_MAX];
    int n;
    char args[512];
} sc2m_jeu_cles;

/* Menu au bar : SC2_RUN_SANS_BNET=1, tous les autres modes retirés. Rend k->n (> 0). */
int sc2m_jeu_cles_menu(sc2m_jeu_cles *k);
/* Campagne : nom PARMI sc2m_cartes (sinon -1) ; SC2_ARGS=-run Maps/Campaign/<nom>.SC2Map (guillemets si espace,
 * args_decouper du runtime les comprend), escarmouche et sauvegarde retirées. */
int sc2m_jeu_cles_carte(sc2m_jeu_cles *k, const char *nom);
/* Escarmouche : carte PARMI sc2m_cartes_melee, race et race_ia PARMI sc2m_races[i][0], difficulte PARMI
 * sc2m_difficultes[i][0] (sinon -1) ; PAS de SC2_ARGS (un -run l'emporterait sur la carte d'escarmouche). */
int sc2m_jeu_cles_escarmouche(sc2m_jeu_cles *k, const char *carte, const char *race, const char *race_ia,
                              const char *difficulte);
/* Chargement d'une sauvegarde (Catalogue.contenuSauvegarde) : SC2_RUN_SANS_BNET=1, SC2_SAUVEGARDE=<chemin>, tous les
 * autres modes retirés. chemin_relatif : relatif au dossier « Documents\StarCraft II » du jeu, séparateurs « / »
 * (les « \ » deviennent « / »), ex. « Saves/Campaign/The Outlaws.SC2Save » — l'appelant préfixe « Saves/ » comme
 * LanceurActivity.ecranCharger, qui passe « Saves/ » + relatif-au-Saves. Vide ou trop long (k->args) : -1. */
int sc2m_jeu_cles_sauvegarde(sc2m_jeu_cles *k, const char *chemin_relatif);

/* Réglages (Options.java) : ne touchent QUE leur clé, jamais les clés de lancement. Chaque rendu est 1 (une clé,
 * dont la valeur NULL quand le réglage revient au défaut du jeu : la clé est retirée de sc2.env). */
int sc2m_reglage_ecran(sc2m_jeu_cles *k, const char *resolution);   /* PARMI sc2m_resolutions[i][0], sinon -1 */
int sc2m_reglage_ombres(sc2m_jeu_cles *k, int coupees);             /* 1 = SC2_OMBRES=0, 0 = SC2_OMBRES retirée */
int sc2m_reglage_particules(sc2m_jeu_cles *k, int coupees);         /* 1 = SC2_INSTANCES=0, 0 = SC2_INSTANCES retirée */

#endif
