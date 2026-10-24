/* test_escarmouche.c — V4.10 (23/10) : partie « pur calcul » de runtime/sc2_escarmouche.c, sans StormLib ni données.
 *
 * Le module est INCLUS avec SC2_ESCARMOUCHE_SANS_MPQ (ses fonctions static sont visibles ici ; la partie StormLib —
 * cache, copie, réécriture de l'archive — n'est pas compilée : elle se vérifie sur les vraies cartes par
 * SC2_ESCARMOUCHE=verifier, cf. docs/BANC-MAC.md). Vérifié :
 *   1. noms : normalisation (casse, accents UTF-8, ponctuation), races et difficultés (français, anglais, codes du jeu,
 *      chiffres ; valeurs c_skir* 0 à 5), valeurs inconnues → -1 ;
 *   2. MapInfo 0x18 SYNTHÉTIQUE (fabriqué ici, aucun octet de carte du jeu) : lecture, joueur 1 humain + IA sur les
 *      places « User » suivantes, autres joueurs intacts, réécriture → relecture identique, octets hors joueurs
 *      conservés ; refus : version 0x17, tronqué, joueur 1 absent ou non User, plus d'IA que de places ;
 *   3. MapScript : insertion avant l'unique « MeleeInitResources(); » (indentation et CRLF de la ligne visée, LF aussi),
 *      fonctions de fin avant l'en-tête de la fonction ; refus : 0 ou 2 occurrences, ancre pas en tête de ligne ;
 *   4. cartes du cache : noms internes reconnus ou écartés, tri (joueurs, nom), choix par nom tolérant, n°, préfixe ou
 *      morceau unique, ambiguïté signalée, inconnue ;
 *   5. options lues de l'environnement (races, liste de races d'IA, nombre d'IA borné aux places, difficulté, VISION,
 *      FIN, valeurs inconnues → défauts) et transformation complète (esc_transformer).
 */
#define SC2_ESCARMOUCHE_SANS_MPQ 1
#include "../sc2_escarmouche.c"

#include <stdarg.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("PASS  "); } else { g_fail++; printf("FAIL  "); } \
                              printf(__VA_ARGS__); putchar('\n'); } while (0)

void sc2_log(const char *fmt, ...)
{
    va_list ap;
    fputs("      ", stdout);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n');
}

/* ---- MapInfo synthétique ---------------------------------------------------------------------------------------- */
static void p32(uint8_t *o, size_t *k, uint32_t v) { esc_p32(o, k, v); }
static void pstr(uint8_t *o, size_t *k, const char *s) { size_t l = strlen(s) + 1; memcpy(o + *k, s, l); *k += l; }
typedef struct { uint8_t id; uint32_t ctl; int32_t coul; const char *race, *decal; } jsynth;
/* MapInfo de version ver : petit aperçu de type 2 (chaîne) si sp2, joueurs j[0..n), suite de 28 octets 0xA0+i. */
static size_t synth(uint8_t *o, uint32_t ver, int sp2, const jsynth *j, int n, size_t *debut)
{
    size_t k = 0;
    memcpy(o, "IpaM", 4); k = 4;
    p32(o, &k, ver); p32(o, &k, 0x1234); p32(o, &k, 0);
    p32(o, &k, 176); p32(o, &k, 168);
    p32(o, &k, sp2 ? 2 : 1); p32(o, &k, 1);
    if (sp2) pstr(o, &k, "Minimap.tga");
    p32(o, &k, 0);
    pstr(o, &k, "Dark"); pstr(o, &k, "Synthese");
    for (int i = 0; i < 4; i++) p32(o, &k, 10u + (uint32_t)i);
    p32(o, &k, 0x8000);
    p32(o, &k, 0); pstr(o, &k, "Assets\\Textures\\chargement.dds");
    o[k++] = 0x20; o[k++] = 0x03;
    for (int i = 0; i < 8; i++) p32(o, &k, (uint32_t)(i == 0 ? 800 : i == 1 ? 600 : i));
    *debut = k;
    p32(o, &k, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        o[k++] = j[i].id; p32(o, &k, j[i].ctl); p32(o, &k, (uint32_t)j[i].coul); pstr(o, &k, j[i].race);
        p32(o, &k, 7); p32(o, &k, (uint32_t)i); p32(o, &k, 9); pstr(o, &k, j[i].decal);
    }
    for (int i = 0; i < 28; i++) o[k++] = (uint8_t)(0xA0 + i);
    return k;
}

static void test_noms(void)
{
    char n[64];
    esc_normaliser("Très-Difficile", n, sizeof n);
    CHECK(!strcmp(n, "tresdifficile"), "normalisation « Très-Difficile » → « %s »", n);
    esc_normaliser("DÉMENTIEL", n, sizeof n);
    CHECK(!strcmp(n, "dementiel"), "normalisation « DÉMENTIEL » (É majuscule) → « %s »", n);
    esc_normaliser("(2)Steppes of War.SC2Map", n, sizeof n);
    CHECK(!strcmp(n, "2steppesofwarsc2map"), "normalisation ponctuation → « %s »", n);
    CHECK(esc_race("terran") == 0 && esc_race("Terr") == 0 && esc_race("T") == 0, "race terran / Terr / T → 0");
    CHECK(esc_race("ZERG") == 1 && esc_race("z") == 1, "race ZERG / z → 1");
    CHECK(esc_race("protoss") == 2 && esc_race("Prot") == 2, "race protoss / Prot → 2");
    CHECK(esc_race("aléatoire") == ESC_ALEATOIRE && esc_race("aleatoire") == ESC_ALEATOIRE && esc_race("RAND") == ESC_ALEATOIRE,
          "race aléatoire / aleatoire / RAND → aléatoire");
    CHECK(esc_race("orc") == -1 && esc_race("") == -1, "race inconnue ou vide → -1");
    static const char *const d[] = { "tres-facile", "facile", "moyen", "difficile", "tres-difficile", "dement" };
    static const char *const a[] = { "VyEy", "Easy", "Medi", "Hard", "VyHd", "Insa" };
    int ok = 1;
    for (int i = 0; i < 6; i++) {
        char c[2] = { (char)('0' + i), 0 };
        ok &= esc_difficulte(d[i]) == i && esc_difficulte(a[i]) == i && esc_difficulte(c) == i;
    }
    CHECK(ok, "difficultés : noms français, codes 3004 et chiffres → c_skirVeryEasy 0 … c_skirCheater 5");
    CHECK(esc_difficulte("Très facile") == 0 && esc_difficulte("démentiel") == 5 && esc_difficulte("Medium") == 2,
          "difficultés accentuées et anglaises");
    CHECK(esc_difficulte("6") == -1 && esc_difficulte("impossible") == -1, "difficulté inconnue → -1");
}

static void test_mapinfo(void)
{
    uint8_t b[2048], o[2048];
    size_t debut, n;
    char err[ESC_ERR];
    esc_mapinfo mi;
    jsynth j2[] = { { 0, 3, 0, "", "" }, { 1, 1, -1, "", "" }, { 2, 1, -1, "", "" } };
    n = synth(b, 0x18, 0, j2, 3, &debut);
    CHECK(esc_mapinfo_lire(b, n, &mi, err, sizeof err) == 0 && mi.n == 3 && mi.debut == debut && mi.fin == n - 28
          && mi.largeur == 176 && mi.hauteur == 168, "MapInfo synthétique 2 places : lu (%d joueurs, joueurs en [%zu, %zu))",
          mi.n, mi.debut, mi.fin);
    CHECK(esc_places(&mi) == 2, "2 places « User »");
    int races[ESC_IA_MAX] = { 1 }, ids[ESC_IA_MAX] = { 0 };
    CHECK(esc_mapinfo_escarmouche(&mi, 0, races, 1, ids, err, sizeof err) == 0 && ids[0] == 2, "J1 Terran + 1 IA → J2");
    CHECK(mi.j[0].control == 3 && mi.j[0].couleur == 0 && !mi.j[0].race[0], "joueur 0 (neutre) intact");
    CHECK(mi.j[1].control == 1 && mi.j[1].couleur == 2 && !strcmp(mi.j[1].race, "Terr"), "J1 : User, bleu (2), Terr");
    CHECK(mi.j[2].control == 2 && mi.j[2].couleur == 1 && !strcmp(mi.j[2].race, "Zerg"), "J2 : Computer, rouge (1), Zerg");
    size_t t = esc_mapinfo_ecrire(b, n, &mi, NULL, 0);
    CHECK(t == n + 8 && esc_mapinfo_ecrire(b, n, &mi, o, sizeof o) == t, "réécrit : %zu o (+ 2 races de 4 lettres)", t);
    CHECK(esc_mapinfo_ecrire(b, n, &mi, o, t - 1) == 0, "tampon trop petit → 0");
    esc_mapinfo r;
    CHECK(esc_mapinfo_lire(o, t, &r, err, sizeof err) == 0 && r.n == 3 && r.j[2].control == 2 && !strcmp(r.j[2].race, "Zerg")
          && r.j[1].couleur == 2 && r.j[2].a == 7 && r.j[2].depart == 2 && r.j[2].c == 9, "relu : joueurs attendus, champs u32 conservés");
    CHECK(!memcmp(o, b, debut) && !memcmp(o + t - 28, b + n - 28, 28), "octets avant et après les joueurs conservés");

    /* 4 places + un joueur hostile en plus (cas DeltaQuadrant, DirtSide…) ; petit aperçu en chaîne ; décalcomanies */
    jsynth j4[] = { { 0, 3, 0, "", "" }, { 1, 1, -1, "", "Decal1" }, { 2, 1, -1, "", "" }, { 3, 1, -1, "", "" },
                    { 4, 1, -1, "", "" }, { 5, 4, 7, "Zerg", "x" } };
    n = synth(b, 0x18, 1, j4, 6, &debut);
    CHECK(esc_mapinfo_lire(b, n, &mi, err, sizeof err) == 0 && mi.n == 6 && esc_places(&mi) == 4,
          "4 places + 1 hostile, aperçu en chaîne : lu (%d joueurs, %d places)", mi.n, esc_places(&mi));
    int r3[ESC_IA_MAX] = { 2, 0, 1 };
    CHECK(esc_mapinfo_escarmouche(&mi, 1, r3, 3, ids, err, sizeof err) == 0 && ids[0] == 2 && ids[1] == 3 && ids[2] == 4,
          "J1 Zerg + 3 IA → J2, J3, J4");
    CHECK(!strcmp(mi.j[2].race, "Prot") && !strcmp(mi.j[3].race, "Terr") && !strcmp(mi.j[4].race, "Zerg")
          && mi.j[2].couleur == 1 && mi.j[3].couleur == 3 && mi.j[4].couleur == 4, "races Prot, Terr, Zerg ; couleurs 1, 3, 4");
    CHECK(mi.j[5].control == 4 && mi.j[5].couleur == 7 && !strcmp(mi.j[5].race, "Zerg"), "joueur hostile 5 intact");
    t = esc_mapinfo_ecrire(b, n, &mi, o, sizeof o);
    CHECK(t && esc_mapinfo_lire(o, t, &r, err, sizeof err) == 0 && r.j[1].ldecal == 6 && !memcmp(r.j[1].decal, "Decal1", 6),
          "réécrit puis relu, décalcomanie conservée");
    esc_mapinfo_lire(b, n, &mi, err, sizeof err);
    CHECK(esc_mapinfo_escarmouche(&mi, 0, r3, 4, ids, err, sizeof err) == -1, "4 IA sur 4 places → refus (« %s »)", err);

    n = synth(b, 0x17, 0, j2, 3, &debut);
    CHECK(esc_mapinfo_lire(b, n, &mi, err, sizeof err) == -1 && strstr(err, "0x17"), "version 0x17 → refus (« %s »)", err);
    n = synth(b, 0x18, 0, j2, 3, &debut);
    CHECK(esc_mapinfo_lire(b, debut + 10, &mi, err, sizeof err) == -1, "tronqué dans les joueurs → refus (« %s »)", err);
    CHECK(esc_mapinfo_lire(b, 60, &mi, err, sizeof err) == -1, "tronqué avant les joueurs → refus (« %s »)", err);
    CHECK(esc_mapinfo_lire((const uint8_t *)"MapI", 4, &mi, err, sizeof err) == -1, "signature absente → refus");
    jsynth jo[] = { { 0, 3, 0, "", "" }, { 1, 2, -1, "", "" }, { 2, 1, -1, "", "" } };
    n = synth(b, 0x18, 0, jo, 3, &debut);
    esc_mapinfo_lire(b, n, &mi, err, sizeof err);
    CHECK(esc_mapinfo_escarmouche(&mi, 0, races, 1, ids, err, sizeof err) == -1, "joueur 1 non User → refus (« %s »)", err);
}

static const char SCRIPT_CRLF[] =
    "include \"TriggerLibs/NativeLib\"\r\n"
    "\r\n"
    "//----\r\n"
    "// Declencheur d'essai\r\n"
    "//----\r\n"
    "bool gt_Essai_Func (bool testConds, bool runActions) {\r\n"
    "    if (!runActions) {\r\n"
    "        return true;\r\n"
    "    }\r\n"
    "\r\n"
    "    MeleeInitResources();\r\n"
    "    MeleeInitUnits();\r\n"
    "    return true;\r\n"
    "}\r\n";

static void test_mapscript(void)
{
    char err[ESC_ERR];
    size_t n = 0;
    char *o = esc_mapscript_inserer(SCRIPT_CRLF, sizeof SCRIPT_CRLF - 1, "// note\nPlayerSetDifficulty(2, 1);\n", NULL, &n,
                                    err, sizeof err);
    const char *attendu = "    // note\r\n    PlayerSetDifficulty(2, 1);\r\n    MeleeInitResources();\r\n";
    CHECK(o && strstr(o, attendu) && n == sizeof SCRIPT_CRLF - 1 + strlen("    // note\r\n    PlayerSetDifficulty(2, 1);\r\n")
          && strlen(o) == n, "CRLF : 2 lignes insérées avant MeleeInitResources, indentation et CRLF de la ligne visée");
    free(o);
    o = esc_mapscript_inserer(SCRIPT_CRLF, sizeof SCRIPT_CRLF - 1, "X();\n", ESC_GALAXY_FIN, &n, err, sizeof err);
    const char *f = o ? strstr(o, "bool gt_EscFin_Victoire") : NULL, *h = o ? strstr(o, "bool gt_Essai_Func") : NULL;
    const char *d = o ? strstr(o, "// Declencheur d'essai\r\n//----\r\n//----") : NULL;
    CHECK(o && f && h && f < h && d && strstr(o, "    X();\r\n    MeleeInitResources();") && strstr(o, "    return true;\r\n}\r\n\r\nbool gt_EscFin_Defaite"),
          "fonctions de fin insérées juste avant l'en-tête de la fonction (CRLF), appel avant MeleeInitResources");
    free(o);
    /* LF seulement, ancre en colonne 0 */
    const char lf[] = "void f () {\nMeleeInitResources();\n}\n";
    o = esc_mapscript_inserer(lf, sizeof lf - 1, "A();\nB();\n", NULL, &n, err, sizeof err);
    CHECK(o && !strcmp(o, "void f () {\nA();\nB();\nMeleeInitResources();\n}\n"), "LF, sans indentation");
    free(o);
    const char deux[] = "void f () {\n    MeleeInitResources();\n    MeleeInitResources();\n}\n";
    CHECK(!esc_mapscript_inserer(deux, sizeof deux - 1, "A();\n", NULL, &n, err, sizeof err) && strstr(err, "2 "),
          "2 occurrences → refus (« %s »)", err);
    const char zero[] = "void f () {\n    MeleeInitUnits();\n}\n";
    CHECK(!esc_mapscript_inserer(zero, sizeof zero - 1, "A();\n", NULL, &n, err, sizeof err) && strstr(err, "0 "),
          "0 occurrence → refus (« %s »)", err);
    const char milieu[] = "void f () {\n    x = 1; MeleeInitResources();\n}\n";
    CHECK(!esc_mapscript_inserer(milieu, sizeof milieu - 1, "A();\n", NULL, &n, err, sizeof err),
          "ancre au milieu d'une ligne → refus (« %s »)", err);
    const char sansf[] = "    MeleeInitResources();\n";
    CHECK(!esc_mapscript_inserer(sansf, sizeof sansf - 1, "A();\n", ESC_GALAXY_FIN, &n, err, sizeof err),
          "fonctions de fin sans en-tête de fonction → refus (« %s »)", err);
    char av[1024];
    int ids[3] = { 2, 3, 4 };
    esc_galaxy_avant(av, sizeof av, ids, 3, 4, 1, 1);
    CHECK(strstr(av, "PlayerSetDifficulty(2, 4);\nPlayerSetDifficulty(3, 4);\nPlayerSetDifficulty(4, 4);\n")
          && strstr(av, "VisEnable(c_visTypeFog, false);") && strstr(av, "\"gt_EscFin_Defaite\"), 1, c_gameResultDefeat")
          && strstr(av, "BankLoad(\"EscarmoucheDebut\", 1);"), "lignes Galaxy : difficulté par IA, vision, banques de fin");
    int ascii = 1;
    for (const char *p = av; *p; p++) ascii &= (unsigned char)*p < 0x80;
    for (const char *p = ESC_GALAXY_FIN; *p; p++) ascii &= (unsigned char)*p < 0x80;
    CHECK(ascii, "Galaxy inséré en ASCII seulement");
    CHECK(strstr(ESC_BANKLIST, "<Bank Name=\"EscarmoucheDebut\" Player=\"1\"/>") && strstr(ESC_BANKLIST, "\"EscarmoucheFinVictoire\"")
          && strstr(ESC_BANKLIST, "\"EscarmoucheFinDefaite\"") && strstr(ESC_BANKLIST, "</BankList>"),
          "BankList.xml (SC2_ESCARMOUCHE_FIN) : les trois banques du joueur 1");
}

static void test_cartes(void)
{
    static const char *const internes[] = {
        "BattlenetCache\\(4)LostTemple.SC2Map\\aa.s2ma", "BattlenetCache\\(2)SteppesofWar.SC2Map\\bb.s2ma",
        "BattlenetCache\\(2)NoviceSteppesofWar.SC2Map\\cc.s2ma", "BattlenetCache\\(4)Metalopolis.SC2Map\\dd.s2ma",
        "BattlenetCache\\(4)NoviceMetalopolis.SC2Map\\ee.s2ma", "BattlenetCache\\(8)Megaton.SC2Map\\ff.S2MA",
        "BattlenetCache\\Liberty.SC2Mod\\11.s2ma", "BattlenetCache\\(2)Sans.SC2Map\\22.s2mv", "Autre\\(2)X.SC2Map\\33.s2ma",
        "BattlenetCache\\(0)Zero.SC2Map\\44.s2ma",
    };
    esc_carte t[16];
    int n = 0, ecartes = 0;
    for (size_t i = 0; i < sizeof internes / sizeof internes[0]; i++) {
        if (esc_carte_depuis_interne(internes[i], &t[n]) == 0) n++; else ecartes++;
    }
    CHECK(n == 6 && ecartes == 4, "noms internes : 6 cartes reconnues, 4 entrées écartées (mod, aperçu, autre dossier, (0))");
    qsort(t, (size_t)n, sizeof t[0], esc_carte_ordre);
    CHECK(!strcmp(t[0].nom, "NoviceSteppesofWar") && !strcmp(t[1].nom, "SteppesofWar") && !strcmp(t[2].nom, "LostTemple")
          && !strcmp(t[5].nom, "Megaton") && t[5].joueurs == 8, "tri (joueurs, nom) : %s, %s, %s … %s", t[0].nom, t[1].nom,
          t[2].nom, t[5].nom);
    char amb[256];
    CHECK(esc_carte_choisir(t, n, "SteppesofWar", amb, sizeof amb) == 1, "nom exact");
    CHECK(esc_carte_choisir(t, n, "steppes of war", amb, sizeof amb) == 1, "« steppes of war » (casse, espaces)");
    CHECK(esc_carte_choisir(t, n, "(2)SteppesofWar.SC2Map", amb, sizeof amb) == 1, "« (2)SteppesofWar.SC2Map »");
    CHECK(esc_carte_choisir(t, n, "3", amb, sizeof amb) == 2 && esc_carte_choisir(t, n, "7", amb, sizeof amb) == -1,
          "n° 3 → LostTemple ; n° 7 → introuvable");
    CHECK(esc_carte_choisir(t, n, "lost", amb, sizeof amb) == 2, "préfixe unique « lost »");
    CHECK(esc_carte_choisir(t, n, "metal", amb, sizeof amb) == 3, "préfixe unique « metal » (Novice… ne commence pas ainsi)");
    CHECK(esc_carte_choisir(t, n, "novice", amb, sizeof amb) == -2 && strstr(amb, "NoviceMetalopolis"),
          "« novice » ambigu (%s)", amb);
    CHECK(esc_carte_choisir(t, n, "gaton", amb, sizeof amb) == 4 + 1, "morceau unique « gaton » → Megaton");
    CHECK(esc_carte_choisir(t, n, "Kulas", amb, sizeof amb) == -1, "inconnue → -1");
}

static void env(const char *k, const char *v) { if (v) setenv(k, v, 1); else unsetenv(k); }

static void test_options(void)
{
    esc_options o;
    env("SC2_ESCARMOUCHE_RACE", "terran"); env("SC2_ESCARMOUCHE_RACE_IA", "zerg"); env("SC2_ESCARMOUCHE_IA", "facile");
    env("SC2_ESCARMOUCHE_NB_IA", NULL); env("SC2_ESCARMOUCHE_VISION", NULL); env("SC2_ESCARMOUCHE_FIN", "0");
    esc_options_lire(&o, 2);
    CHECK(o.race_j1 == 0 && !o.race_j1_alea && o.nb_ia == 1 && o.race_ia[0] == 1 && o.diff == 1 && !o.vision && !o.fin,
          "terran contre zerg facile, 1 IA, sans vision ni banques");
    env("SC2_ESCARMOUCHE_NB_IA", "5"); env("SC2_ESCARMOUCHE_RACE_IA", "protoss,terran"); env("SC2_ESCARMOUCHE_IA", "Démentiel");
    env("SC2_ESCARMOUCHE_VISION", "1"); env("SC2_ESCARMOUCHE_FIN", "1");
    esc_options_lire(&o, 4);
    CHECK(o.nb_ia == 3 && o.race_ia[0] == 2 && o.race_ia[1] == 0 && o.race_ia[2] == 0 && o.diff == 5 && o.vision && o.fin,
          "5 IA sur 4 places → 3 ; races protoss, terran, terran (dernière répétée) ; démentiel 5 ; vision, banques");
    env("SC2_ESCARMOUCHE_RACE", "orc"); env("SC2_ESCARMOUCHE_RACE_IA", NULL); env("SC2_ESCARMOUCHE_IA", "brutal");
    env("SC2_ESCARMOUCHE_NB_IA", "zero");
    esc_options_lire(&o, 2);
    CHECK(o.race_j1_alea && o.race_j1 >= 0 && o.race_j1 <= 2 && o.race_ia_alea[0] && o.race_ia[0] >= 0 && o.race_ia[0] <= 2
          && o.nb_ia == 1 && o.diff == 1, "valeurs inconnues → aléatoire tiré (%s, %s), 1 IA, facile",
          ESC_RACE_NOM[o.race_j1], ESC_RACE_NOM[o.race_ia[0]]);
    int vus[3] = { 0, 0, 0 };
    for (int i = 0; i < 300; i++) vus[esc_tirer_race()]++;
    CHECK(vus[0] > 50 && vus[1] > 50 && vus[2] > 50, "tirage aléatoire : les 3 races sortent (%d/%d/%d sur 300)", vus[0], vus[1], vus[2]);

    /* transformation complète sur un MapInfo synthétique et un script CRLF */
    uint8_t b[2048]; size_t debut;
    jsynth j2[] = { { 0, 3, 0, "", "" }, { 1, 1, -1, "", "" }, { 2, 1, -1, "", "" } };
    size_t n = synth(b, 0x18, 0, j2, 3, &debut);
    env("SC2_ESCARMOUCHE_RACE", "Terr"); env("SC2_ESCARMOUCHE_RACE_IA", "Zerg"); env("SC2_ESCARMOUCHE_IA", "1");
    env("SC2_ESCARMOUCHE_NB_IA", NULL); env("SC2_ESCARMOUCHE_VISION", NULL); env("SC2_ESCARMOUCHE_FIN", NULL);
    esc_options_lire(&o, 2);
    uint8_t *i2; char *s2; size_t ni2, ns2; int ids[ESC_IA_MAX]; char err[ESC_ERR];
    int r = esc_transformer(b, n, SCRIPT_CRLF, sizeof SCRIPT_CRLF - 1, &o, &i2, &ni2, &s2, &ns2, ids, err, sizeof err);
    esc_mapinfo mi;
    CHECK(r == 0 && esc_mapinfo_lire(i2, ni2, &mi, err, sizeof err) == 0 && mi.j[2].control == 2 && strstr(s2, "PlayerSetDifficulty(2, 1);\r\n")
          && !strstr(s2, "VisEnable") && !strstr(s2, "gt_EscFin"), "transformation : J2 Computer, PlayerSetDifficulty(2, 1), ni vision ni banques");
    char dsc[512];
    esc_decrire(dsc, sizeof dsc, &o, ids);
    CHECK(strstr(dsc, "J1 humain Terran, J2 IA Zerg ; difficulté Facile (c_skirEasy = 1)") != NULL, "journal : « %s »", dsc);
    free(i2); free(s2);
}

int main(void)
{
    test_noms();
    test_mapinfo();
    test_mapscript();
    test_cartes();
    test_options();
    printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
