/* test_mac_evenements.c — rejeu SC2_MAC_EVENEMENTS, cœur en C pur (mac/sc2_mac_evenements.c, V3 lot M4, 22/10), sur
 * l'hôte, sans AppKit :
 *   cc -std=gnu17 -O0 -Wall -Wextra -I mac mac/tests/test_mac_evenements.c mac/sc2_mac_evenements.c -lm -o /tmp/t && /tmp/t
 * (ou ctest dans build-tests : tests-hote/v3/evenements.cmake). Code de sortie 0 : tout conforme.
 *
 * A. Grammaire : chaque commande et directive, expansions (CLIC, TOUCHE, TEXTE, MODS), modificateurs tenus, VOIE, REF.
 * B. Erreurs nommées « source:ligne:colonne: message » (colonnes en caractères), plusieurs erreurs, texte et fichier.
 * C. Ordonnancement sur une horloge simulée (1 ms par pas, un Present toutes les 16 ms, géométrie à t = 2 s) : @p N,
 *    @p +N, +MS, +Ss, pas= de TEXTE, attente de la géométrie — ordre et instants EXACTS, échéances rendues.
 * D. ATTENDRE_LIGNE : fin par la ligne vue, délai dépassé (SC2EN_DELAI, définitif), échéance de scrutation.
 * E. Suivi du journal : lignes complètes seulement, curseur, départ à la fin, pread sans toucher le décalage d'écriture,
 *    ligne géante, refus d'un tube ; lignes du rejeu (« [MACEV] » en tête) sautées, protocole complet de l'hôte ; sortie
 *    standard en écriture seule (> et >>, dans un fils) rouverte en lecture, réouverture refusée.
 * F. Conversion j → points de vue (1280×720 dans 3200×1800 et 1280×1024 dans 2560×1440, échelle 2), bandes noires, f et
 *    v, aller-retour EXHAUSTIF à 1 pixel près (exact quand l'image est agrandie) ; REF : ui, scene, lin, brut.
 * G. Isolation : défaut, scénario, variable. H. Spécification : texte, « @p », fichier (BOM, CRLF). I. Journal. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sc2_mac_evenements.h"

static int g_ko, g_ok;
#define VERIF(c, ...) do { if (c) g_ok++; else { g_ko++; printf("  ÉCHEC : "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const sc2en_evt *ev(const sc2en_script *s, int ligne, int sous)
{
    for (int i = 0; i < s->n; i++)
        if (s->evt[i].ligne == ligne && s->evt[i].sous == sous) return &s->evt[i];
    return NULL;
}
static int analyser(const char *texte, sc2en_script *s, char *err, size_t cap)
{
    return sc2en_analyser(texte, strlen(texte), 0, "t", s, err, cap);
}

/* ==== A. grammaire ================================================================================================= */
static void test_grammaire(void)
{
    static const char *SC =
        "# scénario de test\n"                                          /* 1 */
        "REF 1280x720\n"                                                /* 2 */
        "VOIE directe\n"                                                /* 3 */
        "ISOLE 1\n"                                                     /* 4 */
        "MOVE j 640 360   # commentaire\n"                              /* 5 */
        "@p 10 DOWN g j 100 200 mods=shift\n"                           /* 6 */
        "UP g j 100 200\n"                                              /* 7 */
        "+250 CLIC d v 10.5 20.25 mods=rcmd,ctrl\n"                     /* 8 */
        "DRAG m f 0 0.5\n"                                              /* 9 */
        "MOLETTE -3 0.5 precis\n"                                       /* 10 */
        "TOUCHE 0x00 \"a\" mods=cmd rep\n"                              /* 11 */
        "TOUCHE_BAS 0x6D \"\\uF70D\" mods=fn\n"                         /* 12 */
        "TOUCHE_HAUT 0x6D\n"                                            /* 13 */
        "MODS ctrl\n"                                                   /* 14 */
        "TOUCHE 1 \"s\" mods=shift\n"                                   /* 15 */
        "MODS -\n"                                                      /* 16 */
        "MODS -\n"                                                      /* 17 */
        "@p +2 TEXTE \"Hi!\\n\" pas=30\n"                               /* 18 */
        "FOCUS 0\n"                                                     /* 19 */
        "OCCULTE 1\n"                                                   /* 20 */
        "MINIATURISER\n"                                                /* 21 */
        "RESTAURER\n"                                                   /* 22 */
        "PLEIN_ECRAN\n"                                                 /* 23 */
        "plein_ecran 1\n"                                               /* 24 */
        "TAILLE 1600 1000\n"                                            /* 25 */
        "TAILLE 800x600\n"                                              /* 26 */
        "FERMER\n"                                                      /* 27 */
        "SIGNAL TERM\n"                                                 /* 28 */
        "ATTENDRE_LIGNE \"1re frame présentée\" 5000\n"                 /* 29 */
        "JOURNAL \"témoin ; « ok » \\\"q\\\"\"\n"                       /* 30 */
        "VOIE file\n"                                                   /* 31 */
        "@p +3 QUITTER\n"                                               /* 32 */
        "CLIC g j 725 627 @scene\n"                                     /* 33 */
        "REF 1280x720 lin\n"                                            /* 34 */
        "MOVE j 1 2\n"                                                  /* 35 */
        "MOVE j 1 2 @brut\n"                                            /* 36 */
        "REF -\n"                                                       /* 37 */
        "MOVE j 5 6\n"                                                  /* 38 */
        "SIGNAL int\n"                                                  /* 39 */
        "ATTENDRE_LIGNE \"x\"\n"                                        /* 40 */
        "+1.5s MOLETTE 2\n"                                             /* 41 */
        "+20ms MOLETTE 1 precis\n";                                     /* 42 */
    sc2en_script s;
    char err[2048];
    int r = analyser(SC, &s, err, sizeof err);
    VERIF(r == 0, "grammaire : %d erreur(s) :\n%s", r, err);
    VERIF(s.lignes == 42, "grammaire : %d lignes lues (42)", s.lignes);
    VERIF(s.isole == 1, "ISOLE 1 lu (%d)", s.isole);
    const sc2en_evt *e;
    e = ev(&s, 5, 0);
    VERIF(e && e->type == SC2EN_DEPLACER && e->pos.repere == 'j' && e->pos.x == 640 && e->pos.y == 360 && e->pos.ref_w == 1280
          && e->pos.ref_h == 720 && e->pos.ancre == SC2EN_ANCRE_UI && e->quand.type == SC2EN_TOUT_DE_SUITE
          && e->voie == SC2EN_VOIE_DIRECTE && e->colonne == 1 && !strcmp(e->commande, "MOVE"), "l.5 MOVE j 640 360");
    e = ev(&s, 6, 0);
    VERIF(e && e->type == SC2EN_BAS && e->bouton == 0 && e->quand.type == SC2EN_AU_PRESENT && e->quand.v == 10
          && e->drapeaux == (SC2EN_MOD_LSHIFT | SC2EN_MOD_SHIFT) && e->colonne == 7, "l.6 @p 10 DOWN g mods=shift (col %d)",
          e ? e->colonne : -1);
    e = ev(&s, 7, 0);
    VERIF(e && e->type == SC2EN_HAUT && e->bouton == 0 && e->drapeaux == 0, "l.7 UP g sans modificateur");
    e = ev(&s, 8, 0);
    const sc2en_evt *e2 = ev(&s, 8, 1);
    VERIF(e && e2 && e->type == SC2EN_BAS && e2->type == SC2EN_HAUT && e->bouton == 1 && e2->bouton == 1
          && e->quand.type == SC2EN_MS_APRES && e->quand.v == 250 && e2->quand.type == SC2EN_TOUT_DE_SUITE
          && e->pos.repere == 'v' && e->pos.x == 10.5 && e->pos.y == 20.25 && e->pos.ancre == SC2EN_ANCRE_BRUT
          && e->drapeaux == (SC2EN_MOD_RCMD | SC2EN_MOD_LCTRL | SC2EN_MOD_CMD | SC2EN_MOD_CTRL) && e2->drapeaux == e->drapeaux
          && !ev(&s, 8, 2), "l.8 +250 CLIC d v = DOWN (condition) + UP (tout de suite), rcmd+ctrl");
    e = ev(&s, 9, 0);
    VERIF(e && e->type == SC2EN_GLISSER && e->bouton == 2 && e->pos.repere == 'f' && e->pos.x == 0 && e->pos.y == 0.5,
          "l.9 DRAG m f 0 0.5");
    e = ev(&s, 10, 0);
    VERIF(e && e->type == SC2EN_MOLETTE && e->dy == -3 && e->dx == 0.5 && e->precis == 1, "l.10 MOLETTE -3 0.5 precis");
    e = ev(&s, 11, 0);
    e2 = ev(&s, 11, 1);
    VERIF(e && e2 && e->type == SC2EN_TOUCHE_BAS && e2->type == SC2EN_TOUCHE_HAUT && e->kvk == 0 && e2->kvk == 0
          && !strcmp(e->texte, "a") && !strcmp(e2->texte, "a") && e->repetition == 1 && e2->repetition == 0
          && e->drapeaux == (SC2EN_MOD_LCMD | SC2EN_MOD_CMD), "l.11 TOUCHE 0x00 \"a\" mods=cmd rep");
    e = ev(&s, 12, 0);
    VERIF(e && e->type == SC2EN_TOUCHE_BAS && e->kvk == 0x6D && !strcmp(e->texte, "\xEF\x9C\x8D") && e->drapeaux == SC2EN_MOD_FN
          && !ev(&s, 12, 1), "l.12 TOUCHE_BAS 0x6D \"\\uF70D\" mods=fn (UTF-8 EF 9C 8D)");
    e = ev(&s, 13, 0);
    VERIF(e && e->type == SC2EN_TOUCHE_HAUT && e->kvk == 0x6D && !e->texte[0], "l.13 TOUCHE_HAUT 0x6D, characters vide");
    e = ev(&s, 14, 0);
    VERIF(e && e->type == SC2EN_MODIFICATEURS && e->kvk == 0x3B && e->drapeaux == (SC2EN_MOD_LCTRL | SC2EN_MOD_CTRL),
          "l.14 MODS ctrl → flagsChanged 0x3B");
    e = ev(&s, 15, 0);
    VERIF(e && e->kvk == 1 && e->drapeaux == (SC2EN_MOD_LCTRL | SC2EN_MOD_LSHIFT | SC2EN_MOD_CTRL | SC2EN_MOD_SHIFT),
          "l.15 TOUCHE sous MODS ctrl : tenus + mods= (0x%X)", e ? e->drapeaux : 0);
    e = ev(&s, 16, 0);
    VERIF(e && e->type == SC2EN_MODIFICATEURS && e->kvk == 0x3B && e->drapeaux == 0 && !ev(&s, 16, 1), "l.16 MODS - → relâche ctrl");
    e = ev(&s, 17, 0);
    VERIF(e && e->type == SC2EN_JALON && !ev(&s, 17, 1), "l.17 MODS - sans changement → jalon");
    /* l.18 : @p +2 TEXTE "Hi!\n" pas=30 → H (shift), i, ! (shift), Retour */
    static const struct { uint16_t kvk; int maj; const char *car; } T[] = { { 0x04, 1, "H" }, { 0x22, 0, "i" },
                                                                            { 0x12, 1, "!" }, { 0x24, 0, "\r" } };
    int texte_ok = 1;
    for (int k = 0; k < 4; k++) {
        const sc2en_evt *b = ev(&s, 18, 2 * k), *h = ev(&s, 18, 2 * k + 1);
        uint32_t d = T[k].maj ? SC2EN_MOD_LSHIFT | SC2EN_MOD_SHIFT : 0;
        int cond = k == 0 ? b && b->quand.type == SC2EN_PRESENTS_APRES && b->quand.v == 2
                          : b && b->quand.type == SC2EN_MS_APRES && b->quand.v == 30;
        if (!b || !h || b->type != SC2EN_TOUCHE_BAS || h->type != SC2EN_TOUCHE_HAUT || b->kvk != T[k].kvk
            || h->kvk != T[k].kvk || b->drapeaux != d || strcmp(b->texte, T[k].car) || strcmp(h->texte, T[k].car) || !cond
            || h->quand.type != SC2EN_TOUT_DE_SUITE)
            texte_ok = 0;
    }
    VERIF(texte_ok && !ev(&s, 18, 8), "l.18 TEXTE \"Hi!\\n\" pas=30 : 8 primitives, kVK US, Maj, condition puis +30 ms");
    VERIF((e = ev(&s, 19, 0)) && e->type == SC2EN_FOCUS && e->valeur == 0, "l.19 FOCUS 0");
    VERIF((e = ev(&s, 20, 0)) && e->type == SC2EN_OCCULTE && e->valeur == 1, "l.20 OCCULTE 1");
    VERIF((e = ev(&s, 21, 0)) && e->type == SC2EN_MINIATURISER, "l.21 MINIATURISER");
    VERIF((e = ev(&s, 22, 0)) && e->type == SC2EN_RESTAURER, "l.22 RESTAURER");
    VERIF((e = ev(&s, 23, 0)) && e->type == SC2EN_PLEIN_ECRAN && e->valeur == -1, "l.23 PLEIN_ECRAN (bascule)");
    VERIF((e = ev(&s, 24, 0)) && e->type == SC2EN_PLEIN_ECRAN && e->valeur == 1, "l.24 plein_ecran 1 (sans casse)");
    VERIF((e = ev(&s, 25, 0)) && e->type == SC2EN_TAILLE && e->l == 1600 && e->h == 1000, "l.25 TAILLE 1600 1000");
    VERIF((e = ev(&s, 26, 0)) && e->type == SC2EN_TAILLE && e->l == 800 && e->h == 600, "l.26 TAILLE 800x600");
    VERIF((e = ev(&s, 27, 0)) && e->type == SC2EN_FERMER, "l.27 FERMER");
    VERIF((e = ev(&s, 28, 0)) && e->type == SC2EN_SIGNAL && e->valeur == SIGTERM, "l.28 SIGNAL TERM");
    VERIF((e = ev(&s, 29, 0)) && e->type == SC2EN_ATTENDRE_LIGNE && e->delai_ms == 5000 && !strcmp(e->texte, "1re frame présentée"),
          "l.29 ATTENDRE_LIGNE motif 5000");
    VERIF((e = ev(&s, 30, 0)) && e->type == SC2EN_JOURNAL && !strcmp(e->texte, "témoin ; « ok » \"q\""), "l.30 JOURNAL (« ; », échappements)");
    VERIF((e = ev(&s, 32, 0)) && e->type == SC2EN_QUITTER && e->voie == SC2EN_VOIE_FILE && e->quand.type == SC2EN_PRESENTS_APRES
          && e->quand.v == 3, "l.32 @p +3 QUITTER en VOIE file");
    VERIF((e = ev(&s, 33, 0)) && e->pos.ancre == SC2EN_ANCRE_SCENE && e->voie == SC2EN_VOIE_FILE && ev(&s, 33, 1), "l.33 CLIC @scene");
    VERIF((e = ev(&s, 35, 0)) && e->pos.ancre == SC2EN_ANCRE_LIN && e->pos.ref_w == 1280, "l.35 REF … lin : ancre lin");
    VERIF((e = ev(&s, 36, 0)) && e->pos.ancre == SC2EN_ANCRE_BRUT && e->pos.ref_w == 1280, "l.36 @brut");
    VERIF((e = ev(&s, 38, 0)) && e->pos.ancre == SC2EN_ANCRE_BRUT && e->pos.ref_w == 0, "l.38 après REF - : pixels bruts");
    VERIF((e = ev(&s, 39, 0)) && e->valeur == SIGINT, "l.39 SIGNAL int");
    VERIF((e = ev(&s, 40, 0)) && e->delai_ms == SC2EN_DELAI_DEFAUT_MS, "l.40 ATTENDRE_LIGNE : délai par défaut");
    VERIF((e = ev(&s, 41, 0)) && e->quand.type == SC2EN_MS_APRES && e->quand.v == 1500, "l.41 +1.5s = 1500 ms");
    VERIF((e = ev(&s, 42, 0)) && e->quand.type == SC2EN_MS_APRES && e->quand.v == 20 && e->precis && e->dx == 0,
          "l.42 +20ms MOLETTE 1 precis");
    VERIF(s.n_file == 10 && s.n_j == 8, "comptes : VOIE file %d (10 : l.32 à 42), j %d (8)", s.n_file, s.n_j);
    sc2en_liberer(&s);

    /* MODS : côtés, ordre (relâchées d'abord), keyCode, drapeaux d'après */
    r = analyser("MODS cmd,shift\nMODS rshift\nMODS -\nMODS caps,fn,ralt,rctrl\n", &s, err, sizeof err);
    VERIF(r == 0, "MODS : %s", err);
    const struct { int l, k; uint16_t kvk; uint32_t d; } M[] = {
        { 1, 0, 0x37, SC2EN_MOD_LCMD | SC2EN_MOD_CMD },
        { 1, 1, 0x38, SC2EN_MOD_LCMD | SC2EN_MOD_LSHIFT | SC2EN_MOD_CMD | SC2EN_MOD_SHIFT },
        { 2, 0, 0x37, SC2EN_MOD_LSHIFT | SC2EN_MOD_SHIFT },
        { 2, 1, 0x38, 0 },
        { 2, 2, 0x3C, SC2EN_MOD_RSHIFT | SC2EN_MOD_SHIFT },
        { 3, 0, 0x3C, 0 },
        { 4, 0, 0x3D, SC2EN_MOD_RALT | SC2EN_MOD_ALT },
        { 4, 1, 0x3E, SC2EN_MOD_RALT | SC2EN_MOD_RCTRL | SC2EN_MOD_ALT | SC2EN_MOD_CTRL },
        { 4, 2, 0x39, SC2EN_MOD_RALT | SC2EN_MOD_RCTRL | SC2EN_MOD_ALT | SC2EN_MOD_CTRL | SC2EN_MOD_CAPS },
        { 4, 3, 0x3F, SC2EN_MOD_RALT | SC2EN_MOD_RCTRL | SC2EN_MOD_ALT | SC2EN_MOD_CTRL | SC2EN_MOD_CAPS | SC2EN_MOD_FN },
    };
    for (unsigned i = 0; i < sizeof M / sizeof M[0]; i++) {
        e = ev(&s, M[i].l, M[i].k);
        VERIF(e && e->type == SC2EN_MODIFICATEURS && e->kvk == M[i].kvk && e->drapeaux == M[i].d,
              "MODS l.%d n°%d : kvk 0x%X drapeaux 0x%X (attendu 0x%X 0x%X)", M[i].l, M[i].k, e ? e->kvk : 0, e ? e->drapeaux : 0,
              M[i].kvk, M[i].d);
    }
    VERIF(s.n == 10, "MODS : %d primitives (10)", s.n);
    sc2en_liberer(&s);
}

/* ==== B. erreurs nommées =========================================================================================== */
static void test_erreurs(void)
{
    static const struct { const char *texte; const char *attendu; } E[] = {
        { "FOO 1", "t:1:1: commande inconnue « FOO »" },
        { "\n\nCLIC x j 1 2", "t:3:6: CLIC : bouton « x » inconnu" },
        { "MOVE j 1", "t:1:9: MOVE : « j » : deux nombres attendus" },
        { "TOUCHE 0x80", "t:1:8: TOUCHE : kvk dans la plage attendu" },
        { "TEXTE \"abc", "t:1:7: guillemet non fermé" },
        { "REF 1280", "t:1:5: REF : LxH attendu" },
        { "REF 100x720", "t:1:5: REF : LxH attendu (320×200 à 8192×8192)" },
        { "ISOLE 2", "t:1:7: ISOLE : 0 ou 1 attendu" },
        { "@p 5 REF 1280x720", "t:1:1: condition sans objet devant la directive REF" },
        { "QUITTER maintenant", "t:1:9: QUITTER : argument en trop « maintenant »" },
        { "MOVE f 1.5 0", "t:1:8: MOVE : f : fraction hors de [0, 1]" },
        { "JOURNAL \"é\" bof", "t:1:13: JOURNAL : argument en trop « bof »" },            /* colonne en caractères */
        { "  \tJOURNAL \"àéî\" x", "t:1:18: JOURNAL : argument en trop « x »" },
        { "@p", "t:1:3: « @p » : numéro de Present attendu" },
        { "@p x MOVE v 1 1", "t:1:4: « @p » : numéro de Present attendu" },
        { "+abc QUITTER", "t:1:1: délai « +abc » invalide" },
        { "@p 12 ", "t:1:7: commande attendue après la condition" },
        { "DOWN g j 1 2 mods=cmd,hyper", "t:1:23: modificateur « hyper » inconnu" },
        { "MOVE j 1 2 @ui", "t:1:12: MOVE : ancre « @ui » sans REF" },
        { "REF 1280x720\nMOVE v 1 2 @ui", "t:2:12: MOVE : ancre « @ui » sans objet hors d'une coordonnée j" },
        { "REF 1280x720\nMOVE j 1 2 @haut", "t:2:12: MOVE : ancre « @haut » inconnue" },
        { "TEXTE \"aé\"", "t:1:7: TEXTE : caractère n°2 « é » sans touche US" },
        { "ISOLE 1\n# c\nISOLE 0", "t:3:1: ISOLE déjà donné ligne 1" },
        { "TOUCHE 1 \"a\\q\"", "t:1:12: échappement inconnu" },
        { "TOUCHE 1 \"\\u12\"", "t:1:11: échappement \\u : 4 chiffres hexadécimaux attendus" },
        { "TOUCHE 1 \"\\uD800\"", "t:1:11: échappement : point de code U+D800 invalide" },
        { "TOUCHE_HAUT 0x00 rep", "t:1:18: TOUCHE_HAUT : argument « rep » inconnu ou en trop" },
        { "TAILLE 50 600", "t:1:8: TAILLE : W attendu (100 à 16384 points)" },
        { "SIGNAL KILL", "t:1:8: SIGNAL : « KILL » inconnu (TERM ou INT)" },
        { "ATTENDRE_LIGNE \"\"", "t:1:16: ATTENDRE_LIGNE : motif non vide entre guillemets attendu" },
        { "ATTENDRE_LIGNE \"x\" 0", "t:1:20: ATTENDRE_LIGNE : délai en ms attendu" },
        { "VOIE appkit", "t:1:6: VOIE : « appkit » inconnue (directe ou file)" },
        { "MOVE x 1 2", "t:1:6: MOVE : repère « x » inconnu (j, v ou f)" },
        { "MOVE j 1.5 2", "t:1:8: MOVE : j : pixel client entier attendu" },
        { "MOLETTE haut", "t:1:9: MOLETTE : DY attendu" },
        { "TEXTE \"ab\" pas=x", "t:1:12: TEXTE : « pas=MS » attendu" },
        { "JOURNAL \"a\"b", "t:1:12: blanc attendu après la chaîne" },
        { "MOVE v 1\"2 3", "t:1:9: guillemet au milieu d'un mot" },
        { "FOCUS", "t:1:6: FOCUS : 0 ou 1 attendu" },
        { "MODS", "t:1:5: MODS : liste attendue" },
        { "+25h QUITTER", "t:1:1: délai « +25h » invalide" },
        { "+90000s QUITTER", "t:1:1: délai « +90000s » invalide (+MS, +MSms ou +Ss, 24 h au plus)" },
    };
    for (unsigned i = 0; i < sizeof E / sizeof E[0]; i++) {
        sc2en_script s;
        char err[1024];
        int r = analyser(E[i].texte, &s, err, sizeof err);
        VERIF(r == 1 && strstr(err, E[i].attendu) == err, "erreur n°%u « %s » : %d erreur(s), obtenu « %s », attendu « %s… »", i,
              E[i].texte, r, err, E[i].attendu);
        sc2en_liberer(&s);
    }
    /* plusieurs erreurs : toutes nommées, une ligne chacune ; les lignes valides n'empêchent rien */
    sc2en_script s;
    char err[1024];
    int r = analyser("FOO\nMOVE v 1 2\nBAR baz\nMOVE j\n", &s, err, sizeof err);
    VERIF(r == 3 && strstr(err, "t:1:1: commande inconnue « FOO »\n") && strstr(err, "t:3:1: commande inconnue « BAR »\n")
          && strstr(err, "t:4:7: MOVE : « j » : deux nombres attendus\n"), "trois erreurs : %d, « %s »", r, err);
    sc2en_liberer(&s);
    /* tampon d'erreurs minuscule : tronqué, jamais débordé, compte juste */
    char petit[8];
    memset(petit, 'Z', sizeof petit);
    r = sc2en_analyser("FOO\nBAR\n", 8, 0, "t", &s, petit, 5);
    VERIF(r == 2 && petit[4] == 0 && petit[5] == 'Z', "tampon d'erreurs de 5 octets : %d erreurs, « %.5s »", r, petit);
    sc2en_liberer(&s);
    /* texte : séparateur « ; », source nommée par l'appelant */
    r = sc2en_charger("MOVE v 1 2;FOO", "VAR", &s, err, sizeof err);
    VERIF(r == 1 && strstr(err, "VAR:2:1: commande inconnue « FOO »") == err, "texte : « %s »", err);
    sc2en_liberer(&s);
    /* fichier absent */
    r = sc2en_charger("@/inexistant/sc2-macev.txt", "VAR", &s, err, sizeof err);
    VERIF(r == 1 && strstr(err, "/inexistant/sc2-macev.txt:0:0: scénario illisible") == err, "fichier absent : « %s »", err);
    sc2en_liberer(&s);
}

/* ==== C. ordonnancement ============================================================================================ */
typedef struct { int ligne, sous; double t; unsigned p; } trace;
static double g_maintenant;                 /* horloge simulée, lue par vue_factice */

/* horloge simulée : pas de 1 ms, Present n° floor(t / 16), géométrie connue dès t_geo ; trace des exécutions */
static int simuler(const sc2en_script *s, double t_fin, double t_geo, trace *tr, int max, int (*vue)(void *, const char *),
                   int *code, double *ech_a, double t_ech)
{
    sc2en_rejeu r;
    sc2en_instant t0 = { 0, 0, 0 };
    int n = 0;
    sc2en_demarrer(&r, s, t0, vue, NULL);
    *code = SC2EN_RIEN;
    for (double t = 0; t <= t_fin; t += 1) {
        sc2en_instant i = { t, (unsigned)(t / 16), t >= t_geo };
        g_maintenant = t;
        for (;;) {
            const sc2en_evt *e;
            double ech;
            int k = sc2en_avancer(&r, i, &e, &ech);
            if (k == SC2EN_EXECUTER) {
                if (n < max) tr[n++] = (trace){ e->ligne, e->sous, t, i.presents };
                continue;
            }
            if (k == SC2EN_RIEN && ech_a && t == t_ech) *ech_a = ech;
            if (k != SC2EN_RIEN) { *code = k; return n; }
            break;
        }
    }
    return n;
}

static void test_ordonnancement(void)
{
    static const char *SC =
        "JOURNAL \"a\"\n"          /* 1 : t 0 */
        "+100 JOURNAL \"b\"\n"     /* 2 : t 100 */
        "@p 10 JOURNAL \"c\"\n"    /* 3 : Present 10 = t 160 */
        "@p +5 JOURNAL \"d\"\n"    /* 4 : Present 15 = t 240 */
        "+50 JOURNAL \"e\"\n"      /* 5 : t 290 */
        "@p 3 JOURNAL \"f\"\n"     /* 6 : déjà passé : t 290, même tour */
        "CLIC g v 1 1\n"           /* 7 : DOWN, UP à t 290 */
        "+1.5s JOURNAL \"g\"\n"    /* 8 : t 1790 */
        "+20ms MOVE j 1 1\n"       /* 9 : échu à 1810, géométrie à 2000 → t 2000 */
        "+30 JOURNAL \"h\"\n"      /* 10 : 30 ms après l'exécution RÉELLE de la 9 (2000), pas après son échéance → 2030 */
        "TEXTE \"ab\"\n"           /* 11 : a à 2030, b au Present suivant (127 = t 2032) */
        "TEXTE \"cd\" pas=40\n";   /* 12 : c à 2032, d à 2072 */
    static const trace A[] = {
        { 1, 0, 0, 0 }, { 2, 0, 100, 6 }, { 3, 0, 160, 10 }, { 4, 0, 240, 15 }, { 5, 0, 290, 18 }, { 6, 0, 290, 18 },
        { 7, 0, 290, 18 }, { 7, 1, 290, 18 }, { 8, 0, 1790, 111 }, { 9, 0, 2000, 125 }, { 10, 0, 2030, 126 },
        { 11, 0, 2030, 126 }, { 11, 1, 2030, 126 }, { 11, 2, 2032, 127 }, { 11, 3, 2032, 127 },
        { 12, 0, 2032, 127 }, { 12, 1, 2032, 127 }, { 12, 2, 2072, 129 }, { 12, 3, 2072, 129 },
    };
    sc2en_script s;
    char err[512];
    int r = analyser(SC, &s, err, sizeof err);
    VERIF(r == 0, "ordonnancement : %s", err);
    trace tr[64];
    int code;
    double ech = -2;
    int n = simuler(&s, 3000, 2000, tr, 64, NULL, &code, &ech, 241);
    int na = (int)(sizeof A / sizeof A[0]);
    VERIF(n == na && code == SC2EN_FINI, "ordonnancement : %d exécutions (%d), code %d (FINI)", n, na, code);
    for (int i = 0; i < n && i < na; i++)
        VERIF(tr[i].ligne == A[i].ligne && tr[i].sous == A[i].sous && tr[i].t == A[i].t && tr[i].p == A[i].p,
              "exécution n°%d : l.%d/%d à t=%.0f Present %u, attendu l.%d/%d à t=%.0f Present %u", i, tr[i].ligne, tr[i].sous,
              tr[i].t, tr[i].p, A[i].ligne, A[i].sous, A[i].t, A[i].p);
    VERIF(ech == 290, "échéance rendue à t=241 (l.5 : +50 après 240) : %.0f (290)", ech);
    /* échéance -1 quand on attend un Present ou la géométrie */
    sc2en_rejeu rj;
    sc2en_instant t0 = { 0, 0, 0 };
    const sc2en_evt *e;
    sc2en_demarrer(&rj, &s, t0, NULL, NULL);
    sc2en_avancer(&rj, t0, &e, &ech);                               /* l.1 */
    sc2en_avancer(&rj, (sc2en_instant){ 100, 6, 0 }, &e, &ech);      /* l.2 */
    int k = sc2en_avancer(&rj, (sc2en_instant){ 101, 6, 0 }, &e, &ech);
    VERIF(k == SC2EN_RIEN && ech == -1 && !e, "attente d'un Present : RIEN, échéance -1 (%d, %.0f)", k, ech);
    sc2en_liberer(&s);
    /* un scénario vide finit tout de suite ; FINI reste */
    r = analyser("# rien\n\n", &s, err, sizeof err);
    sc2en_demarrer(&rj, &s, t0, NULL, NULL);
    k = sc2en_avancer(&rj, t0, &e, &ech);
    int k2 = sc2en_avancer(&rj, t0, &e, &ech);
    VERIF(r == 0 && s.n == 0 && s.lignes == 2 && k == SC2EN_FINI && k2 == SC2EN_FINI, "scénario vide : FINI (%d, %d)", k, k2);
    sc2en_liberer(&s);
}

/* ==== D. ATTENDRE_LIGNE ============================================================================================ */
static int vue_factice(void *ctx, const char *motif)
{
    (void)ctx;
    return !strcmp(motif, "pret") && g_maintenant >= 300;
}

static void test_attendre(void)
{
    static const char *SC =
        "JOURNAL \"x\"\n"                      /* 1 : t 0 */
        "ATTENDRE_LIGNE \"pret\" 1000\n"       /* 2 : début t 0, ligne vue à t 300 */
        "+10 JOURNAL \"y\"\n"                  /* 3 : t 310 */
        "ATTENDRE_LIGNE \"jamais\" 200\n"      /* 4 : début t 310, délai à 510 */
        "JOURNAL \"z\"\n";                     /* 5 : jamais */
    sc2en_script s;
    char err[512];
    int r = analyser(SC, &s, err, sizeof err);
    VERIF(r == 0, "ATTENDRE_LIGNE : %s", err);
    trace tr[16];
    int code;
    double ech = -2;
    int n = simuler(&s, 2000, 0, tr, 16, vue_factice, &code, &ech, 100);
    VERIF(n == 4 && code == SC2EN_DELAI, "ATTENDRE_LIGNE : %d exécutions (4), code %d (DELAI)", n, code);
    static const trace A[] = { { 1, 0, 0, 0 }, { 2, 0, 0, 0 }, { 3, 0, 310, 19 }, { 4, 0, 310, 19 } };
    for (int i = 0; i < n && i < 4; i++)
        VERIF(tr[i].ligne == A[i].ligne && tr[i].t == A[i].t, "ATTENDRE_LIGNE n°%d : l.%d à t=%.0f (l.%d à t=%.0f)", i, tr[i].ligne,
              tr[i].t, A[i].ligne, A[i].t);
    VERIF(ech == 150, "échéance de scrutation pendant l'attente (t=100) : %.0f (150)", ech);
    /* instant exact du délai, puis DELAI définitif avec la primitive en cause */
    sc2en_rejeu rj;
    const sc2en_evt *e;
    sc2en_demarrer(&rj, &s, (sc2en_instant){ 0, 0, 1 }, NULL, NULL);   /* sans suivi : la ligne n'est jamais vue */
    int k1 = sc2en_avancer(&rj, (sc2en_instant){ 0, 0, 1 }, &e, &ech);    /* l.1 */
    int k2 = sc2en_avancer(&rj, (sc2en_instant){ 0, 0, 1 }, &e, &ech);    /* l.2 : début */
    int k3 = sc2en_avancer(&rj, (sc2en_instant){ 999, 62, 1 }, &e, &ech);
    double ech3 = ech;
    int k4 = sc2en_avancer(&rj, (sc2en_instant){ 1000, 62, 1 }, &e, &ech);
    const sc2en_evt *e4 = e;
    int k5 = sc2en_avancer(&rj, (sc2en_instant){ 5000, 300, 1 }, &e, &ech);
    VERIF(k1 == SC2EN_EXECUTER && k2 == SC2EN_EXECUTER && k3 == SC2EN_RIEN && ech3 == 1000 && k4 == SC2EN_DELAI && e4
          && e4->ligne == 2 && k5 == SC2EN_DELAI && e && e->ligne == 2,
          "délai exact : %d %d %d (échéance %.0f) %d %d", k1, k2, k3, ech3, k4, k5);
    sc2en_liberer(&s);
}

/* ==== E. suivi du journal ========================================================================================== */
static void ecrire(int fd, const char *s)
{
    size_t n = strlen(s);
    while (n) {
        ssize_t k = write(fd, s, n);
        if (k <= 0) { perror("write"); exit(2); }
        s += k;
        n -= (size_t)k;
    }
}

/* E2. Les lignes du rejeu lui-même (« [MACEV] » en tête, la trace que l'hôte écrit à chaque EXECUTER) ne satisfont pas
 * ATTENDRE_LIGNE : la trace d'un ATTENDRE_LIGNE recopie son motif. Revue M4, sonde automatch.c. */
static void ecrire_geante(int fd, const char *tete, size_t nx, const char *queue)
{
    char *g = malloc(nx + 1);
    memset(g, 'x', nx);
    g[nx] = 0;
    ecrire(fd, tete);
    ecrire(fd, g);
    ecrire(fd, queue);
    free(g);
}

static void suivi_lignes_du_rejeu(const char *dir)
{
    char chemin[600], o[1024], l[1100], err[512];
    snprintf(chemin, sizeof chemin, "%s/rejeu.log", dir);
    int fd = open(chemin, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open"); g_ko++; return; }
    const char *M = "1re frame présentée";
    sc2en_script s;
    int r = analyser("ATTENDRE_LIGNE \"1re frame présentée\" 5000\nQUITTER\n", &s, err, sizeof err);
    VERIF(r == 0 && s.n == 2, "rejeu : analyse (%d : %s)", r, err);
    if (r || s.n != 2) { sc2en_liberer(&s); close(fd); unlink(chemin); return; }
    sc2en_suivi j;
    VERIF(sc2en_suivi_ouvrir(&j, chemin, 0) == 0, "rejeu : ouverture (%s)", strerror(j.erreur));
    /* 1. la trace de l'ATTENDRE_LIGNE, telle que l'hôte la journalise, puis la vraie ligne */
    sc2en_decrire(&s.evt[0], NULL, 0, 0, o, sizeof o);
    snprintf(l, sizeof l, "[MACEV] %s\n", o);
    VERIF(strstr(l, M) != NULL, "rejeu : la trace recopie le motif (%s)", l);
    ecrire(fd, l);
    VERIF(sc2en_suivi_chercher(&j, M) == 0, "rejeu : sa propre trace ne compte pas");
    ecrire(fd, "[VKP] 1re frame présentée\n");
    VERIF(sc2en_suivi_chercher(&j, M) == 1, "rejeu : la vraie ligne compte");
    /* 2. « [MACEV] » ailleurs qu'en tête de ligne : la ligne compte */
    ecrire(fd, "jeu : [MACEV] 1re frame présentée\n");
    VERIF(sc2en_suivi_chercher(&j, M) == 1, "rejeu : [MACEV] hors de la tête ne fait pas sauter la ligne");
    /* 3. ligne géante du rejeu, écrite en deux fois (l'état « milieu » survit entre deux appels), puis une vraie ligne */
    ecrire_geante(fd, "[MACEV] ", 40000, "");
    VERIF(sc2en_suivi_chercher(&j, M) == 0, "rejeu : ligne géante du rejeu entamée → 0");
    ecrire(fd, " 1re frame présentée\n");
    VERIF(sc2en_suivi_chercher(&j, M) == 0 && j.pos == lseek(fd, 0, SEEK_END),
          "rejeu : ligne géante du rejeu terminée, sautée (pos %lld)", j.pos);
    ecrire(fd, "vraie : 1re frame présentée\n");
    VERIF(sc2en_suivi_chercher(&j, M) == 1, "rejeu : vraie ligne après la ligne géante du rejeu");
    /* 4. ligne géante ordinaire qui contient deux fois le motif : elle ne sert qu'une fois */
    ecrire_geante(fd, "", 40000, "GEANT");
    ecrire_geante(fd, "", 40000, "GEANT\n");
    VERIF(sc2en_suivi_chercher(&j, "GEANT") == 1, "rejeu : ligne géante, 1re occurrence");
    VERIF(sc2en_suivi_chercher(&j, "GEANT") == 0, "rejeu : ligne géante, la 2e occurrence de la même ligne ne sert pas");
    ecrire(fd, "GEANT\n");
    VERIF(sc2en_suivi_chercher(&j, "GEANT") == 1, "rejeu : ligne suivante vue");
    sc2en_suivi_fermer(&j);
    /* 5. protocole complet de l'en-tête : « [MACEV] » + sc2en_decrire à chaque EXECUTER, dans le journal suivi */
    sc2en_suivi js;
    VERIF(sc2en_suivi_ouvrir(&js, chemin, -1) == 0, "rejeu : ouverture à la fin");
    sc2en_rejeu rj;
    sc2en_instant t = { 0, 0, 1 };
    sc2en_demarrer(&rj, &s, t, sc2en_suivi_rappel, &js);
    char seq[256] = "";
    for (int pas = 0; pas < 12; pas++) {
        const sc2en_evt *e;
        double ech;
        int k = sc2en_avancer(&rj, t, &e, &ech);
        size_t q = strlen(seq);
        snprintf(seq + q, sizeof seq - q, "%s%.0f:%d%s", q ? " " : "", t.t_ms, k, k == SC2EN_EXECUTER ? e->commande : "");
        if (k == SC2EN_FINI || k == SC2EN_DELAI) break;
        if (k == SC2EN_EXECUTER) {
            sc2en_decrire(e, NULL, t.presents, t.t_ms / 1000, o, sizeof o);
            snprintf(l, sizeof l, "[MACEV] %s\n", o);
            ecrire(fd, l);
            continue;
        }
        t.t_ms += 50;
        if (t.t_ms == 200) ecrire(fd, "[VKP] 1re frame présentée\n");
    }
    VERIF(!strcmp(seq, "0:1ATTENDRE_LIGNE 0:0 50:0 100:0 150:0 200:1QUITTER 200:2"),
          "rejeu : ATTENDRE_LIGNE fini par la ligne du jeu, pas par sa trace (%s)", seq);
    sc2en_suivi_fermer(&js);
    sc2en_liberer(&s);
    close(fd);
    unlink(chemin);
}

/* E3. Sortie standard en écriture seule (« > fichier », « >> fichier ») : rouverte en lecture. Revue M4, sonde
 * stdout_suivi.c ; dans un fils, fd 1 redirigé comme par le shell. Bits de sortie du fils : échecs. */
static int fils_stdout(const char *chemin, int drapeaux, int rouvert)
{
    int f = open(chemin, drapeaux, 0600);
    if (f < 0 || dup2(f, 1) != 1) return 0x80;
    close(f);
    int ko = 0;
    sc2en_suivi j;
    if (sc2en_suivi_ouvrir(&j, "-", 0) != 0) return 0x01;
    if (!j.possede != !rouvert || (j.fd != 1) != !!rouvert) ko |= 0x02;       /* double possédé, ou fd 1 lui-même */
    if (write(1, "[VKP] 1re frame présentée\n", strlen("[VKP] 1re frame présentée\n")) <= 0) ko |= 0x80;
    if (sc2en_suivi_chercher(&j, "1re frame") != 1) ko |= 0x04;
    sc2en_suivi_fermer(&j);
    if (fcntl(1, F_GETFD) == -1) ko |= 0x08;                   /* le double est fermé, pas la sortie standard */
    return ko;
}

static void suivi_ecriture_seule(const char *dir)
{
    char chemin[600];
    snprintf(chemin, sizeof chemin, "%s/stdout.log", dir);
    static const struct { const char *nom; int drapeaux, rouvert; } R[] = {
        { "> fichier", O_WRONLY | O_CREAT | O_TRUNC, 1 }, { ">> fichier", O_WRONLY | O_CREAT | O_APPEND, 1 },
        { "1<> fichier", O_RDWR | O_CREAT, 0 },
    };
    for (unsigned i = 0; i < sizeof R / sizeof R[0]; i++) {
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) _exit(fils_stdout(chemin, R[i].drapeaux, R[i].rouvert));
        int st = 0;
        VERIF(p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "stdout %s : sortie du fils 0x%x (1 ouvrir, 2 possede/fd, 4 chercher, 8 fd 1 fermé, 80 banc)", R[i].nom,
              WIFEXITED(st) ? WEXITSTATUS(st) : 0xff);
    }
    /* fd prêté en écriture seule : double possédé, fd d'origine jamais fermé */
    int w = open(chemin, O_WRONLY | O_TRUNC);
    sc2en_suivi j;
    VERIF(w >= 0 && sc2en_suivi_fd(&j, w, 0) == 0 && j.possede && j.fd != w, "écriture seule : fd rouvert en lecture");
    ecrire(w, "MOTIF3\n");
    VERIF(sc2en_suivi_chercher(&j, "MOTIF3") == 1, "écriture seule : ligne vue (%s)", strerror(j.erreur));
    sc2en_suivi_fermer(&j);
    VERIF(fcntl(w, F_GETFD) != -1, "écriture seule : fd prêté non fermé");
    close(w);
    /* réouverture impossible (fichier sans droit de lecture) : -1, EBADF, dès l'ouverture */
    if (geteuid() != 0) {
        char ferme[650];
        snprintf(ferme, sizeof ferme, "%s/ecriture-seule.log", dir);
        int f = open(ferme, O_WRONLY | O_CREAT | O_TRUNC, 0200);
        VERIF(f >= 0 && sc2en_suivi_fd(&j, f, 0) == -1 && j.erreur == EBADF && j.fd == -1,
              "écriture seule : réouverture refusée → -1, EBADF (%d, %s)", j.fd, strerror(j.erreur));
        if (f >= 0) close(f);
        unlink(ferme);
    }
    unlink(chemin);
}

static void test_suivi(void)
{
    char dir[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/test_macev.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); g_ko++; return; }
    char chemin[600];
    snprintf(chemin, sizeof chemin, "%s/journal.log", dir);
    int fd = open(chemin, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open"); g_ko++; return; }
    ecrire(fd, "ligne 1\n[VKP] 1re fr");
    sc2en_suivi j;
    VERIF(sc2en_suivi_ouvrir(&j, chemin, 0) == 0, "suivi : ouverture (%s)", strerror(j.erreur));
    const char *M = "1re frame présentée";
    VERIF(sc2en_suivi_chercher(&j, M) == 0, "suivi : ligne incomplète → 0");
    ecrire(fd, "ame présentée\n");
    VERIF(sc2en_suivi_chercher(&j, M) == 1, "suivi : ligne complétée → 1");
    VERIF(sc2en_suivi_chercher(&j, M) == 0, "suivi : la même ligne ne sert qu'une fois");
    ecrire(fd, "autre\nencore : 1re frame présentée\n");
    VERIF(sc2en_suivi_rappel(&j, M) == 1, "suivi : nouvelle occurrence (adaptateur)");
    VERIF(sc2en_suivi_chercher(&j, "autre") == 0, "suivi : le curseur ne revient pas en arrière");
    sc2en_suivi j2;
    VERIF(sc2en_suivi_ouvrir(&j2, chemin, -1) == 0 && sc2en_suivi_chercher(&j2, "ligne 1") == 0, "suivi : départ à la fin");
    ecrire(fd, "ligne 1 bis\n");
    VERIF(sc2en_suivi_chercher(&j2, "ligne 1") == 1, "suivi : départ à la fin, ligne nouvelle vue");
    ecrire(fd, "partielle MOTIF2");                    /* motif entier, mais ligne sans fin : pas encore */
    VERIF(sc2en_suivi_chercher(&j2, "MOTIF2") == 0, "suivi : motif dans une ligne incomplète → 0");
    ecrire(fd, " fin\n");
    VERIF(sc2en_suivi_chercher(&j2, "MOTIF2") == 1, "suivi : la même ligne terminée → 1");
    sc2en_suivi_fermer(&j2);
    /* sortie standard simulée : fd prêté, lu par pread — le décalage d'écriture ne bouge pas */
    sc2en_suivi js;
    off_t avant = lseek(fd, 0, SEEK_CUR);
    VERIF(sc2en_suivi_fd(&js, fd, 0) == 0 && sc2en_suivi_chercher(&js, "ligne 1 bis") == 1, "suivi : fd prêté");
    VERIF(lseek(fd, 0, SEEK_CUR) == avant, "suivi : décalage d'écriture intact (%lld → %lld)", (long long)avant,
          (long long)lseek(fd, 0, SEEK_CUR));
    sc2en_suivi_fermer(&js);
    VERIF(fcntl(fd, F_GETFD) != -1, "suivi : fd prêté non fermé");
    /* ligne géante (100 000 octets, puis le motif, puis 50 000, sans fin de ligne) : vue par morceaux */
    char *g = malloc(150010);
    memset(g, 'x', 100000);
    memcpy(g + 100000, "GEANT", 5);
    memset(g + 100005, 'y', 50000);
    g[150005] = 0;
    ecrire(fd, g);
    free(g);
    VERIF(sc2en_suivi_chercher(&j, "GEANT") == 1, "suivi : motif dans une ligne géante");
    VERIF(sc2en_suivi_chercher(&j, "GEANT") == 0, "suivi : ligne géante : motif vu une fois");
    sc2en_suivi_fermer(&j);
    /* refus : tube, fichier absent */
    int p[2];
    VERIF(pipe(p) == 0 && sc2en_suivi_fd(&js, p[0], 0) == -1 && js.erreur == ESPIPE, "suivi : tube refusé (ESPIPE)");
    close(p[0]);
    close(p[1]);
    char absent[700];
    snprintf(absent, sizeof absent, "%s/absent.log", dir);
    VERIF(sc2en_suivi_ouvrir(&js, absent, 0) == -1 && js.erreur == ENOENT && sc2en_suivi_chercher(&js, "x") == -1,
          "suivi : fichier absent");
    close(fd);
    unlink(chemin);
    suivi_lignes_du_rejeu(dir);
    suivi_ecriture_seule(dir);
    rmdir(dir);
}

/* ==== F. coordonnées =============================================================================================== */
static sc2en_vue vue(uint32_t jw, uint32_t jh, uint32_t fw, uint32_t fh, double echelle)
{
    sc2en_vue v;
    memset(&v, 0, sizeof v);
    v.geo = sc2_geo_swapchain(jw, jh, fw, fh, fw, fh);
    v.echelle = echelle;
    v.fw = fw;
    v.fh = fh;
    return v;
}
static sc2en_coord cj(int32_t x, int32_t y)
{
    sc2en_coord c;
    memset(&c, 0, sizeof c);
    c.repere = 'j';
    c.x = x;
    c.y = y;
    c.ancre = SC2EN_ANCRE_BRUT;
    return c;
}

/* aller-retour exhaustif : chaque pixel client → points de vue → (× échelle) → sc2_geo_vers_jeu */
static void aller_retour(const char *nom, sc2en_vue v, int exact_attendu)
{
    long exacts = 0, total = 0, hors = 0;
    int ecart_max = 0;
    for (int32_t y = 0; y < (int32_t)v.geo.jh; y++)
        for (int32_t x = 0; x < (int32_t)v.geo.jw; x++) {
            sc2en_coord c = cj(x, y);
            sc2en_point pt;
            int d = sc2en_vers_vue(&c, &v, &pt);
            int32_t rx, ry;
            int d2 = sc2en_vue_vers_jeu(&v, pt.vx, pt.vy, &rx, &ry);
            int ex = abs(rx - x), ey = abs(ry - y), m = ex > ey ? ex : ey;
            if (m > ecart_max) ecart_max = m;
            if (!m) exacts++;
            if (d != 1 || d2 != 1) hors++;
            total++;
        }
    VERIF(ecart_max <= 1 && !hors, "%s : aller-retour, écart max %d pixel(s), %ld hors image", nom, ecart_max, hors);
    if (exact_attendu) VERIF(exacts == total, "%s : aller-retour exact pour %ld / %ld pixels", nom, exacts, total);
    printf("  %s : %ld pixels client, %ld exacts, écart max %d\n", nom, total, exacts, ecart_max);
}

static void test_coordonnees(void)
{
    sc2en_point pt;
    sc2en_coord c;
    /* F1 : 1280×720 dans 3200×1800 (vue de 1600×900 points, échelle 2) : 2,5 pixels de vue par pixel client */
    sc2en_vue a = vue(1280, 720, 3200, 1800, 2);
    VERIF(a.geo.ok && a.geo.x0 == 0 && a.geo.sx == 2.5, "F1 : géométrie (x0 %.2f, sx %.4f)", a.geo.x0, a.geo.sx);
    c = cj(640, 360);
    VERIF(sc2en_vers_vue(&c, &a, &pt) == 1 && pt.px == 1601 && pt.py == 901 && pt.vx == 800.5 && pt.vy == 450.5
          && pt.cx == 640 && pt.cy == 360, "F1 : j 640 360 → v %.2f %.2f px %d %d (800.50 450.50, 1601 901)", pt.vx, pt.vy, pt.px, pt.py);
    c = cj(0, 0);
    VERIF(sc2en_vers_vue(&c, &a, &pt) == 1 && pt.px == 1 && pt.py == 1 && pt.vx == 0.5 && pt.vy == 0.5, "F1 : j 0 0 → v %.2f %.2f",
          pt.vx, pt.vy);
    c = cj(1279, 719);
    VERIF(sc2en_vers_vue(&c, &a, &pt) == 1 && pt.px == 3198 && pt.py == 1798 && pt.vx == 1599 && pt.vy == 899,
          "F1 : j 1279 719 → v %.2f %.2f", pt.vx, pt.vy);
    c = cj(1280, 100);
    VERIF(sc2en_vers_vue(&c, &a, &pt) == 0, "F1 : j 1280 100 hors image (dedans %d)", pt.dedans);
    /* F2 : 1280×1024 dans 2560×1440 (1280×720 points) : image 1800×1440 en (380, 0), bandes de 380 pixels */
    sc2en_vue b = vue(1280, 1024, 2560, 1440, 2);
    VERIF(b.geo.ok && b.geo.x0 == 380 && b.geo.y0 == 0 && b.geo.sx == 1.40625, "F2 : géométrie (x0 %.2f, sx %.5f)", b.geo.x0, b.geo.sx);
    c = cj(0, 0);
    VERIF(sc2en_vers_vue(&c, &b, &pt) == 1 && pt.px == 380 && pt.py == 0 && pt.vx == 190 && pt.vy == 0, "F2 : j 0 0 → v %.2f %.2f",
          pt.vx, pt.vy);
    c = cj(640, 512);
    VERIF(sc2en_vers_vue(&c, &b, &pt) == 1 && pt.px == 1280 && pt.py == 720 && pt.vx == 640 && pt.vy == 360,
          "F2 : j 640 512 → v %.2f %.2f (centre 640 360)", pt.vx, pt.vy);
    c = cj(1279, 1023);
    VERIF(sc2en_vers_vue(&c, &b, &pt) == 1 && pt.px == 2179 && pt.py == 1439, "F2 : j 1279 1023 → px %d %d (2179 1439)", pt.px, pt.py);
    /* bandes noires : f 0 0.5 (bord gauche), v 1279.5 360 (bande droite) → hors image, ramenés au bord */
    memset(&c, 0, sizeof c);
    c.repere = 'f'; c.x = 0; c.y = 0.5; c.ancre = SC2EN_ANCRE_BRUT;
    VERIF(sc2en_vers_vue(&c, &b, &pt) == 0 && pt.px == 0 && pt.py == 720 && pt.vx == 0 && pt.vy == 360 && pt.cx == 0 && pt.cy == 512,
          "F2 : f 0 0.5 → v %.2f %.2f client %d %d, bande (dedans %d)", pt.vx, pt.vy, pt.cx, pt.cy, pt.dedans);
    c.x = 1; c.y = 1;
    VERIF(sc2en_vers_vue(&c, &b, &pt) == 0 && pt.px == 2559 && pt.py == 1439 && pt.vx == 1279.5 && pt.vy == 719.5,
          "F2 : f 1 1 → dernier pixel (v %.2f %.2f)", pt.vx, pt.vy);
    c.repere = 'v'; c.x = 1279.5; c.y = 360;
    VERIF(sc2en_vers_vue(&c, &b, &pt) == 0 && pt.px == 2559 && pt.cx == 1279, "F2 : v 1279.5 360 → bande droite, client %d", pt.cx);
    c.x = 640; c.y = 360;
    VERIF(sc2en_vers_vue(&c, &b, &pt) == 1 && pt.cx == 640 && pt.cy == 512, "F2 : v 640 360 → client %d %d (640 512)", pt.cx, pt.cy);
    /* conversion impossible : j sans géométrie, f sans taille de vue, échelle nulle */
    sc2en_vue z;
    memset(&z, 0, sizeof z);
    z.echelle = 2;
    c = cj(1, 1);
    VERIF(sc2en_vers_vue(&c, &z, &pt) == -1, "j sans géométrie : -1");
    c.repere = 'f'; c.x = c.y = 0.5;
    VERIF(sc2en_vers_vue(&c, &z, &pt) == -1, "f sans taille de vue : -1");
    z = a;
    z.echelle = 0;
    VERIF(sc2en_vers_vue(&c, &z, &pt) == -1, "échelle nulle : -1");
    c.repere = 'v'; c.x = 10; c.y = 10;
    z = a;
    z.geo.ok = 0;
    VERIF(sc2en_vers_vue(&c, &z, &pt) == 0 && pt.cx == -1 && pt.vx == 10, "v sans géométrie : points rendus, client inconnu");
    /* F3 : aller-retour exhaustif à l'échelle 2, bandes noires comprises ; puis une image réduite (à 1 pixel près) */
    aller_retour("1280x720 dans 3200x1800", a, 1);
    aller_retour("1280x1024 dans 2560x1440", b, 1);
    aller_retour("1280x720 dans 1000x563 (réduite, échelle 1)", vue(1280, 720, 1000, 563, 1), 0);
    /* F4 : REF (modèles de V1.1 mesurés sur le Fold, inventaire v1-declencheurs §6.2) */
    int32_t x, y;
    sc2en_ref_vers_client(1280, 720, SC2EN_ANCRE_UI, 725, 627, 1280, 1024, &x, &y);
    VERIF(x == 753 && y == 836, "REF ui : (725, 627) → (%d, %d) en 1280x1024 (753, 836)", x, y);
    sc2en_ref_vers_client(1280, 720, SC2EN_ANCRE_SCENE, 270, 510, 1280, 1024, &x, &y);
    VERIF(x == 270 && y == 662, "REF scene : (270, 510) → (%d, %d) (270, 662)", x, y);
    sc2en_ref_vers_client(1280, 720, SC2EN_ANCRE_LIN, 640, 360, 1280, 1024, &x, &y);
    VERIF(x == 640 && y == 512, "REF lin : (640, 360) → (%d, %d) (640, 512)", x, y);
    sc2en_ref_vers_client(1280, 720, SC2EN_ANCRE_BRUT, 725, 627, 1280, 1024, &x, &y);
    VERIF(x == 725 && y == 627, "REF brut : identité (%d, %d)", x, y);
    sc2en_ref_vers_client(0, 0, SC2EN_ANCRE_UI, 725, 627, 1280, 1024, &x, &y);
    VERIF(x == 725 && y == 627, "sans REF : identité (%d, %d)", x, y);
    sc2en_ref_vers_client(1280, 720, SC2EN_ANCRE_UI, 725, 627, 1280, 720, &x, &y);
    VERIF(x == 725 && y == 627, "REF = client : identité (%d, %d)", x, y);
    /* de bout en bout : analyse → REF → géométrie 1280×1024 */
    sc2en_script s;
    char err[512];
    int r = analyser("REF 1280x720\nMOVE j 725 627\nMOVE j 270 510 @scene\nREF -\nMOVE j 725 627\n", &s, err, sizeof err);
    VERIF(r == 0 && s.n == 3, "REF de bout en bout : %s", err);
    static const int32_t C[3][2] = { { 753, 836 }, { 270, 662 }, { 725, 627 } };
    for (int i = 0; i < s.n && i < 3; i++) {
        int d = sc2en_vers_vue(&s.evt[i].pos, &b, &pt);
        int32_t rx, ry;
        sc2en_vue_vers_jeu(&b, pt.vx, pt.vy, &rx, &ry);
        VERIF(d == 1 && pt.cx == C[i][0] && pt.cy == C[i][1] && rx == C[i][0] && ry == C[i][1],
              "REF n°%d : client (%d, %d), retour (%d, %d), attendu (%d, %d)", i, pt.cx, pt.cy, rx, ry, C[i][0], C[i][1]);
    }
    sc2en_liberer(&s);
}

/* ==== G. isolation, H. spécification, I. journal, clavier US ======================================================= */
static void test_divers(void)
{
    sc2en_script s;
    char err[512];
    const char *w;
    analyser("MOVE v 1 1\n", &s, err, sizeof err);
    VERIF(sc2en_isole(&s, NULL, &w) == 1 && !strcmp(w, "défaut"), "isolation : défaut 1");
    VERIF(sc2en_isole(&s, "0", &w) == 0 && !strcmp(w, "variable"), "isolation : variable 0");
    VERIF(sc2en_isole(&s, "oui", &w) == 1 && !strcmp(w, "défaut"), "isolation : variable invalide ignorée");
    sc2en_liberer(&s);
    analyser("ISOLE 0\n", &s, err, sizeof err);
    VERIF(sc2en_isole(&s, NULL, &w) == 0 && !strcmp(w, "scénario"), "isolation : ISOLE 0 du scénario");
    VERIF(sc2en_isole(&s, "1", &w) == 1 && !strcmp(w, "variable"), "isolation : la variable l'emporte sur le scénario");
    VERIF(sc2en_isole(&s, "", &w) == 0 && !strcmp(w, "scénario"), "isolation : variable vide ignorée");
    sc2en_liberer(&s);

    /* spécification : « @p … » est du texte ; « ; » hors guillemets sépare ; fichier avec BOM et CRLF */
    int r = sc2en_charger("@p 3 QUITTER", "VAR", &s, err, sizeof err);
    VERIF(r == 0 && s.n == 1 && s.evt[0].type == SC2EN_QUITTER && s.evt[0].quand.type == SC2EN_AU_PRESENT && s.evt[0].quand.v == 3
          && !strcmp(s.source, "VAR"), "texte « @p 3 QUITTER » : %s", err);
    sc2en_liberer(&s);
    r = sc2en_charger("@p+2 FOCUS 1", "VAR", &s, err, sizeof err);
    VERIF(r == 0 && s.n == 1 && s.evt[0].quand.type == SC2EN_PRESENTS_APRES && s.evt[0].quand.v == 2, "texte « @p+2 … » : %s", err);
    sc2en_liberer(&s);
    r = sc2en_charger("MOVE v 1 2;CLIC g v 3 4 ; JOURNAL \"a;b\" ;# c;OCCULTE 1", "VAR", &s, err, sizeof err);
    VERIF(r == 0 && s.n == 5 && s.evt[3].type == SC2EN_JOURNAL && !strcmp(s.evt[3].texte, "a;b") && s.evt[4].type == SC2EN_OCCULTE
          && s.evt[4].ligne == 5, "texte à « ; » : %d primitives, %s", s.n, err);
    sc2en_liberer(&s);
    char dir[512], chemin[600], spec[610];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/test_macev.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (mkdtemp(dir)) {
        snprintf(chemin, sizeof chemin, "%s/scenario.txt", dir);
        FILE *f = fopen(chemin, "wb");
        fputs("\xEF\xBB\xBF# fichier\r\nMOVE j 1 2\r\n\r\nFOO\r\n", f);
        fclose(f);
        snprintf(spec, sizeof spec, "@%s", chemin);
        r = sc2en_charger(spec, "VAR", &s, err, sizeof err);
        char attendu[700];
        snprintf(attendu, sizeof attendu, "%s:4:1: commande inconnue « FOO »\n", chemin);
        VERIF(r == 1 && !strcmp(err, attendu) && s.n == 1 && s.evt[0].ligne == 2 && s.lignes == 4,
              "fichier (BOM, CRLF) : « %s »", err);
        sc2en_liberer(&s);
        unlink(chemin);
        rmdir(dir);
    } else g_ko++;

    /* journal : une primitive convertie, puis le résumé */
    r = analyser("\n\n\n\n\n\n\n\n\n\n\n@p 400 CLIC g j 767 301 mods=cmd\nTEXTE \"a\"\nATTENDRE_LIGNE \"[VKP] x\" 900\nMODS -\n", &s, err, sizeof err);
    sc2en_vue v = vue(1280, 720, 2560, 1440, 2);
    sc2en_point pt;
    char o[512];
    sc2en_vers_vue(&s.evt[0].pos, &v, &pt);
    sc2en_decrire(&s.evt[0], &pt, 412, 12.345, o, sizeof o);
    VERIF(r == 0 && !strcmp(o, "l.12 CLIC → DOWN g j 767 301 mods=cmd → v 767.50 301.50 (px 1535 603, client 767 301) — Present n°412, "
          "t=12.345 s"), "journal : « %s »", o);
    sc2en_decrire(&s.evt[2], NULL, 1, 1, o, sizeof o);
    VERIF(!strcmp(o, "l.13 TEXTE → TOUCHE_BAS 0x00 \"a\" — Present n°1, t=1.000 s"), "journal TEXTE : « %s »", o);
    sc2en_decrire(&s.evt[4], NULL, 1, 1, o, sizeof o);
    VERIF(!strcmp(o, "l.14 ATTENDRE_LIGNE → ATTENDRE_LIGNE \"[VKP] x\" (900 ms au plus) — Present n°1, t=1.000 s"), "journal ATTENDRE : « %s »", o);
    sc2en_decrire(&s.evt[5], NULL, 1, 1, o, sizeof o);
    VERIF(strstr(o, "l.15 MODS → jalon"), "journal jalon : « %s »", o);
    sc2en_vue zv = v;
    zv.geo.ok = 0;
    sc2en_vers_vue(&s.evt[0].pos, &zv, &pt);
    sc2en_decrire(&s.evt[0], &pt, 0, 0, o, sizeof o);
    VERIF(strstr(o, "client ?, hors image)") != NULL, "journal sans géométrie : « %s »", o);
    char petit[16];
    int k = sc2en_decrire(&s.evt[0], &pt, 0, 0, petit, sizeof petit);
    VERIF(k == 15 && strlen(petit) == 15, "journal tronqué sans débordement (%d)", k);
    sc2en_resumer(&s, 1, "défaut", o, sizeof o);
    VERIF(!strcmp(o, "scénario t : 15 lignes, 6 événements (j : 2, VOIE file : 0), sans REF, ISOLE 1 (défaut)"), "résumé : « %s »", o);
    VERIF(!strstr(o, ".SC2Map") && !strstr(o, "Loading"), "résumé sans les chaînes comptées par les bancs");
    sc2en_liberer(&s);
    r = analyser("MODS rctrl\nTOUCHE 0x0C \"\\e\\u0001\\uF70D\"\nMODS -\n", &s, err, sizeof err);
    sc2en_decrire(&s.evt[0], NULL, 0, 0, o, sizeof o);
    VERIF(r == 0 && strstr(o, "flagsChanged 0x3E, tenus : mods=rctrl"), "journal flagsChanged rctrl : « %s »", o);
    sc2en_decrire(&s.evt[1], NULL, 0, 0, o, sizeof o);
    VERIF(strstr(o, "TOUCHE_BAS 0x0C \"\\e\\u0001\\uF70D\" mods=rctrl"), "journal échappements : « %s »", o);
    sc2en_decrire(&s.evt[3], NULL, 0, 0, o, sizeof o);
    VERIF(strstr(o, "flagsChanged 0x3E, tenus : aucun"), "journal flagsChanged relâché : « %s »", o);
    sc2en_liberer(&s);

    /* clavier US */
    uint16_t kv;
    int maj;
    VERIF(sc2en_qwerty('a', &kv, &maj) && kv == 0x00 && !maj, "US : a");
    VERIF(sc2en_qwerty('A', &kv, &maj) && kv == 0x00 && maj, "US : A (Maj)");
    VERIF(sc2en_qwerty('!', &kv, &maj) && kv == 0x12 && maj, "US : ! (Maj+1)");
    VERIF(sc2en_qwerty('m', &kv, &maj) && kv == 0x2E && !maj, "US : m");
    VERIF(sc2en_qwerty(' ', &kv, &maj) && kv == 0x31, "US : espace");
    VERIF(sc2en_qwerty('~', &kv, &maj) && kv == 0x32 && maj, "US : ~");
    VERIF(!sc2en_qwerty(0xE9, &kv, &maj), "US : é sans touche");
    VERIF(sc2en_drapeaux(SC2EN_MOD_RCMD | SC2EN_MOD_LALT) == (SC2EN_MOD_RCMD | SC2EN_MOD_LALT | SC2EN_MOD_CMD | SC2EN_MOD_ALT),
          "drapeaux : bits NSEventModifierFlag déduits des côtés");
    /* horodatage-marqueur : le rejeu au-dessus de 1e9 s, le matériel (secondes depuis le démarrage, ici 10 ans) en dessous */
    VERIF(sc2en_horodatage(0) == 1e9 && sc2en_horodatage(42) == 1e9 + 42 && sc2en_synthetique(sc2en_horodatage(7))
          && !sc2en_synthetique(315360000.0) && !sc2en_synthetique(12.5), "horodatage-marqueur du rejeu");
}

int main(void)
{
    struct { const char *nom; void (*f)(void); } T[] = {
        { "A grammaire", test_grammaire }, { "B erreurs nommées", test_erreurs }, { "C ordonnancement", test_ordonnancement },
        { "D ATTENDRE_LIGNE", test_attendre }, { "E suivi du journal", test_suivi }, { "F coordonnées", test_coordonnees },
        { "G-I isolation, spécification, journal", test_divers },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        int avant = g_ko;
        T[i].f();
        printf("%s %s\n", g_ko == avant ? "PASS" : "ÉCHEC", T[i].nom);
    }
    printf("RÉSULTAT : %d vérifications, %d échec(s)\n", g_ok + g_ko, g_ko);
    return g_ko ? 1 : 0;
}
