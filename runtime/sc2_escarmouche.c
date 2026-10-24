/* sc2_escarmouche.c — V4.10 (23/10) : escarmouche contre l'IA, hors ligne (« carte d'escarmouche dérivée »).
 *
 * Interface et principe : sc2_escarmouche.h. Les étapes, à chaque lancement avec SC2_ESCARMOUCHE=<carte> :
 *   1. le cache Battle.net pré-rempli de l'installation ($SC2_INSTALL/Mods/Liberty.SC2Mod/Base.SC2Data, casse résolue
 *      comme sc2_iat.c) est ouvert en lecture par StormLib : ses entrées BattlenetCache\(N)<Nom>.SC2Map\<sha>.s2ma
 *      sont les cartes mêlée (63 sur le DVD WoL) ; elles sont triées par (N, nom) — l'ordre de « liste » et des n° ;
 *   2. la carte choisie (nom sans casse, espaces et ponctuation ignorés, « (N) » et « .SC2Map » tolérés ; à défaut
 *      un préfixe ou un morceau UNIQUE ; ou son n°) est extraite vers un temporaire puis renommée en
 *      $SC2_ROOT/Users/sc2/Documents/StarCraft II/Maps/Escarmouche/<Nom>.SC2Map — copie neuve à chaque lancement ;
 *   3. la copie est rouverte en écriture : MapInfo (version 0x18 stricte, sinon refus) — joueur 1 « User » humain
 *      (race, bleu), les emplacements « User » suivants deviennent « Computer » (race, couleur), les autres sont
 *      laissés tels quels (vides en mode 2) ; MapScript.galaxy — PlayerSetDifficulty(<joueur>, <c_skir*>) inséré
 *      avant l'UNIQUE « MeleeInitResources(); » (refus s'il n'y en a pas exactement un), avec l'indentation et les
 *      fins de ligne (CRLF) de la ligne visée ; puis SFileCompactArchive (le chemin prouvé au diagnostic) ;
 *   4. relecture de contrôle (MapInfo reparsé, joueurs attendus, insertion présente), journal « [ESCARMOUCHE] »,
 *      et le chemin Windows de la copie est rendu au lanceur simulé (sc2_escarmouche_carte).
 * Échec à n'importe quelle étape : « [ESCARMOUCHE] ÉCHEC … » et le jeu démarre normalement (pas d'escarmouche, pas
 * d'arrêt). Les cartes et scripts produits restent dans le C:\ émulé : jamais dans l'installation ni dans le dépôt.
 *
 * Énumération des difficultés (Core.SC2Mod, TriggerLibs/AI.galaxy) : c_skirVeryEasy 0, c_skirEasy 1, c_skirMedium 2,
 * c_skirHard 3, c_skirVeryHard 4, c_skirCheater 5 (attribut 3004 des salons : VyEy Easy Medi Hard VyHd Insa).
 * Races du MapInfo : « Terr », « Zerg », « Prot » (race vide = Protoss en mode 2 : l'aléatoire est tiré ici).
 *
 * Options de banc : SC2_ESCARMOUCHE_VISION=1 coupe masque et brouillard (VisEnable, captures de la base IA) ;
 * SC2_ESCARMOUCHE_FIN=1 écrit des banques (BankSave) au début de la partie (EscarmoucheDebut, preuve que le script
 * modifié a compilé et tourné) et à la fin du joueur 1 (EscarmoucheFinVictoire / EscarmoucheFinDefaite, sur
 * TriggerAddEventPlayerLeft) : la mêlée n'écrit pas de …Victory.SC2Save, le banc d'entrées attend donc
 * « QUAND:FICHIER:*EscarmoucheFin*.SC2Bank ECRITURE » (scripts/entrees/escarmouche-longue.txt). NON OPÉRANT au banc du
 * 23/10 : le script augmenté compile et la partie se déroule (IA, défaite à 10 min), mais aucune .SC2Bank n'est écrite,
 * avec ou sans BankList.xml — à instruire (SC2_TRACE : CreateFileW des banques).
 *
 * SC2_ESCARMOUCHE_SANS_MPQ : seule la partie « pur calcul » (MapInfo, MapScript, noms, options) est compilée — le
 * test hôte (recomp-sc2/tests/test_escarmouche.c) l'inclut ainsi, sans StormLib.
 */
#define _GNU_SOURCE
#include "sc2_escarmouche.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define ESC_JOUEURS_MAX 16
#define ESC_IA_MAX 7
#define ESC_ERR 256

/* ======================================================================================================== */
/* Pur calcul : options, noms, MapInfo, MapScript (testé sans StormLib)                                     */
/* ======================================================================================================== */

static const char *const ESC_RACE_CODE[3] = { "Terr", "Zerg", "Prot" };
static const char *const ESC_RACE_NOM[3]  = { "Terran", "Zerg", "Protoss" };
#define ESC_ALEATOIRE 3
static const char *const ESC_DIFF_NOM[6]  = { "Très facile", "Facile", "Moyen", "Difficile", "Très difficile", "Démentiel" };
static const char *const ESC_DIFF_GALAXY[6] = { "c_skirVeryEasy", "c_skirEasy", "c_skirMedium", "c_skirHard",
                                                "c_skirVeryHard", "c_skirCheater" };
/* Couleurs du MapInfo (index de la palette des joueurs) : humain bleu (2), IA rouge, sarcelle, violet, jaune, orange,
 * vert, rose — les deux premières sont celles prouvées au diagnostic (m2-ia : J1 bleu, J2 rouge). */
#define ESC_COULEUR_HUMAIN 2
static const int ESC_COULEUR_IA[ESC_IA_MAX] = { 1, 3, 4, 5, 6, 7, 8 };

/* Minuscules ASCII, accents français courants ramenés à la lettre de base (UTF-8 : é è ê ë É È → e, à â → a,
 * î ï → i, ô → o, û ù → u, ç → c), et seuls [a-z0-9] gardés (espaces, tirets, points, parenthèses ôtés). */
static void esc_normaliser(const char *s, char *o, size_t cap)
{
    size_t k = 0;
    if (!cap) return;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && k + 1 < cap; p++) {
        int c = *p;
        if (c == 0xC3 && p[1]) {
            int d = p[1] | 0x20;                  /* É (0x89) → é (0xA9), etc. */
            p++;
            if (d >= 0xA8 && d <= 0xAB) c = 'e';
            else if (d >= 0xA0 && d <= 0xA5) c = 'a';
            else if (d >= 0xAC && d <= 0xAF) c = 'i';
            else if (d >= 0xB2 && d <= 0xB6) c = 'o';
            else if (d >= 0xB9 && d <= 0xBC) c = 'u';
            else if (d == 0xA7) c = 'c';
            else continue;
        }
        if (isalnum(c)) o[k++] = (char)tolower(c);
    }
    o[k] = 0;
}

/* Race : 0 Terran, 1 Zerg, 2 Protoss, 3 aléatoire ; -1 inconnue. Noms français et anglais, codes du jeu, initiales. */
static int esc_race(const char *s)
{
    char n[32];
    esc_normaliser(s, n, sizeof n);
    if (!strcmp(n, "terran") || !strcmp(n, "terr") || !strcmp(n, "t")) return 0;
    if (!strcmp(n, "zerg") || !strcmp(n, "z")) return 1;
    if (!strcmp(n, "protoss") || !strcmp(n, "prot") || !strcmp(n, "p")) return 2;
    if (!strcmp(n, "aleatoire") || !strcmp(n, "alea") || !strcmp(n, "random") || !strcmp(n, "rand")
        || !strcmp(n, "hasard") || !strcmp(n, "r")) return ESC_ALEATOIRE;
    return -1;
}

/* Difficulté de l'IA : valeur c_skir* (0 à 5) ; -1 inconnue. Chiffre, noms français (ceux du jeu en français),
 * noms anglais, codes de l'attribut 3004. */
static int esc_difficulte(const char *s)
{
    static const struct { const char *nom; int v; } T[] = {
        { "0", 0 }, { "tresfacile", 0 }, { "veryeasy", 0 }, { "vyey", 0 }, { "debutant", 0 },
        { "1", 1 }, { "facile", 1 }, { "easy", 1 },
        { "2", 2 }, { "moyen", 2 }, { "moyenne", 2 }, { "medium", 2 }, { "medi", 2 }, { "normal", 2 },
        { "3", 3 }, { "difficile", 3 }, { "hard", 3 },
        { "4", 4 }, { "tresdifficile", 4 }, { "veryhard", 4 }, { "vyhd", 4 },
        { "5", 5 }, { "dement", 5 }, { "dementiel", 5 }, { "insane", 5 }, { "insa", 5 }, { "cheater", 5 },
        { "triche", 5 },
    };
    char n[32];
    esc_normaliser(s, n, sizeof n);
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(n, T[i].nom)) return T[i].v;
    return -1;
}

/* ---- MapInfo (version 0x18, WoL 1.0.x) — portage de l'outil du diagnostic V4.10 (mapinfo.py) ----------------------
 * 'IpaM' u32 version, u32 ×2, u32 largeur, hauteur, u32 type petit aperçu, type grand aperçu (2 → chaîne chacun),
 * u32, chaîne brouillard, chaîne tuiles, u32 ×4 caméra, u32 hauteur de base, u32 type d'écran de chargement, chaîne
 * écran de chargement, u16, u32 ×8, u32 nombre de joueurs, puis par joueur : u8 id, u32 contrôle (1 User, 2 Computer,
 * 3 Neutral, 4 Hostile), i32 couleur (-1 = aucune), chaîne race, u32 ×3, chaîne décalcomanie ; puis la suite, recopiée
 * telle quelle. Seuls contrôle, couleur et race sont réécrits. */
typedef struct {
    uint8_t id; uint32_t control; int32_t couleur;
    char race[8];
    uint32_t a, depart, c;
    const uint8_t *decal; size_t ldecal;          /* chaîne d'origine (sans le NUL), dans le tampon lu */
} esc_joueur;
typedef struct {
    uint32_t largeur, hauteur;
    size_t debut, fin;                            /* [debut, fin) : nombre de joueurs + joueurs */
    int n;
    esc_joueur j[ESC_JOUEURS_MAX];
} esc_mapinfo;

typedef struct { const uint8_t *b; size_t n, o; int ok; } esc_curseur;
static uint32_t esc_u32(esc_curseur *c)
{
    if (!c->ok || c->o > c->n || c->n - c->o < 4) { c->ok = 0; return 0; }
    const uint8_t *p = c->b + c->o; c->o += 4;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void esc_sauter(esc_curseur *c, size_t k) { if (!c->ok || c->o > c->n || c->n - c->o < k) c->ok = 0; else c->o += k; }
static const uint8_t *esc_chaine(esc_curseur *c, size_t *l)
{
    const uint8_t *z = c->ok && c->o < c->n ? memchr(c->b + c->o, 0, c->n - c->o) : NULL;
    if (!z) { c->ok = 0; *l = 0; return c->b; }
    const uint8_t *d = c->b + c->o;
    *l = (size_t)(z - d); c->o += *l + 1;
    return d;
}

static int esc_mapinfo_lire(const uint8_t *b, size_t n, esc_mapinfo *mi, char *err, size_t cap)
{
    esc_curseur c = { b, n, 0, 1 };
    size_t l;
    memset(mi, 0, sizeof *mi);
    if (n < 8 || memcmp(b, "IpaM", 4)) { snprintf(err, cap, "MapInfo : signature « IpaM » absente"); return -1; }
    c.o = 4;
    uint32_t ver = esc_u32(&c);
    if (ver != 0x18) { snprintf(err, cap, "MapInfo : version 0x%X (seule 0x18, WoL 1.0.x, est connue)", ver); return -1; }
    esc_sauter(&c, 8);
    mi->largeur = esc_u32(&c); mi->hauteur = esc_u32(&c);
    uint32_t sp = esc_u32(&c), lp = esc_u32(&c);
    if (sp == 2) esc_chaine(&c, &l);
    if (lp == 2) esc_chaine(&c, &l);
    esc_sauter(&c, 4);
    esc_chaine(&c, &l); esc_chaine(&c, &l);       /* brouillard, tuiles */
    esc_sauter(&c, 16 + 4);                       /* caméra, hauteur de base */
    esc_u32(&c); esc_chaine(&c, &l);              /* écran de chargement : type, chemin */
    esc_sauter(&c, 2 + 4 * 8);
    mi->debut = c.o;
    uint32_t nj = esc_u32(&c);
    if (!c.ok) { snprintf(err, cap, "MapInfo tronqué avant la liste des joueurs (%zu o)", n); return -1; }
    if (nj > ESC_JOUEURS_MAX) { snprintf(err, cap, "MapInfo : %u joueurs (au plus %d attendus)", nj, ESC_JOUEURS_MAX); return -1; }
    for (uint32_t i = 0; i < nj; i++) {
        esc_joueur *j = &mi->j[i];
        if (c.o >= n) { c.ok = 0; break; }
        j->id = b[c.o++];
        j->control = esc_u32(&c); j->couleur = (int32_t)esc_u32(&c);
        const uint8_t *r = esc_chaine(&c, &l);
        if (c.ok && l >= sizeof j->race) { snprintf(err, cap, "MapInfo : race du joueur %u trop longue (%zu o)", j->id, l); return -1; }
        if (c.ok) { memcpy(j->race, r, l); j->race[l] = 0; }
        j->a = esc_u32(&c); j->depart = esc_u32(&c); j->c = esc_u32(&c);
        j->decal = esc_chaine(&c, &j->ldecal);
    }
    if (!c.ok) { snprintf(err, cap, "MapInfo tronqué dans la liste des joueurs (%zu o)", n); return -1; }
    mi->n = (int)nj; mi->fin = c.o;
    return 0;
}

static void esc_p32(uint8_t *o, size_t *k, uint32_t v)
{ o[(*k)++] = (uint8_t)v; o[(*k)++] = (uint8_t)(v >> 8); o[(*k)++] = (uint8_t)(v >> 16); o[(*k)++] = (uint8_t)(v >> 24); }

/* Taille du MapInfo réécrit (out NULL) ou écriture dans out (cap octets) ; 0 si cap ne suffit pas. */
static size_t esc_mapinfo_ecrire(const uint8_t *b, size_t n, const esc_mapinfo *mi, uint8_t *out, size_t cap)
{
    size_t t = mi->debut + 4 + (n - mi->fin);
    for (int i = 0; i < mi->n; i++) t += 1 + 8 + strlen(mi->j[i].race) + 1 + 12 + mi->j[i].ldecal + 1;
    if (!out) return t;
    if (cap < t) return 0;
    size_t k = 0;
    memcpy(out, b, mi->debut); k = mi->debut;
    esc_p32(out, &k, (uint32_t)mi->n);
    for (int i = 0; i < mi->n; i++) {
        const esc_joueur *j = &mi->j[i];
        out[k++] = j->id;
        esc_p32(out, &k, j->control); esc_p32(out, &k, (uint32_t)j->couleur);
        size_t lr = strlen(j->race); memcpy(out + k, j->race, lr + 1); k += lr + 1;
        esc_p32(out, &k, j->a); esc_p32(out, &k, j->depart); esc_p32(out, &k, j->c);
        memcpy(out + k, j->decal, j->ldecal); k += j->ldecal; out[k++] = 0;
    }
    memcpy(out + k, b + mi->fin, n - mi->fin); k += n - mi->fin;
    return k;
}

/* Places jouables : joueurs « User » (contrôle 1). */
static int esc_places(const esc_mapinfo *mi)
{
    int p = 0;
    for (int i = 0; i < mi->n; i++) p += mi->j[i].control == 1;
    return p;
}

/* Joueur 1 humain (race_j1, bleu) ; les nb_ia emplacements « User » suivants, par id croissant, deviennent
 * « Computer » (races_ia[k], ESC_COULEUR_IA[k]) ; ids_ia[k] reçoit leur id. 0 ou -1 (err). */
static int esc_mapinfo_escarmouche(esc_mapinfo *mi, int race_j1, const int *races_ia, int nb_ia, int *ids_ia,
                                   char *err, size_t cap)
{
    esc_joueur *j1 = NULL;
    for (int i = 0; i < mi->n; i++) if (mi->j[i].id == 1) j1 = &mi->j[i];
    if (!j1 || j1->control != 1) { snprintf(err, cap, "MapInfo : joueur 1 absent ou non « User »"); return -1; }
    if (nb_ia < 1 || nb_ia > ESC_IA_MAX) { snprintf(err, cap, "nombre d'IA %d hors de 1..%d", nb_ia, ESC_IA_MAX); return -1; }
    snprintf(j1->race, sizeof j1->race, "%s", ESC_RACE_CODE[race_j1]);
    j1->couleur = ESC_COULEUR_HUMAIN;
    int k = 0;
    for (int id = 2; id < 256 && k < nb_ia; id++)
        for (int i = 0; i < mi->n; i++)
            if (mi->j[i].id == id && mi->j[i].control == 1) {
                mi->j[i].control = 2;
                snprintf(mi->j[i].race, sizeof mi->j[i].race, "%s", ESC_RACE_CODE[races_ia[k]]);
                mi->j[i].couleur = ESC_COULEUR_IA[k];
                ids_ia[k++] = id;
                break;
            }
    if (k < nb_ia) { snprintf(err, cap, "%d IA demandée(s), %d place(s) libre(s) après le joueur 1", nb_ia, k); return -1; }
    return 0;
}

/* ---- MapScript.galaxy -------------------------------------------------------------------------------------------- */
static const char ESC_ANCRE[] = "MeleeInitResources();";

/* Lignes (sans indentation, séparées par \n) insérées avant « MeleeInitResources(); ». ASCII seulement. */
static void esc_galaxy_avant(char *o, size_t cap, const int *ids, int nb, int diff, int vision, int fin)
{
    size_t k = 0;
    k += (size_t)snprintf(o + k, cap - k, "// SC2 Partout V4.10 : escarmouche hors ligne, %d IA, difficulte %s\n", nb,
                          ESC_DIFF_GALAXY[diff]);
    for (int i = 0; i < nb && k < cap; i++)
        k += (size_t)snprintf(o + k, cap - k, "PlayerSetDifficulty(%d, %d);\n", ids[i], diff);
    if (vision && k < cap)
        k += (size_t)snprintf(o + k, cap - k, "VisEnable(c_visTypeMask, false);\nVisEnable(c_visTypeFog, false);\n");
    if (fin && k < cap)
        snprintf(o + k, cap - k,
                 "BankLoad(\"EscarmoucheDebut\", 1);\n"
                 "BankSave(BankLastCreated());\n"
                 "TriggerAddEventPlayerLeft(TriggerCreate(\"gt_EscFin_Victoire\"), 1, c_gameResultVictory);\n"
                 "TriggerAddEventPlayerLeft(TriggerCreate(\"gt_EscFin_Defaite\"), 1, c_gameResultDefeat);\n");
}

/* Fonctions de fin (SC2_ESCARMOUCHE_FIN), insérées avant l'en-tête de la fonction qui appelle MeleeInitResources. */
static const char ESC_GALAXY_FIN[] =
    "//--------------------------------------------------------------------------------------------------\n"
    "// SC2 Partout V4.10 (SC2_ESCARMOUCHE_FIN) : fin de partie du joueur 1 -> banque, pour le banc\n"
    "//--------------------------------------------------------------------------------------------------\n"
    "bool gt_EscFin_Victoire (bool testConds, bool runActions) {\n"
    "    BankLoad(\"EscarmoucheFinVictoire\", 1);\n"
    "    BankValueSetFromInt(BankLastCreated(), \"Partie\", \"Resultat\", 1);\n"
    "    BankSave(BankLastCreated());\n"
    "    return true;\n"
    "}\n"
    "\n"
    "bool gt_EscFin_Defaite (bool testConds, bool runActions) {\n"
    "    BankLoad(\"EscarmoucheFinDefaite\", 1);\n"
    "    BankValueSetFromInt(BankLastCreated(), \"Partie\", \"Resultat\", 2);\n"
    "    BankSave(BankLastCreated());\n"
    "    return true;\n"
    "}\n"
    "\n";

/* BankList.xml de la carte dérivée (SC2_ESCARMOUCHE_FIN) : banques préchargées pour le joueur 1. */
static const char ESC_BANKLIST[] =
    "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\n"
    "<BankList>\n"
    "    <Bank Name=\"EscarmoucheDebut\" Player=\"1\"/>\n"
    "    <Bank Name=\"EscarmoucheFinVictoire\" Player=\"1\"/>\n"
    "    <Bank Name=\"EscarmoucheFinDefaite\" Player=\"1\"/>\n"
    "</BankList>\n";

/* Ajoute t (lignes \n) à o, chaque ligne préfixée de ind[0..lind) et terminée par eol. */
static size_t esc_lignes(char *o, const char *t, const char *ind, size_t lind, const char *eol)
{
    size_t k = 0, le = strlen(eol);
    while (*t) {
        const char *f = strchr(t, '\n');
        size_t l = f ? (size_t)(f - t) : strlen(t);
        if (l) { if (o) memcpy(o + k, ind, lind); k += lind; }
        if (o) memcpy(o + k, t, l);
        k += l;
        if (o) memcpy(o + k, eol, le);
        k += le;
        t += l + (f ? 1 : 0);
    }
    return k;
}

/* Insère avant (lignes, indentées comme la ligne visée) avant la ligne de l'unique « MeleeInitResources(); » et defs
 * (texte brut, fins de ligne de la ligne visée) avant l'en-tête de la fonction qui la contient — dernière ligne
 * au-dessus qui commence en colonne 0 par une lettre ou « _ ». Rend un tampon alloué (NUL final, *nout sans lui) ou
 * NULL (err). */
static char *esc_mapscript_inserer(const char *src, size_t n, const char *avant, const char *defs, size_t *nout,
                                   char *err, size_t cap)
{
    const char *p = NULL, *q = src;
    int occ = 0;
    for (;;) {
        const char *r = memmem(q, n - (size_t)(q - src), ESC_ANCRE, sizeof ESC_ANCRE - 1);
        if (!r) break;
        occ++; p = r; q = r + 1;
    }
    if (occ != 1) { snprintf(err, cap, "MapScript.galaxy : %d « %s » (exactement 1 attendu)", occ, ESC_ANCRE); return NULL; }
    const char *ligne = p;                                       /* début de la ligne visée */
    while (ligne > src && ligne[-1] != '\n') ligne--;
    size_t lind = 0;
    while (ligne + lind < p && (ligne[lind] == ' ' || ligne[lind] == '\t')) lind++;
    if (ligne + lind != p) { snprintf(err, cap, "MapScript.galaxy : « %s » n'est pas en tête de sa ligne", ESC_ANCRE); return NULL; }
    const char *fl = memchr(p, '\n', n - (size_t)(p - src));
    const char *eol = fl && fl > p && fl[-1] == '\r' ? "\r\n" : "\n";
    const char *entete = NULL;                                   /* en-tête de la fonction (defs seulement) */
    if (defs && *defs) {
        for (const char *l = ligne; l > src; ) {
            const char *d = l - 1;                               /* d = '\n' de la ligne précédente */
            while (d > src && d[-1] != '\n') d--;
            if (isalpha((unsigned char)*d) || *d == '_') { entete = d; break; }
            l = d;
        }
        if (!entete) { snprintf(err, cap, "MapScript.galaxy : en-tête de la fonction de « %s » introuvable", ESC_ANCRE); return NULL; }
    }
    size_t ld = defs && *defs ? esc_lignes(NULL, defs, "", 0, eol) : 0;
    size_t la = esc_lignes(NULL, avant, ligne, lind, eol);
    char *o = malloc(n + ld + la + 1);
    if (!o) { snprintf(err, cap, "mémoire"); return NULL; }
    size_t k = 0;
    if (entete) {
        memcpy(o, src, (size_t)(entete - src)); k = (size_t)(entete - src);
        k += esc_lignes(o + k, defs, "", 0, eol);
        memcpy(o + k, entete, (size_t)(ligne - entete)); k += (size_t)(ligne - entete);
    } else {
        memcpy(o, src, (size_t)(ligne - src)); k = (size_t)(ligne - src);
    }
    k += esc_lignes(o + k, avant, ligne, lind, eol);
    memcpy(o + k, ligne, n - (size_t)(ligne - src)); k += n - (size_t)(ligne - src);
    o[k] = 0;
    *nout = k;
    return o;
}

/* ---- cartes du cache ------------------------------------------------------------------------------------------- */
typedef struct {
    char interne[300];                            /* BattlenetCache\(N)<Nom>.SC2Map\<sha>.s2ma */
    char nom[96];
    int joueurs;
    uint32_t taille;
} esc_carte;

static const char ESC_PREFIXE[] = "BattlenetCache\\";

/* Nom interne → carte : « BattlenetCache\(N)<Nom>.SC2Map\<…>.s2ma » (casse indifférente) ; 0 ou -1 (autre entrée). */
static int esc_carte_depuis_interne(const char *interne, esc_carte *c)
{
    size_t lp = sizeof ESC_PREFIXE - 1, li = strlen(interne);
    if (li >= sizeof c->interne || strncasecmp(interne, ESC_PREFIXE, lp) || li < 5 || strcasecmp(interne + li - 5, ".s2ma"))
        return -1;
    const char *s = interne + lp;
    if (*s != '(' || !isdigit((unsigned char)s[1])) return -1;
    char *f;
    long j = strtol(s + 1, &f, 10);
    if (*f != ')' || j < 1 || j > 16) return -1;
    const char *nom = f + 1, *e = strchr(nom, '\\');
    if (!e || e - nom <= 7 || strncasecmp(e - 7, ".SC2Map", 7) || (size_t)(e - 7 - nom) >= sizeof c->nom) return -1;
    memset(c, 0, sizeof *c);
    memcpy(c->interne, interne, li + 1);
    memcpy(c->nom, nom, (size_t)(e - 7 - nom));
    c->joueurs = (int)j;
    return 0;
}

static int esc_carte_ordre(const void *a, const void *b)
{
    const esc_carte *x = a, *y = b;
    if (x->joueurs != y->joueurs) return x->joueurs - y->joueurs;
    int r = strcasecmp(x->nom, y->nom);
    return r ? r : strcmp(x->interne, y->interne);
}

/* Carte demandée (nom ou n° de « liste ») → index ; -1 introuvable, -2 ambigu (candidats dans amb). */
static int esc_carte_choisir(const esc_carte *t, int n, const char *demande, char *amb, size_t cap)
{
    char d[128], c[128];
    const char *s = demande;
    if (amb && cap) amb[0] = 0;
    if (*s && strspn(s, "0123456789") == strlen(s)) {
        long i = strtol(s, NULL, 10);
        return i >= 1 && i <= n ? (int)i - 1 : -1;
    }
    if (*s == '(') { const char *f = strchr(s, ')'); if (f) s = f + 1; }      /* « (2)SteppesofWar » */
    esc_normaliser(s, d, sizeof d);
    size_t ld = strlen(d);
    if (ld > 6 && !strcmp(d + ld - 6, "sc2map")) d[ld -= 6] = 0;               /* « SteppesofWar.SC2Map » */
    if (!ld) return -1;
    for (int i = 0; i < n; i++) { esc_normaliser(t[i].nom, c, sizeof c); if (!strcmp(c, d)) return i; }
    for (int passe = 0; passe < 2; passe++) {                                   /* préfixe unique, puis morceau unique */
        int trouve = -1, nb = 0;
        size_t k = 0;
        for (int i = 0; i < n; i++) {
            esc_normaliser(t[i].nom, c, sizeof c);
            if (passe == 0 ? strncmp(c, d, ld) != 0 : strstr(c, d) == NULL) continue;
            nb++; trouve = i;
            if (amb && k + 2 < cap) k += (size_t)snprintf(amb + k, cap - k, "%s%s", k ? ", " : "", t[i].nom);
            if (k >= cap) k = cap - 1;
        }
        if (nb == 1) return trouve;
        if (nb > 1) return -2;
    }
    return -1;
}

/* ---- options ----------------------------------------------------------------------------------------------------- */
typedef struct {
    int race_j1, race_j1_alea;
    int nb_ia;
    int race_ia[ESC_IA_MAX], race_ia_alea[ESC_IA_MAX];
    int diff;
    int vision, fin;
} esc_options;

static uint32_t g_esc_alea;
static int esc_tirer_race(void)
{
    if (!g_esc_alea) g_esc_alea = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ 0x9E3779B9u;
    g_esc_alea ^= g_esc_alea << 13; g_esc_alea ^= g_esc_alea >> 17; g_esc_alea ^= g_esc_alea << 5;
    return (int)(g_esc_alea % 3);
}

static int esc_booleen(const char *v) { return v && *v && strcmp(v, "0") != 0; }

/* Lit SC2_ESCARMOUCHE_RACE, _RACE_IA (une race, ou une liste « zerg,protoss » : une par IA, la dernière répétée),
 * _NB_IA, _IA (difficulté), _VISION, _FIN. Valeur inconnue : journalisée, défaut gardé (tolérant). Les aléatoires sont
 * tirés ici. places = emplacements « User » de la carte (le nombre d'IA y est borné à places - 1). */
static void esc_options_lire(esc_options *o, int places)
{
    memset(o, 0, sizeof *o);
    const char *v = getenv("SC2_ESCARMOUCHE_RACE");
    int r = v && *v ? esc_race(v) : ESC_ALEATOIRE;
    if (r < 0) { sc2_log("[ESCARMOUCHE] SC2_ESCARMOUCHE_RACE=« %s » inconnue (terran|zerg|protoss|aleatoire) : aléatoire", v); r = ESC_ALEATOIRE; }
    o->race_j1_alea = r == ESC_ALEATOIRE;
    o->race_j1 = r == ESC_ALEATOIRE ? esc_tirer_race() : r;

    o->nb_ia = 1;
    v = getenv("SC2_ESCARMOUCHE_NB_IA");
    if (v && *v) {
        char *f; long n = strtol(v, &f, 10);
        if (*f || n < 1) sc2_log("[ESCARMOUCHE] SC2_ESCARMOUCHE_NB_IA=« %s » invalide (entier ≥ 1) : 1", v);
        else o->nb_ia = (int)(n > ESC_IA_MAX ? ESC_IA_MAX : n);
    }
    if (o->nb_ia > places - 1) {
        sc2_log("[ESCARMOUCHE] %d IA demandée(s), la carte n'a que %d place(s) : %d IA", o->nb_ia, places, places - 1);
        o->nb_ia = places - 1;
    }

    v = getenv("SC2_ESCARMOUCHE_RACE_IA");
    char liste[128]; snprintf(liste, sizeof liste, "%s", v && *v ? v : "aleatoire");
    int derniere = ESC_ALEATOIRE, k = 0;
    for (char *sv = NULL, *t = strtok_r(liste, ",;", &sv); k < ESC_IA_MAX; t = t ? strtok_r(NULL, ",;", &sv) : NULL, k++) {
        if (t) {
            int x = esc_race(t);
            if (x < 0) sc2_log("[ESCARMOUCHE] SC2_ESCARMOUCHE_RACE_IA : « %s » inconnue (terran|zerg|protoss|aleatoire) : aléatoire", t);
            derniere = x < 0 ? ESC_ALEATOIRE : x;
        }
        o->race_ia_alea[k] = derniere == ESC_ALEATOIRE;
        o->race_ia[k] = derniere == ESC_ALEATOIRE ? (k < o->nb_ia ? esc_tirer_race() : 0) : derniere;
    }

    v = getenv("SC2_ESCARMOUCHE_IA");
    int d = v && *v ? esc_difficulte(v) : 1;
    if (d < 0) {
        sc2_log("[ESCARMOUCHE] SC2_ESCARMOUCHE_IA=« %s » inconnue (tres-facile|facile|moyen|difficile|tres-difficile|dement, "
                "ou 0 à 5) : facile", v);
        d = 1;
    }
    o->diff = d;
    o->vision = esc_booleen(getenv("SC2_ESCARMOUCHE_VISION"));
    o->fin = esc_booleen(getenv("SC2_ESCARMOUCHE_FIN"));
}

/* Applique les options au MapInfo et au MapScript (en mémoire). Rend 0 et les deux nouveaux contenus (alloués), ou -1. */
static int esc_transformer(const uint8_t *info, size_t ninfo, const char *script, size_t nscript, const esc_options *o,
                           uint8_t **info2, size_t *ninfo2, char **script2, size_t *nscript2, int *ids_ia,
                           char *err, size_t cap)
{
    esc_mapinfo mi;
    *info2 = NULL; *script2 = NULL;
    if (esc_mapinfo_lire(info, ninfo, &mi, err, cap)) return -1;
    if (esc_mapinfo_escarmouche(&mi, o->race_j1, o->race_ia, o->nb_ia, ids_ia, err, cap)) return -1;
    size_t t = esc_mapinfo_ecrire(info, ninfo, &mi, NULL, 0);
    uint8_t *b = malloc(t ? t : 1);
    if (!b || esc_mapinfo_ecrire(info, ninfo, &mi, b, t) != t) { free(b); snprintf(err, cap, "MapInfo : écriture"); return -1; }
    esc_mapinfo relu;                                           /* aller-retour : la réécriture se relit à l'identique */
    if (esc_mapinfo_lire(b, t, &relu, err, cap) || relu.n != mi.n || relu.fin - relu.debut != t - mi.debut - (ninfo - mi.fin)) {
        free(b); snprintf(err, cap, "MapInfo réécrit illisible"); return -1;
    }
    char avant[1024];
    esc_galaxy_avant(avant, sizeof avant, ids_ia, o->nb_ia, o->diff, o->vision, o->fin);
    size_t ns = 0;
    char *s = esc_mapscript_inserer(script, nscript, avant, o->fin ? ESC_GALAXY_FIN : NULL, &ns, err, cap);
    if (!s) { free(b); return -1; }
    *info2 = b; *ninfo2 = t; *script2 = s; *nscript2 = ns;
    return 0;
}

/* Description lisible des joueurs (journal). */
static void esc_decrire(char *o, size_t cap, const esc_options *op, const int *ids)
{
    size_t k = (size_t)snprintf(o, cap, "J1 humain %s%s", ESC_RACE_NOM[op->race_j1], op->race_j1_alea ? " (tirée au sort)" : "");
    for (int i = 0; i < op->nb_ia && k < cap; i++)
        k += (size_t)snprintf(o + k, cap - k, ", J%d IA %s%s", ids[i], ESC_RACE_NOM[op->race_ia[i]],
                              op->race_ia_alea[i] ? " (tirée au sort)" : "");
    if (k < cap) snprintf(o + k, cap - k, " ; difficulté %s (%s = %d)%s%s", ESC_DIFF_NOM[op->diff], ESC_DIFF_GALAXY[op->diff],
                          op->diff, op->vision ? " ; vision totale (SC2_ESCARMOUCHE_VISION)" : "",
                          op->fin ? " ; banques de fin (SC2_ESCARMOUCHE_FIN)" : "");
}

/* ======================================================================================================== */
/* StormLib : cache, copie, réécriture                                                                      */
/* ======================================================================================================== */
#ifndef SC2_ESCARMOUCHE_SANS_MPQ
#include "StormLib.h"

static char g_esc_carte[300];                    /* chemin Windows rendu au lanceur simulé ("" : aucun) */

const char *sc2_escarmouche_carte(void) { return g_esc_carte[0] ? g_esc_carte : NULL; }

/* Windows ignore la casse, l'hôte non (« Base.SC2Data » demandé, « base.SC2Data » possible sur disque) : composant
 * par composant à partir de debut, le premier nom égal sans tenir compte de la casse (même règle que sc2_iat.c). */
static void esc_casse(char *p, size_t debut)
{
    struct stat st;
    if (!stat(p, &st)) return;
    for (size_t i = debut; p[i]; ) {
        size_t j = i; while (p[j] && p[j] != '/') j++;
        char sv = p[j]; p[j] = 0;
        if (stat(p, &st)) {
            char dir[1024]; size_t ld = i > 1 ? i - 1 : 1;
            if (ld >= sizeof dir) { p[j] = sv; return; }
            memcpy(dir, p, ld); dir[ld] = 0;
            DIR *d = opendir(dir); struct dirent *e; int ok = 0;
            while (d && (e = readdir(d)))
                if (strlen(e->d_name) == j - i && !strcasecmp(e->d_name, p + i)) { memcpy(p + i, e->d_name, j - i); ok = 1; break; }
            if (d) closedir(d);
            if (!ok) { p[j] = sv; return; }
        }
        p[j] = sv; i = sv ? j + 1 : j;
    }
}

static int esc_mkdir_p(const char *chemin)
{
    char t[1024];
    if (snprintf(t, sizeof t, "%s", chemin) >= (int)sizeof t) return -1;
    for (char *q = t + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
    return mkdir(t, 0755) && errno != EEXIST ? -1 : 0;
}

/* Fichier interne entier en mémoire (malloc, NUL final en plus). 0 ou -1. */
static int esc_lire_mpq(HANDLE h, const char *nom, uint8_t **buf, size_t *n)
{
    HANDLE f = NULL;
    *buf = NULL; *n = 0;
    if (!SFileOpenFileEx(h, nom, SFILE_OPEN_FROM_MPQ, &f)) return -1;
    DWORD hi = 0, lo = SFileGetFileSize(f, &hi);
    if (lo == SFILE_INVALID_SIZE || hi || lo > (64u << 20)) { SFileCloseFile(f); return -1; }
    uint8_t *b = malloc((size_t)lo + 1);
    DWORD lus = 0;
    if (!b || (lo && (!SFileReadFile(f, b, lo, &lus, NULL) || lus != lo))) { free(b); SFileCloseFile(f); return -1; }
    SFileCloseFile(f);
    b[lo] = 0;
    *buf = b; *n = lo;
    return 0;
}

static int esc_ecrire_mpq(HANDLE h, const char *nom, const void *b, size_t n)
{
    HANDLE f = NULL;
    if (!SFileCreateFile(h, nom, 0, (DWORD)n, 0, MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING, &f)) return -1;
    int ok = SFileWriteFile(f, b, (DWORD)n, MPQ_COMPRESSION_ZLIB);
    return SFileFinishFile(f) && ok ? 0 : -1;
}

/* Cartes mêlée du cache (triées) ; -1 si l'archive est illisible. */
static int esc_lister(HANDLE h, esc_carte **out)
{
    SFILE_FIND_DATA fd;
    int n = 0, cap = 0;
    esc_carte *t = NULL;
    *out = NULL;
    HANDLE hf = SFileFindFirstFile(h, "*.s2ma", &fd, NULL);
    for (bool ok = hf != NULL; ok; ok = SFileFindNextFile(hf, &fd)) {
        esc_carte c;
        if (esc_carte_depuis_interne(fd.cFileName, &c)) continue;
        c.taille = fd.dwFileSize;
        if (n == cap) {
            esc_carte *nt = realloc(t, sizeof *t * (size_t)(cap = cap ? cap * 2 : 64));
            if (!nt) break;
            t = nt;
        }
        t[n++] = c;
    }
    if (hf) SFileFindClose(hf);
    qsort(t, (size_t)n, sizeof *t, esc_carte_ordre);
    *out = t;
    return n;
}

/* Extrait l'entrée du cache vers dest (temporaire voisin puis rename). 0 ou -1 (err). */
static int esc_extraire(HANDLE hc, const esc_carte *c, const char *dest, char *err, size_t cap)
{
    uint8_t *b; size_t n;
    if (esc_lire_mpq(hc, c->interne, &b, &n)) { snprintf(err, cap, "lecture de %s dans le cache", c->interne); return -1; }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", dest);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(b, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    free(b);
    if (!ok || rename(tmp, dest)) { snprintf(err, cap, "écriture de %s : %s", dest, strerror(errno)); unlink(tmp); return -1; }
    return 0;
}

/* Prépare la copie dest de la carte c selon o : extraction, réécriture, compactage, relecture. 0 ou -1 (err). */
static int esc_preparer(HANDLE hc, const esc_carte *c, const char *dest, const esc_options *o, int *ids_ia,
                        int *places, char *err, size_t cap)
{
    if (esc_extraire(hc, c, dest, err, cap)) return -1;
    HANDLE h = NULL;
    if (!SFileOpenArchive(dest, 0, 0, &h)) { snprintf(err, cap, "copie %s : ouverture StormLib en écriture (erreur %u)", dest, (unsigned)SErrGetLastError()); return -1; }
    uint8_t *info = NULL, *info2 = NULL; size_t ni = 0, ni2 = 0;
    uint8_t *script = NULL; char *script2 = NULL; size_t ns = 0, ns2 = 0;
    int r = -1;
    if (esc_lire_mpq(h, "MapInfo", &info, &ni)) { snprintf(err, cap, "MapInfo absent de la carte"); goto fin; }
    if (esc_lire_mpq(h, "MapScript.galaxy", &script, &ns)) { snprintf(err, cap, "MapScript.galaxy absent de la carte"); goto fin; }
    esc_mapinfo mi;
    if (esc_mapinfo_lire(info, ni, &mi, err, cap)) goto fin;
    *places = esc_places(&mi);
    esc_options oc = *o;
    if (oc.nb_ia > *places - 1) oc.nb_ia = *places - 1;
    if (esc_transformer(info, ni, (const char *)script, ns, &oc, &info2, &ni2, &script2, &ns2, ids_ia, err, cap)) goto fin;
    if (esc_ecrire_mpq(h, "MapInfo", info2, ni2)) { snprintf(err, cap, "réécriture de MapInfo (erreur %u)", (unsigned)SErrGetLastError()); goto fin; }
    if (esc_ecrire_mpq(h, "MapScript.galaxy", script2, ns2)) { snprintf(err, cap, "réécriture de MapScript.galaxy (erreur %u)", (unsigned)SErrGetLastError()); goto fin; }
    /* SC2_ESCARMOUCHE_FIN : les banques du banc déclarées dans BankList.xml, comme celles des cartes de la campagne
     * (TRaynor01.SC2Map\BankList.xml : <Bank Name="TCampaign" Player="1"/>…) — sans déclaration, la banque
     * EscarmoucheDebut n'a pas été écrite au banc du 23/10. Échec : journalisé, la carte reste jouable. */
    if (o->fin && esc_ecrire_mpq(h, "BankList.xml", ESC_BANKLIST, sizeof ESC_BANKLIST - 1))
        sc2_log("[ESCARMOUCHE] BankList.xml non ajouté (erreur %u) : banques de fin probablement sans effet",
                (unsigned)SErrGetLastError());
    if (!SFileCompactArchive(h, NULL, false))
        sc2_log("[ESCARMOUCHE] compactage de la copie refusé (erreur %u) : archive gardée non compactée", (unsigned)SErrGetLastError());
    r = 0;
fin:
    if (!SFileCloseArchive(h) && !r) { snprintf(err, cap, "fermeture de la copie (erreur %u)", (unsigned)SErrGetLastError()); r = -1; }
    free(info); free(script);
    if (r) { free(info2); free(script2); return -1; }
    /* relecture de contrôle : ce que le jeu ouvrira */
    if (!SFileOpenArchive(dest, 0, MPQ_OPEN_READ_ONLY, &h)) { snprintf(err, cap, "copie %s illisible après réécriture", dest); r = -1; }
    else {
        uint8_t *bi = NULL, *bs = NULL; size_t li = 0, ls = 0;
        esc_mapinfo relu;
        if (esc_lire_mpq(h, "MapInfo", &bi, &li) || li != ni2 || memcmp(bi, info2, li) || esc_mapinfo_lire(bi, li, &relu, err, cap))
            { if (!err[0]) snprintf(err, cap, "MapInfo relu différent de celui écrit"); r = -1; }
        else if (esc_lire_mpq(h, "MapScript.galaxy", &bs, &ls) || ls != ns2 || memcmp(bs, script2, ls))
            { snprintf(err, cap, "MapScript.galaxy relu différent de celui écrit"); r = -1; }
        free(bi); free(bs);
        SFileCloseArchive(h);
    }
    free(info2); free(script2);
    return r;
}

/* Copies des lancements précédents (*.SC2Map, *.tmp du dossier privé Maps/Escarmouche, fichiers seulement) : retirées,
 * pour qu'un C:\ conservé d'un lancement à l'autre (banc, app Mac) n'en accumule pas. Rend le nombre retiré. */
static int esc_nettoyer(const char *dossier)
{
    DIR *d = opendir(dossier);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (!(l > 7 && !strcasecmp(e->d_name + l - 7, ".SC2Map")) && !(l > 4 && !strcmp(e->d_name + l - 4, ".tmp"))) continue;
        char p[1400]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dossier, e->d_name);
        if (!lstat(p, &st) && S_ISREG(st.st_mode) && !unlink(p)) n++;
    }
    if (d) closedir(d);
    return n;
}

static HANDLE esc_ouvrir_cache(char *chemin, size_t cap)
{
    const char *inst = getenv("SC2_INSTALL");
    size_t debut;
    if (inst && *inst) { snprintf(chemin, cap, "%s", inst); }
    else { const char *r = getenv("SC2_ROOT"); snprintf(chemin, cap, "%s/StarCraft II", r && *r ? r : "sc2-c"); }
    debut = strlen(chemin) + 1;
    snprintf(chemin + strlen(chemin), cap - strlen(chemin), "/Mods/Liberty.SC2Mod/Base.SC2Data");
    esc_casse(chemin, debut);
    HANDLE h = NULL;
    if (!SFileOpenArchive(chemin, 0, MPQ_OPEN_READ_ONLY, &h)) return NULL;
    return h;
}

/* SC2_ESCARMOUCHE=verifier : chaque carte préparée (options par défaut, IA Zerg) dans un temporaire et relue. */
static int esc_verifier(HANDLE hc, const esc_carte *t, int n, const char *dossier)
{
    int ko = 0;
    char dest[1100];
    snprintf(dest, sizeof dest, "%s/.verification.SC2Map", dossier);
    esc_options o;
    memset(&o, 0, sizeof o);
    o.race_j1 = 0; o.nb_ia = 1; o.race_ia[0] = 1; o.diff = 1; o.fin = 1; o.vision = 1;
    for (int i = 0; i < n; i++) {
        char err[ESC_ERR] = ""; int ids[ESC_IA_MAX] = { 0 }, places = 0;
        int r = esc_preparer(hc, &t[i], dest, &o, ids, &places, err, sizeof err);
        if (r) ko++;
        sc2_log("[ESCARMOUCHE] vérification %2d %-24s (%d) : %s%s%s", i + 1, t[i].nom, t[i].joueurs, r ? "ÉCHEC — " : "OK",
                r ? err : "", r ? "" : (places == t[i].joueurs ? "" : " (places ≠ (N) du cache)"));
        if (!r && places != t[i].joueurs) sc2_log("[ESCARMOUCHE]    %d place(s) « User » dans le MapInfo, %d annoncée(s)", places, t[i].joueurs);
    }
    unlink(dest);
    sc2_log("[ESCARMOUCHE] vérification : %d/%d carte(s) préparées et relues sans erreur", n - ko, n);
    return ko ? 1 : 0;
}

int sc2_escarmouche_demarrer(const char *racine, int *code_sortie)
{
    const char *e = getenv("SC2_ESCARMOUCHE");
    g_esc_carte[0] = 0;
    if (!e || !*e || !strcmp(e, "0")) return 0;
    int liste = !strcasecmp(e, "liste"), verif = !strcasecmp(e, "verifier");
    char arc[1024];
    HANDLE hc = esc_ouvrir_cache(arc, sizeof arc);
    if (!hc) {
        sc2_log("[ESCARMOUCHE] ÉCHEC : cache des cartes mêlée %s illisible (StormLib, erreur %u ; SC2_INSTALL ?) — %s", arc,
                (unsigned)SErrGetLastError(), liste || verif ? "arrêt" : "lancement normal, sans escarmouche");
        if (liste || verif) { *code_sortie = 1; return 1; }
        return 0;
    }
    esc_carte *t = NULL;
    int n = esc_lister(hc, &t);
    if (liste) {
        sc2_log("[ESCARMOUCHE] liste : %d carte(s) mêlée dans le cache Battle.net de %s", n, arc);
        for (int i = 0; i < n; i++)
            sc2_log("[ESCARMOUCHE]   %2d  %-24s %d joueurs  (%u o)", i + 1, t[i].nom, t[i].joueurs, t[i].taille);
        sc2_log("[ESCARMOUCHE] liste : fin (SC2_ESCARMOUCHE=<nom> ou <n°> lance une partie contre l'IA)");
        free(t); SFileCloseArchive(hc);
        *code_sortie = 0;
        return 1;
    }
    char dossier[1024];
    snprintf(dossier, sizeof dossier, "%s/Users/sc2/Documents/StarCraft II/Maps/Escarmouche", racine && *racine ? racine : "sc2-c");
    if (esc_mkdir_p(dossier)) {
        sc2_log("[ESCARMOUCHE] ÉCHEC : dossier %s impossible à créer (%s) — %s", dossier, strerror(errno),
                verif ? "arrêt" : "lancement normal, sans escarmouche");
        free(t); SFileCloseArchive(hc);
        if (verif) { *code_sortie = 1; return 1; }
        return 0;
    }
    int vieilles = esc_nettoyer(dossier);
    if (vieilles) sc2_log("[ESCARMOUCHE] %d copie(s) d'un lancement précédent retirée(s) de %s", vieilles, dossier);
    if (verif) {
        *code_sortie = esc_verifier(hc, t, n, dossier);
        free(t); SFileCloseArchive(hc);
        return 1;
    }
    char amb[512];
    int i = esc_carte_choisir(t, n, e, amb, sizeof amb);
    if (i < 0) {
        if (i == -2) sc2_log("[ESCARMOUCHE] ÉCHEC : carte « %s » ambiguë (%s) — lancement normal, sans escarmouche", e, amb);
        else sc2_log("[ESCARMOUCHE] ÉCHEC : carte « %s » introuvable parmi les %d cartes mêlée du cache (SC2_ESCARMOUCHE=liste "
                     "les donne) — lancement normal, sans escarmouche", e, n);
        free(t); SFileCloseArchive(hc);
        return 0;
    }
    esc_options o;
    esc_options_lire(&o, t[i].joueurs);
    char hote[1100], err[ESC_ERR] = "";
    snprintf(hote, sizeof hote, "%s/%s.SC2Map", dossier, t[i].nom);
    int ids[ESC_IA_MAX] = { 0 }, places = 0;
    int r = esc_preparer(hc, &t[i], hote, &o, ids, &places, err, sizeof err);
    if (!r && o.nb_ia > places - 1) o.nb_ia = places - 1;
    if (r) {
        sc2_log("[ESCARMOUCHE] ÉCHEC : carte %s (%s) — %s — lancement normal, sans escarmouche", t[i].nom, t[i].interne, err);
        unlink(hote);
    } else {
        char d[512];
        esc_decrire(d, sizeof d, &o, ids);
        snprintf(g_esc_carte, sizeof g_esc_carte, "C:\\Users\\sc2\\Documents\\StarCraft II\\Maps\\Escarmouche\\%s.SC2Map", t[i].nom);
        sc2_log("[ESCARMOUCHE] carte n°%d %s (%d joueurs, cache %s) : %s", i + 1, t[i].nom, t[i].joueurs, t[i].interne, d);
        sc2_log("[ESCARMOUCHE] copie privée %s (MapInfo et MapScript.galaxy réécrits, relus) ; lanceur simulé, mode 2 : %s",
                hote, g_esc_carte);
        const char *sb = getenv("SC2_RUN_SANS_BNET");
        if (!sb || strcmp(sb, "1")) {
            sc2_log("[ESCARMOUCHE] SC2_RUN_SANS_BNET=%s → 1 : l'escarmouche passe par le lanceur simulé", sb ? sb : "(absente)");
            setenv("SC2_RUN_SANS_BNET", "1", 1);
        }
    }
    free(t); SFileCloseArchive(hc);
    return 0;
}
#endif /* SC2_ESCARMOUCHE_SANS_MPQ */
