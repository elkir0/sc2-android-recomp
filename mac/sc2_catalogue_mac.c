/* mac/sc2_catalogue_mac.c — catalogue du lanceur macOS, en C pur (V3, lot M9, 24/10/2026). En-tête : sc2_catalogue_mac.h. */
#include "sc2_catalogue_mac.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <sys/stat.h>  /* lstat */

/* Les 33 cartes Maps/Campaign/⋆.SC2Map de Base.SC2Maps (listfile de l'archive), ordre alphabétique —
 * Catalogue.java CARTES_ARCHIVE, l. 45-51, recopié tel quel. */
const char *const sc2m_cartes[] = {
    "TArcade", "THanson01", "THanson02", "THanson03A", "THanson03B", "THorner01", "THorner02", "THorner03",
    "THorner04", "THorner05S", "TRaynor01", "TRaynor02", "TRaynor03", "TSimulator", "TStory01", "TTosh01",
    "TTosh02", "TTosh03A", "TTosh03B", "TTutorial01", "TTychus01", "TTychus02", "TTychus03", "TTychus04",
    "TTychus05", "TValerian01", "TValerian02A", "TValerian02B", "TValerian03", "TZeratul01", "TZeratul02",
    "TZeratul03", "TZeratul04",
};
const int sc2m_n_cartes = (int)(sizeof sc2m_cartes / sizeof *sc2m_cartes);

/* V4.10 : les 63 cartes mêlée du cache Battle.net (Base.SC2Data), dans l'ordre de « SC2_ESCARMOUCHE=liste » (le
 * n° accepté par SC2_ESCARMOUCHE est l'indice + 1) — Catalogue.java CARTES_MELEE, l. 58-71. */
const char *const sc2m_cartes_melee[] = {
    "AgriaValley", "BlisteringSands", "BurialGrounds", "Crossfire", "DesertOasis", "IncinerationZone", "JungleBasin",
    "JunkYard", "NoviceBlisteringSands", "NoviceDesertOasis", "NoviceSteppesofWar", "ScrapStation", "SteppesofWar",
    "WorldShip", "XelNagaCaverns",
    "Elysium",
    "AridWastes", "DebrisField", "DeltaQuadrant", "DiscordIV", "HighOrbit", "KulasRavine", "LostTemple", "Metalopolis",
    "MonlythRidge", "NewAntioch", "Nightmare", "NoviceDiscordIV", "NoviceKulasRavine", "NoviceMetalopolis",
    "NoviceMonlythRidge", "NoviceTerminus", "NoviceTwilightFortress", "RedStoneGulch", "SacredGround", "ScorchedHaven",
    "ShakurasPlateau", "TarsonisAssault", "Terminus", "TwilightFortress", "WarZone",
    "ArakanCitadel", "Colony426", "DigSite", "DirtSide", "Frontier", "Monsoon", "Quicksand", "TectonicRift", "TheBioLab",
    "Typhon", "UlaanDeeps",
    "Abyss", "Extinction", "ForbiddenPlanet", "HighGround", "LavaFlow", "Megaton", "Outpost", "Primeval", "SandCanyon",
    "Tempest", "ToxicSlums",
};

/* Joueurs de chaque carte mêlée : 15 à 2, 1 à 3, 25 à 4, 11 à 6, 11 à 8 (Catalogue.java JOUEURS_MELEE, l. 73-79). */
const int sc2m_joueurs_melee[] = {
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
};
const int sc2m_n_cartes_melee = (int)(sizeof sc2m_cartes_melee / sizeof *sc2m_cartes_melee);

/* Races / difficultés de l'IA (Catalogue.java RACES l. 83-85, DIFFICULTES l. 87-90) et résolutions
 * (Options.java RESOLUTIONS l. 23-29) — { valeur de la variable, libellé }. */
const char *const sc2m_races[][2] = {
    { "terran", "Terran" }, { "zerg", "Zerg" }, { "protoss", "Protoss" }, { "aleatoire", "Aléatoire" },
};
const int sc2m_n_races = (int)(sizeof sc2m_races / sizeof *sc2m_races);

const char *const sc2m_difficultes[][2] = {
    { "tres-facile", "Très facile" }, { "facile", "Facile" }, { "moyen", "Moyen" }, { "difficile", "Difficile" },
    { "tres-difficile", "Très difficile" }, { "dement", "Démentiel" },
};
const int sc2m_n_difficultes = (int)(sizeof sc2m_difficultes / sizeof *sc2m_difficultes);

const char *const sc2m_resolutions[][2] = {
    { "auto", "Plein écran (auto)" }, { "1280x720", "1280×720" }, { "1280x1024", "1280×1024" },
    { "1600x1200", "1600×1200" }, { "1920x1080", "1920×1080" },
};
const int sc2m_n_resolutions = (int)(sizeof sc2m_resolutions / sizeof *sc2m_resolutions);

/* Carte proposée par défaut (n° 13, celle des essais du diagnostic V4.10). */
const char *const sc2m_melee_defaut = "SteppesofWar";

const char *sc2m_carte_chemin(const char *nom, char *o, size_t cap)
{
    if (!nom || !o || cap == 0) return NULL;
    if ((size_t)snprintf(o, cap, "%s/%s%s", SC2M_DOSSIER_CARTES, nom, SC2M_EXTENSION) >= cap) return NULL;
    return o;
}

const char *sc2m_env_valeur(const char *fichier, const char *cle, char *o, size_t cap)
{
    if (!fichier || !cle || !o || cap == 0) return NULL;
    o[0] = 0;
    int trouve = 0;
    FILE *f = fopen(fichier, "r");
    if (!f) return NULL;
    char ligne[1024];
    while (fgets(ligne, sizeof ligne, f)) {
        if (ligne[0] == '#') continue;
        char *eg = strchr(ligne, '=');
        if (!eg || eg == ligne) continue;
        *eg = 0;
        if (strcmp(ligne, cle)) continue;
        char *v = eg + 1;
        size_t n = strlen(v);
        while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;   /* CR toléré, comme valeurEnv */
        snprintf(o, cap, "%s", v);
        trouve = 1;                    /* la DERNIÈRE occurrence l'emporte (lire_env applique dans l'ordre) */
    }
    fclose(f);
    return trouve ? o : NULL;
}

/* ---- sauvegardes du dossier Saves (Progression.java, V5) --------------------------------------------------------- */

/* estSauvegarde : nom plus long que 8 caractères finissant par « .SC2Save » sans casse. */
static int est_sauvegarde(const char *nom)
{
    size_t n = strlen(nom);
    return n > 8 && !strcasecmp(nom + n - 8, ".SC2Save");
}

/* Progression.ORDRE : date décroissante, puis relatif croissant ; rend < 0 si a passe devant b. */
static int avant(const sc2m_sauvegarde *a, const sc2m_sauvegarde *b)
{
    if (a->date != b->date) return a->date > b->date ? -1 : 1;
    return strcmp(a->relatif, b->relatif);
}

/* Insère s à sa place dans la liste triée (au plus max) ; liste pleine : le nouveau doit battre le plus ancien
 * gardé, sinon il est ignoré — le rendu reste exactement le top-max de la liste complète. */
static void inserer(sc2m_sauvegarde *l, int *n, int max, const sc2m_sauvegarde *s)
{
    if (*n >= max) {
        if (!avant(s, &l[*n - 1])) return;
        (*n)--;                        /* le plus ancien tombe */
    }
    int i = (*n)++;
    while (i > 0 && avant(s, &l[i - 1]) < 0) { l[i] = l[i - 1]; i--; }   /* < 0 : s passe DEVANT l[i-1] */
    l[i] = *s;
}

/* parcourir : récursif, au plus SC2M_SAUVEGARDES_PROFONDEUR_MAX niveaux sous saves_dir ; liens symboliques jamais
 * suivis, dossiers illisibles ignorés. Un relatif qui ne tient pas dans sc2m_sauvegarde.relatif est écarté (le
 * nom serait tronqué). */
static void parcourir(const char *dossier, const char *relatif, int profondeur, sc2m_sauvegarde *l, int *n, int max)
{
    if (profondeur > SC2M_SAUVEGARDES_PROFONDEUR_MAX) return;
    DIR *d = opendir(dossier);
    if (!d) return;                    /* dossier illisible ou absent : ignoré */
    char chemin[1024], rel[512];
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(chemin, sizeof chemin, "%s/%s", dossier, e->d_name) >= (int)sizeof chemin) continue;
        struct stat st;
        if (lstat(chemin, &st)) continue;                 /* erreur : l'entrée est ignorée */
        if (S_ISLNK(st.st_mode)) continue;                /* jamais suivi */
        int r = *relatif ? snprintf(rel, sizeof rel, "%s/%s", relatif, e->d_name)
                         : snprintf(rel, sizeof rel, "%s", e->d_name);
        if (r < 0 || r >= (int)sizeof rel || r >= (int)sizeof l->relatif) continue;
        if (S_ISDIR(st.st_mode)) parcourir(chemin, rel, profondeur + 1, l, n, max);
        else if (S_ISREG(st.st_mode) && est_sauvegarde(e->d_name)) {
            sc2m_sauvegarde s;
            snprintf(s.relatif, sizeof s.relatif, "%s", rel);
            s.date = (long long)st.st_mtime;
            s.taille = (long long)st.st_size;
            inserer(l, n, max, &s);
        }
    }
    closedir(d);
}

int sc2m_sauvegardes(const char *saves_dir, sc2m_sauvegarde *s, int max)
{
    if (!saves_dir || !s || max <= 0) return -1;
    int n = 0;
    parcourir(saves_dir, "", 1, s, &n, max);
    return n;
}

int sc2m_sauvegarde_utilisable(const sc2m_sauvegarde *s)
{
    return s != NULL && s->taille >= SC2M_SAUVEGARDE_TAILLE_MIN;
}

/* Retire les clés de lancement qui ne sont pas celles du choix : Android réécrit sc2.env entier, la fusion Mac doit
 * donc effacer explicitement le mode précédent. */
static int cles_ajouter(sc2m_jeu_cles *k, const char *cle, const char *val)
{
    if (k->n >= SC2M_LANCEUR_CLES_MAX) return -1;
    k->cles[k->n] = cle;
    k->vals[k->n] = val;
    return k->n++;
}

static void cles_retirer_mode(sc2m_jeu_cles *k, int garde_args, int garde_escarmouche, int garde_sauvegarde)
{
    if (!garde_args) cles_ajouter(k, "SC2_ARGS", NULL);
    if (!garde_escarmouche) {
        cles_ajouter(k, "SC2_ESCARMOUCHE", NULL);
        cles_ajouter(k, "SC2_ESCARMOUCHE_RACE", NULL);
        cles_ajouter(k, "SC2_ESCARMOUCHE_RACE_IA", NULL);
        cles_ajouter(k, "SC2_ESCARMOUCHE_IA", NULL);
    }
    if (!garde_sauvegarde) cles_ajouter(k, "SC2_SAUVEGARDE", NULL);
}

int sc2m_jeu_cles_menu(sc2m_jeu_cles *k)
{
    k->n = 0;
    k->args[0] = 0;
    cles_ajouter(k, "SC2_RUN_SANS_BNET", "1");
    cles_retirer_mode(k, 0, 0, 0);
    return k->n;
}

int sc2m_jeu_cles_carte(sc2m_jeu_cles *k, const char *nom)
{
    int trouve = -1;
    for (int i = 0; i < sc2m_n_cartes; i++)
        if (nom && !strcmp(sc2m_cartes[i], nom)) { trouve = i; break; }
    if (trouve < 0) return -1;
    if (!sc2m_carte_chemin(nom, k->args, sizeof k->args)) return -1;
    k->n = 0;
    cles_ajouter(k, "SC2_RUN_SANS_BNET", "1");
    /* « -run "chemin avec espace" » : args_decouper du runtime comprend les guillemets (Catalogue.contenuCarte). */
    char run[600];
    if (strchr(k->args, ' ')) snprintf(run, sizeof run, "-run \"%s\"", k->args);
    else                      snprintf(run, sizeof run, "-run %s", k->args);
    snprintf(k->args, sizeof k->args, "%s", run);
    cles_ajouter(k, "SC2_ARGS", k->args);
    cles_retirer_mode(k, 1, 0, 0);
    return k->n;
}

int sc2m_jeu_cles_escarmouche(sc2m_jeu_cles *k, const char *carte, const char *race, const char *race_ia,
                              const char *difficulte)
{
    int ic = -1, ir = -1, iri = -1, id = -1;
    for (int i = 0; i < sc2m_n_cartes_melee; i++)
        if (carte && !strcmp(sc2m_cartes_melee[i], carte)) { ic = i; break; }
    for (int i = 0; i < sc2m_n_races; i++)
        if (race && !strcmp(sc2m_races[i][0], race)) { ir = i; break; }
    for (int i = 0; i < sc2m_n_races; i++)
        if (race_ia && !strcmp(sc2m_races[i][0], race_ia)) { iri = i; break; }
    for (int i = 0; i < sc2m_n_difficultes; i++)
        if (difficulte && !strcmp(sc2m_difficultes[i][0], difficulte)) { id = i; break; }
    if (ic < 0 || ir < 0 || iri < 0 || id < 0) return -1;
    k->n = 0;
    k->args[0] = 0;
    cles_ajouter(k, "SC2_RUN_SANS_BNET", "1");
    cles_ajouter(k, "SC2_ESCARMOUCHE", carte);
    cles_ajouter(k, "SC2_ESCARMOUCHE_RACE", race);
    cles_ajouter(k, "SC2_ESCARMOUCHE_RACE_IA", race_ia);
    cles_ajouter(k, "SC2_ESCARMOUCHE_IA", difficulte);
    cles_retirer_mode(k, 0, 1, 0);       /* PAS de SC2_ARGS : un « -run » l'emporterait sur la carte d'escarmouche */
    return k->n;
}

int sc2m_jeu_cles_sauvegarde(sc2m_jeu_cles *k, const char *chemin_relatif)
{
    /* SC2_ARGS est retiré dans ce mode : k->args sert de tampon à la valeur SC2_SAUVEGARDE (Catalogue
     * .contenuSauvegarde remplace les « \ » par des « / » — le runtime comprend les deux). */
    if (!chemin_relatif || !*chemin_relatif) return -1;
    size_t n = strlen(chemin_relatif);
    if (n >= sizeof k->args) return -1;
    for (size_t i = 0; i <= n; i++)
        k->args[i] = chemin_relatif[i] == '\\' ? '/' : chemin_relatif[i];
    k->n = 0;
    cles_ajouter(k, "SC2_RUN_SANS_BNET", "1");
    cles_ajouter(k, "SC2_SAUVEGARDE", k->args);
    cles_retirer_mode(k, 0, 0, 1);       /* la sauvegarde est LE mode de lancement : SC2_ARGS et escarmouche out */
    return k->n;
}

int sc2m_reglage_ecran(sc2m_jeu_cles *k, const char *resolution)
{
    int i = -1;
    for (int j = 0; j < sc2m_n_resolutions; j++)
        if (resolution && !strcmp(sc2m_resolutions[j][0], resolution)) { i = j; break; }
    if (i < 0) return -1;
    k->n = 0;
    k->args[0] = 0;
    cles_ajouter(k, "SC2_ECRAN", sc2m_resolutions[i][0]);
    return k->n;
}

int sc2m_reglage_ombres(sc2m_jeu_cles *k, int coupees)
{
    k->n = 0;
    k->args[0] = 0;
    /* la clé est TOUJOURS dans le jeu : « 0 » si coupées, NULL (retrait) si selon le réglage du jeu —
     * une fusion qui ne dit rien laisse « SC2_OMBRES=0 » d'un réglage précédent en place. */
    cles_ajouter(k, "SC2_OMBRES", coupees ? "0" : NULL);
    return k->n;
}

int sc2m_reglage_particules(sc2m_jeu_cles *k, int coupees)
{
    k->n = 0;
    k->args[0] = 0;
    cles_ajouter(k, "SC2_INSTANCES", coupees ? "0" : NULL);   /* NULL (retrait) = activées */
    return k->n;
}
