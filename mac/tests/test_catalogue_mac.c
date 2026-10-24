/* test_catalogue_mac.c — V3, lot M9 (24/10/2026) : catalogue du lanceur macOS (mac/sc2_catalogue_mac.c).
 *
 * La source de vérité est android/src/fr/sc2partout/Catalogue.java (et Options.java) : ce test RELIT le fichier Java
 * (chemin en argv[1], passé par tests-hote/v3/catalogue.cmake) et compare, élément par élément et dans l'ordre :
 *   1. sc2m_cartes == CARTES_ARCHIVE (33 noms) — critère 6 de la fiche M9 : « diff » mécanique avec Catalogue.java ;
 *   2. sc2m_cartes_melee == CARTES_MELEE (63) et sc2m_joueurs_melee == JOUEURS_MELEE (répartition 15/1/25/11/11) ;
 *   3. races, difficultés, résolutions, carte par défaut (RACES, DIFFICULTES, Options.RESOLUTIONS, MELEE_DEFAUT) ;
 *   4. les jeux de clés : menu au bar, carte (SC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map, guillemets si espace,
 *      clés de l'autre mode RETIRÉES — vals NULL), escarmouche (sans SC2_ARGS), réglages (clé isolée) ;
 *   5. sc2m_env_valeur : dernière occurrence, lignes « # » ignorées, CR toléré ;
 *   6. les sauvegardes du dossier Saves (miroir de Progression.sauvegardes) sur un arbre temporaire : ordre, casse,
 *      profondeur, taille minimale, plafond max — et le jeu de clés du chargement (contenuSauvegarde) ;
 *   7. argv[1] absent → « --cartes » : les 33 noms, UN PAR LIGNE, dans l'ordre (sortie pour le diff du banc).
 * Sortie : une ligne PASS/FAIL par contrôle, « RÉSULTAT : N PASS, M FAIL », code 0 sans échec.
 */
#include "sc2_catalogue_mac.h"

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>     /* AT_FDCWD */
#include <string.h>
#include <strings.h>   /* strcasecmp (ordre alphabétique sans casse) */
#include <sys/stat.h>  /* mkdir, utimensat */
#include <unistd.h>    /* symlink, rmdir */

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("PASS  "); } else { g_fail++; printf("FAIL  "); } \
                              printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

#define MAX_ELEMS 256

/* Extrait d'un fichier Java le contenu d'un tableau de chaînes « { "a", "b", … } » (paires imbriquées comprises :
 * RACES = { { "terran", "Terran" }, … } rend 8 chaînes à plat). Jusqu'à MAX_ELEMS éléments, dans l'ordre du fichier.
 * Rend le nombre d'éléments, -1 si l'affectation n'est pas trouvée. */
static int java_chaines(const char *chemin, const char *nom, char (*sortie)[64], int max)
{
    FILE *f = fopen(chemin, "r");
    if (!f) return -1;
    char motif[128];
    snprintf(motif, sizeof motif, "%s =", nom);
    char ligne[2048];
    int n = 0, dans = 0, fini = 0, profondeur = 0;
    while (!fini && fgets(ligne, sizeof ligne, f)) {
        char *p = ligne;
        if (!dans) {
            p = strstr(ligne, motif);
            if (!p) continue;
            p += strlen(motif);
            dans = 1;
            profondeur = 0;          /* la première accolade ouvrante du tableau (même ligne ou la suivante) */
        }
        for (; *p && n < max; p++) {
            if (*p == '{') { profondeur++; continue; }
            if (*p == '}') {
                profondeur--;
                if (profondeur == 0) { fini = 1; break; }
                continue;
            }
            if (*p != '"') continue;
            char *d = p + 1, *q = d;
            int l = 0;
            while (*q && *q != '"' && l < 63) sortie[n][l++] = *q++;
            sortie[n][l] = 0;
            n++;
            p = q;                    /* le « for » repart du guillemet fermant */
        }
    }
    fclose(f);
    return dans ? n : -1;
}

/* Même extraction pour un tableau d'INT « { 2, 2, … } ». */
static int java_entiers(const char *chemin, const char *nom, int *sortie, int max)
{
    FILE *f = fopen(chemin, "r");
    if (!f) return -1;
    char motif[128];
    snprintf(motif, sizeof motif, "%s =", nom);
    char ligne[2048];
    int n = 0, dans = 0, fini = 0;
    while (!fini && fgets(ligne, sizeof ligne, f)) {
        char *p = ligne;
        if (!dans) {
            p = strstr(ligne, motif);
            if (!p) continue;
            p += strlen(motif);
            dans = 1;
        }
        for (; *p && n < max; p++) {
            if (*p == '}') { fini = 1; break; }
            if (*p == '-' || (*p >= '0' && *p <= '9')) {
                sortie[n++] = (int)strtol(p, &p, 10);
                p--;                  /* recule avant le séparateur que le « for » sautera */
            }
        }
    }
    fclose(f);
    return dans ? n : -1;
}

static int meme_tableau_chaines(const char *titre, const char *const *mac, int n_mac,
                                char (*java)[64], int n_java)
{
    int ok = n_mac == n_java;
    int premier = -1;
    if (ok)
        for (int i = 0; i < n_mac; i++)
            if (strcmp(mac[i], java[i])) { ok = 0; premier = i; break; }
    CHECK(ok, "%s : %d éléments identiques à Catalogue.java%s", titre, n_mac,
          ok ? "" : (premier >= 0 ? " (1re divergence : « " : " (taille "));
    if (!ok) {
        if (premier >= 0) printf("      mac[%d] = « %s », java = « %s »\n", premier, mac[premier], java[premier]);
        else printf("      mac : %d, java : %d\n", n_mac, n_java);
    }
    return ok;
}

/* Valeur d'une clé dans k (NULL si absente). */
static const char *kval(const sc2m_jeu_cles *k, const char *cle)
{
    for (int i = 0; i < k->n; i++)
        if (!strcmp(k->cles[i], cle)) return k->vals[i];
    return NULL;
}

/* ---- l'essai des sauvegardes (6) : arbre temporaire sous /tmp ----------------------------------------------------- */

/* Supprime un dossier et son contenu (liens symboliques non suivis : remove les détache). */
static void retirer_arbre(const char *dossier)
{
    DIR *d = opendir(dossier);
    if (d) {
        char p[1024];
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(p, sizeof p, "%s/%s", dossier, e->d_name);
            struct stat st;
            if (!lstat(p, &st) && S_ISDIR(st.st_mode)) retirer_arbre(p);
            else remove(p);
        }
        closedir(d);
    }
    rmdir(dossier);
}

/* Écrit un fichier d'octets octets de « x » et pose sa date de modification (à la seconde). */
static int ecrire_fichier(const char *chemin, long octets, long date)
{
    FILE *f = fopen(chemin, "w");
    if (!f) return -1;
    for (long i = 0; i < octets; i++) fputc('x', f);
    fclose(f);
    struct timespec t[2] = { { date, 0 }, { date, 0 } };
    return utimensat(AT_FDCWD, chemin, t, 0);
}

int main(int argc, char **argv)
{
    /* 6. Mode « --cartes » : la liste pour le diff du banc (critère 6), rien d'autre. */
    if (argc > 1 && !strcmp(argv[1], "--cartes")) {
        for (int i = 0; i < sc2m_n_cartes; i++) printf("%s\n", sc2m_cartes[i]);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--cartes-melee")) {
        for (int i = 0; i < sc2m_n_cartes_melee; i++) printf("%s %d joueurs\n", sc2m_cartes_melee[i], sc2m_joueurs_melee[i]);
        return 0;
    }

    CHECK(argc > 1, "chemin de Catalogue.java en argv[1] (passé par tests-hote/v3/catalogue.cmake)");
    if (argc <= 1) { printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail); return g_fail ? 1 : 0; }
    const char *java = argv[1];

    /* 1. Les 33 cartes de campagne, dans l'ordre du fichier Java (critère 6). */
    static char j_arch[MAX_ELEMS][64];
    int n_arch = java_chaines(java, "CARTES_ARCHIVE", j_arch, MAX_ELEMS);
    CHECK(n_arch == 33, "CARTES_ARCHIVE lue dans Catalogue.java : %d éléments (33 attendus)", n_arch);
    CHECK(sc2m_n_cartes == 33, "sc2m_n_cartes == 33 (critère 6) : %d", sc2m_n_cartes);
    meme_tableau_chaines("sc2m_cartes", sc2m_cartes, sc2m_n_cartes, j_arch, n_arch);

    /* Ordre alphabétique sans casse puis avec (ORDRE de Catalogue) : la copie doit y être déjà. */
    int trie = 1;
    for (int i = 1; i < sc2m_n_cartes; i++) {
        int c = strcasecmp(sc2m_cartes[i - 1], sc2m_cartes[i]);
        if (c > 0 || (c == 0 && strcmp(sc2m_cartes[i - 1], sc2m_cartes[i]) > 0)) { trie = 0; break; }
    }
    CHECK(trie, "sc2m_cartes en ordre alphabétique (ORDRE de Catalogue)");

    /* 2. Les 63 cartes mêlée et leurs joueurs. */
    static char j_melee[MAX_ELEMS][64];
    int n_melee = java_chaines(java, "CARTES_MELEE", j_melee, MAX_ELEMS);
    CHECK(n_melee == 63, "CARTES_MELEE lue : %d éléments (63 attendus)", n_melee);
    CHECK(sc2m_n_cartes_melee == 63, "sc2m_n_cartes_melee == 63 : %d", sc2m_n_cartes_melee);
    meme_tableau_chaines("sc2m_cartes_melee", sc2m_cartes_melee, sc2m_n_cartes_melee, j_melee, n_melee);

    int j_joueurs[MAX_ELEMS];
    int n_joueurs = java_entiers(java, "JOUEURS_MELEE", j_joueurs, MAX_ELEMS);
    int joueurs_ok = n_joueurs == 63 && n_joueurs == sc2m_n_cartes_melee;
    int compteur[9] = { 0 };
    if (joueurs_ok)
        for (int i = 0; i < 63; i++) {
            if (sc2m_joueurs_melee[i] != j_joueurs[i]) { joueurs_ok = 0; break; }
            compteur[sc2m_joueurs_melee[i]]++;
        }
    CHECK(joueurs_ok, "sc2m_joueurs_melee == JOUEURS_MELEE (15×2, 1×3, 25×4, 11×6, 11×8 : %d×2 %d×3 %d×4 %d×6 %d×8)",
          compteur[2], compteur[3], compteur[4], compteur[6], compteur[8]);

    /* 3. Races, difficultés, résolutions, défaut. */
    static char j_races[MAX_ELEMS][64];
    int n_races = java_chaines(java, "RACES", j_races, MAX_ELEMS);
    int races_ok = n_races == 2 * sc2m_n_races && sc2m_n_races == 4;
    if (races_ok)
        for (int i = 0; i < sc2m_n_races; i++) {
            if (strcmp(sc2m_races[i][0], j_races[2 * i]) || strcmp(sc2m_races[i][1], j_races[2 * i + 1]))
                { races_ok = 0; break; }
        }
    CHECK(races_ok, "sc2m_races == RACES (4 races, valeurs et libellés)");

    static char j_diff[MAX_ELEMS][64];
    int n_diff = java_chaines(java, "DIFFICULTES", j_diff, MAX_ELEMS);
    int diff_ok = n_diff == 2 * sc2m_n_difficultes && sc2m_n_difficultes == 6;
    if (diff_ok)
        for (int i = 0; i < sc2m_n_difficultes; i++) {
            if (strcmp(sc2m_difficultes[i][0], j_diff[2 * i]) || strcmp(sc2m_difficultes[i][1], j_diff[2 * i + 1]))
                { diff_ok = 0; break; }
        }
    CHECK(diff_ok, "sc2m_difficultes == DIFFICULTES (6 difficultés, valeurs et libellés)");

    CHECK(sc2m_melee_defaut && !strcmp(sc2m_melee_defaut, "SteppesofWar"),
          "sc2m_melee_defaut == MELEE_DEFAUT (« SteppesofWar »)");

    if (argc > 2) {
        static char j_res[MAX_ELEMS][64];
        int n_res = java_chaines(argv[2], "RESOLUTIONS", j_res, MAX_ELEMS);
        int res_ok = n_res == 2 * sc2m_n_resolutions && sc2m_n_resolutions == 5;
        if (res_ok)
            for (int i = 0; i < sc2m_n_resolutions; i++) {
                if (strcmp(sc2m_resolutions[i][0], j_res[2 * i]) || strcmp(sc2m_resolutions[i][1], j_res[2 * i + 1]))
                    { res_ok = 0; break; }
            }
        CHECK(res_ok, "sc2m_resolutions == Options.RESOLUTIONS (5 résolutions, valeurs et libellés)");
    } else {
        CHECK(0, "chemin d'Options.java en argv[2] (résolutions non comparées)");
    }

    /* 4. Chemins et jeux de clés. */
    char chemin[512];
    CHECK(sc2m_carte_chemin("TRaynor01", chemin, sizeof chemin)
              && !strcmp(chemin, "Maps/Campaign/TRaynor01.SC2Map"),
          "sc2m_carte_chemin : « Maps/Campaign/TRaynor01.SC2Map »");

    sc2m_jeu_cles k;
    CHECK(sc2m_jeu_cles_menu(&k) > 0 && kval(&k, "SC2_RUN_SANS_BNET") && !strcmp(kval(&k, "SC2_RUN_SANS_BNET"), "1")
              && kval(&k, "SC2_ARGS") == NULL && kval(&k, "SC2_ESCARMOUCHE") == NULL
              && kval(&k, "SC2_SAUVEGARDE") == NULL,
          "menu au bar : SC2_RUN_SANS_BNET=1, SC2_ARGS/SC2_ESCARMOUCHE*/SC2_SAUVEGARDE retirés");

    CHECK(sc2m_jeu_cles_carte(&k, "TRaynor01") > 0
              && kval(&k, "SC2_ARGS") && !strcmp(kval(&k, "SC2_ARGS"), "-run Maps/Campaign/TRaynor01.SC2Map")
              && kval(&k, "SC2_ESCARMOUCHE") == NULL && kval(&k, "SC2_ESCARMOUCHE_IA") == NULL
              && kval(&k, "SC2_SAUVEGARDE") == NULL,
          "carte TRaynor01 : SC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map (critère 7), autres modes retirés");
    CHECK(sc2m_jeu_cles_carte(&k, "inexistante") == -1, "carte hors catalogue refusée (-1)");

    CHECK(sc2m_jeu_cles_escarmouche(&k, "SteppesofWar", "terran", "protoss", "moyen") > 0
              && kval(&k, "SC2_ESCARMOUCHE") && !strcmp(kval(&k, "SC2_ESCARMOUCHE"), "SteppesofWar")
              && kval(&k, "SC2_ESCARMOUCHE_RACE") && !strcmp(kval(&k, "SC2_ESCARMOUCHE_RACE"), "terran")
              && kval(&k, "SC2_ESCARMOUCHE_RACE_IA") && !strcmp(kval(&k, "SC2_ESCARMOUCHE_RACE_IA"), "protoss")
              && kval(&k, "SC2_ESCARMOUCHE_IA") && !strcmp(kval(&k, "SC2_ESCARMOUCHE_IA"), "moyen")
              && kval(&k, "SC2_ARGS") == NULL,
          "escarmouche : 5 clés posées, PAS de SC2_ARGS (un -run l'emporterait)");
    CHECK(sc2m_jeu_cles_escarmouche(&k, "HorsCache", "terran", "zerg", "facile") == -1
              && sc2m_jeu_cles_escarmouche(&k, "SteppesofWar", "hydralisk", "zerg", "facile") == -1
              && sc2m_jeu_cles_escarmouche(&k, "SteppesofWar", "terran", "zerg", "impossible") == -1,
          "escarmouche : carte, race ou difficulté hors tableaux refusée (-1)");

    CHECK(sc2m_reglage_ecran(&k, "1920x1080") > 0 && kval(&k, "SC2_ECRAN")
              && !strcmp(kval(&k, "SC2_ECRAN"), "1920x1080") && k.n == 1,
          "réglage écran : SC2_ECRAN=1920x1080 seul");
    CHECK(sc2m_reglage_ecran(&k, "800x600") == -1, "résolution hors tableaux refusée (-1)");
    CHECK(sc2m_reglage_ombres(&k, 1) == 1 && k.n == 1 && !strcmp(k.cles[0], "SC2_OMBRES") && k.vals[0]
              && !strcmp(k.vals[0], "0"),
          "réglage ombres coupées : SC2_OMBRES=0");
    CHECK(sc2m_reglage_ombres(&k, 0) == 1 && k.n == 1 && !strcmp(k.cles[0], "SC2_OMBRES") && k.vals[0] == NULL,
          "réglage ombres selon le jeu : la clé est RETIRÉE (valeur NULL), pas laissée en place");
    CHECK(sc2m_reglage_particules(&k, 1) == 1 && k.n == 1 && !strcmp(k.cles[0], "SC2_INSTANCES") && k.vals[0]
              && !strcmp(k.vals[0], "0"),
          "réglage particules coupées : SC2_INSTANCES=0");
    CHECK(sc2m_reglage_particules(&k, 0) == 1 && k.n == 1 && !strcmp(k.cles[0], "SC2_INSTANCES") && k.vals[0] == NULL,
          "réglage particules activées : la clé est RETIRÉE (valeur NULL)");

    /* Guilmets de SC2_ARGS : aucun des 33 noms n'a d'espace, la règle (Catalogue.contenuCarte) reste vérifiée
     * pour le cas général : pas de guillemet sur un chemin sans espace. */
    CHECK(sc2m_jeu_cles_carte(&k, "TRaynor01") > 0 && !strchr(kval(&k, "SC2_ARGS"), '"'),
          "SC2_ARGS sans guillemets quand le chemin n'a pas d'espace");

    /* 5. sc2m_env_valeur sur un vrai fichier temporaire. */
    {
        char tmp[] = "/tmp/sc2m_test_catalogue.env";
        FILE *f = fopen(tmp, "w");
        if (f) {
            fputs("# commentaire\n"
                  "SC2_ECRAN=1280x720\n"
                  "SC2_OMBRES=0\n"
                  "SC2_ESCARMOUCHE=SteppesofWar\n"
                  "SC2_ESCARMOUCHE=LostTemple\n"
                  "\n"
                  "sansegale\n"
                  "SC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map\r\n", f);
            fclose(f);
        }
        char v[256];
        CHECK(f && sc2m_env_valeur(tmp, "SC2_ESCARMOUCHE", v, sizeof v) && !strcmp(v, "LostTemple"),
              "sc2m_env_valeur : DERNIÈRE occurrence retenue");
        CHECK(sc2m_env_valeur(tmp, "SC2_ARGS", v, sizeof v)
                  && !strcmp(v, "-run Maps/Campaign/TRaynor01.SC2Map"),
              "sc2m_env_valeur : CR final retiré, « = » et espaces de la valeur gardés");
        CHECK(sc2m_env_valeur(tmp, "SC2_ECRAN", v, sizeof v) && !strcmp(v, "1280x720"), "sc2m_env_valeur : clé simple");
        CHECK(sc2m_env_valeur(tmp, "absente", v, sizeof v) == NULL, "sc2m_env_valeur : clé absente → NULL");
        CHECK(sc2m_env_valeur("/tmp/sc2m_test_catalogue.inexistant", "x", v, sizeof v) == NULL,
              "sc2m_env_valeur : fichier absent → NULL");
        remove(tmp);
    }

    /* 6. Sauvegardes : miroir de Progression.sauvegardes sur un arbre temporaire sous /tmp. */
    {
        const char *racine = "/tmp/sc2m_test_sauvegardes";
        char p[1024];
        retirer_arbre(racine);                        /* s'il reste un résidu d'un essai précédent */
        mkdir(racine, 0755);
        snprintf(p, sizeof p, "%s/Campaign", racine);
        mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/Campaign/The Outlaws.SC2Save", racine);   /* valide, plus récent */
        int e1 = ecrire_fichier(p, 995766, 1770000400);
        snprintf(p, sizeof p, "%s/LibertyCampaignSave.SC2Save", racine);    /* valide, plus ancien, racine */
        int e2 = ecrire_fichier(p, 995766, 1770000100);
        snprintf(p, sizeof p, "%s/Vide.sc2save", racine);                    /* .SC2Save sans casse, trop petit */
        int e3 = ecrire_fichier(p, 1181, 1770000300);
        snprintf(p, sizeof p, "%s/Unsaved", racine);                         /* profondeur 2 */
        mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/Unsaved/Liberation Day Victory.SC2Save", racine);
        int e4 = ecrire_fichier(p, 995766, 1770000200);
        {                                                                     /* profondeur 5 : exclu */
            char sous[1100];
            const char *morceaux[] = { "/a", "/a/b", "/a/b/c", "/a/b/c/d" };
            for (int i = 0; i < 4; i++) {
                snprintf(sous, sizeof sous, "%s%s", racine, morceaux[i]);
                mkdir(sous, 0755);
            }
            snprintf(sous, sizeof sous, "%s/a/b/c/d/Trop Profond.SC2Save", racine);
            ecrire_fichier(sous, 995766, 1770000500);   /* plus récent que tout : serait premier s'il était lu */
        }
        snprintf(p, sizeof p, "%s/Lien.SC2Save", racine);                    /* lien symbolique : jamais suivi */
        int e5 = symlink("/etc/hosts", p);
        (void)e1; (void)e2; (void)e3; (void)e4; (void)e5;

        sc2m_sauvegarde s[10];
        int n = sc2m_sauvegardes(racine, s, 10);
        CHECK(n == 4, "sauvegardes : 4 trouvées, Vide.sc2save compris (lien symbolique et profondeur 5 exclus) : %d",
              n);
        int ordre_ok = n == 4 && !strcmp(s[0].relatif, "Campaign/The Outlaws.SC2Save")
                       && !strcmp(s[1].relatif, "Vide.sc2save")
                       && !strcmp(s[2].relatif, "Unsaved/Liberation Day Victory.SC2Save")
                       && !strcmp(s[3].relatif, "LibertyCampaignSave.SC2Save");
        CHECK(ordre_ok, "sauvegardes : ordre date décroissante puis relatif croissant, extension sans casse");
        sc2m_sauvegarde vide = { .relatif = "Vide.sc2save", .taille = 1181 };
        CHECK(n == 4 && sc2m_sauvegarde_utilisable(&s[0]) && !sc2m_sauvegarde_utilisable(&vide)
                   && !sc2m_sauvegarde_utilisable(&s[1]) && s[0].taille == 995766,
              "sauvegardes : la liste garde tout, utilisable = taille >= 4096 masque Vide.sc2save (1 181 o)");
        CHECK(sc2m_sauvegardes(racine, s, 2) == 2 && !strcmp(s[0].relatif, "Campaign/The Outlaws.SC2Save")
                   && !strcmp(s[1].relatif, "Vide.sc2save"),
              "sauvegardes : max=2 garde le top-2 exact de la liste complète (Trop Profond serait premier s'il était lu)");
        CHECK(sc2m_sauvegardes("/tmp/sc2m_test_sauvegardes.inexistant", s, 10) == 0,
              "sauvegardes : dossier absent → 0");
        CHECK(sc2m_sauvegardes(NULL, s, 10) == -1 && sc2m_sauvegardes(racine, s, 0) == -1
                   && sc2m_sauvegardes(racine, NULL, 10) == -1,
              "sauvegardes : NULL ou max<=0 → -1");
        retirer_arbre(racine);
    }

    /* 7. Jeu de clés du chargement. */
    CHECK(sc2m_jeu_cles_sauvegarde(&k, "Saves/Campaign/The Outlaws.SC2Save") > 0
              && kval(&k, "SC2_RUN_SANS_BNET") && !strcmp(kval(&k, "SC2_RUN_SANS_BNET"), "1")
              && kval(&k, "SC2_SAUVEGARDE")
              && !strcmp(kval(&k, "SC2_SAUVEGARDE"), "Saves/Campaign/The Outlaws.SC2Save")
              && kval(&k, "SC2_ARGS") == NULL && kval(&k, "SC2_ESCARMOUCHE") == NULL
              && kval(&k, "SC2_ESCARMOUCHE_IA") == NULL && k.n == 7,
          "sauvegarde : SC2_RUN_SANS_BNET=1 + SC2_SAUVEGARDE=…, autres modes retirés (7 clés)");
    CHECK(sc2m_jeu_cles_sauvegarde(&k, "Saves\\Campaign\\The Outlaws.SC2Save") > 0
              && !strcmp(kval(&k, "SC2_SAUVEGARDE"), "Saves/Campaign/The Outlaws.SC2Save"),
          "sauvegarde : « \\ » normalisés en « / » (contenuSauvegarde)");
    CHECK(sc2m_jeu_cles_sauvegarde(&k, "") == -1 && sc2m_jeu_cles_sauvegarde(&k, NULL) == -1,
          "sauvegarde : chemin vide ou NULL refusé (-1)");
    CHECK(sc2m_jeu_cles_sauvegarde(&k, "Saves/x") > 0 && k.n == 7,
          "sauvegarde : le suffixe .SC2Save n'est pas exigé (c'est la liste qui garantit), 7 clés");

    printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
