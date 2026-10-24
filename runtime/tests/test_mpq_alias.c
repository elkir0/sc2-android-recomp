/* test_mpq_alias.c — alias « X.wav » → « X.wav_ » des tables de hachage MPQ (recomp-sc2/runtime/sc2_mpq_alias.h, 24/10).
 * Constantes publiques du format, aller-retour du chiffrement, alias sur des tables synthétiques : ajout, entrée déjà
 * présente, nom absent de la table, locales multiples, case effacée réutilisée, case libre de réserve, idempotence,
 * entrées d'origine inchangées. Sortie 0 = conforme. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sc2_mpq_alias.h"

static int g_echecs;
#define VERIFIE(c, ...) do { if (!(c)) { g_echecs++; printf("ÉCHEC l.%d : ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void vide(mpq_hent *t, uint32_t n) { for (uint32_t i = 0; i < n; i++) t[i] = (mpq_hent){ MPQ_H_LIBRE, MPQ_H_LIBRE, MPQ_H_LIBRE, MPQ_H_LIBRE }; }
static int poser(mpq_hent *t, uint32_t n, const char *nom, uint32_t loc, uint32_t bloc)
{
    uint32_t i = mpq_hache(nom, 0) & (n - 1);
    for (uint32_t k = 0; k < n; k++, i = (i + 1) & (n - 1))
        if (t[i].bloc == MPQ_H_LIBRE || t[i].bloc == MPQ_H_EFFACE) {
            t[i] = (mpq_hent){ mpq_hache(nom, 1), mpq_hache(nom, 2), loc, bloc }; return (int)i;
        }
    return -1;
}
static uint32_t bloc_de(const mpq_hent *t, uint32_t n, const char *nom, uint32_t loc)
{
    int i = mpq_h_trouve(t, n, nom, strlen(nom), loc);
    return i < 0 ? MPQ_H_LIBRE : t[i].bloc;
}

int main(void)
{
    /* 1. constantes publiques (clés des tables) et insensibilité casse / séparateur */
    VERIFIE(mpq_hache("(hash table)", 3) == 0xC3AF3770u, "clé (hash table) 0x%08X", mpq_hache("(hash table)", 3));
    VERIFIE(mpq_hache("(block table)", 3) == 0xEC83B3A3u, "clé (block table) 0x%08X", mpq_hache("(block table)", 3));
    VERIFIE(mpq_hache("assets/sounds/a.wav", 1) == mpq_hache("Assets\\Sounds\\A.WAV", 1), "casse / séparateur");

    /* 2. aller-retour du chiffrement, et chiffré différent du clair */
    uint32_t clair[8] = { 0, 1, 2, 0xFFFFFFFFu, 0x12345678u, 7, 8, 9 }, d[8];
    memcpy(d, clair, sizeof d);
    mpq_chiffrer(d, 8, 0xC3AF3770u);
    VERIFIE(memcmp(d, clair, sizeof d) != 0, "chiffré identique au clair");
    mpq_dechiffrer(d, 8, 0xC3AF3770u);
    VERIFIE(!memcmp(d, clair, sizeof d), "aller-retour du chiffrement");

    /* 3. table de 64 entrées : alias, déjà présent, absent, deux locales, entrées d'origine inchangées, idempotence */
    enum { N = 64 };
    mpq_hent t[N], avant[N];
    vide(t, N);
    poser(t, N, "Assets\\Sounds\\A.wav_", 0, 3);
    poser(t, N, "Assets\\Sounds\\B.wav_", 0, 4);
    poser(t, N, "Assets\\Sounds\\B.wav", 0, 5);           /* déjà présent : jamais doublé */
    poser(t, N, "Assets\\Sounds\\C.ogg", 0, 6);
    poser(t, N, "Assets\\Sounds\\E.wav_", 0, 7);
    poser(t, N, "Assets\\Sounds\\E.wav_", 0x40C, 8);      /* frFR */
    memcpy(avant, t, sizeof t);
    const char lst[] = "Assets\\Sounds\\A.wav_\r\nAssets\\Sounds\\B.wav_\nAssets\\Sounds\\C.ogg;Assets\\Sounds\\D.wav_\r\n"
                       "Assets\\Sounds\\E.WAV_\r\n";
    mpq_alias_bilan bl;
    int r = mpq_alias_wav(t, N, lst, strlen(lst), &bl);
    VERIFIE(r == 3 && bl.ajouts == 3, "ajouts %d (attendu 3 : A, E neutre, E frFR)", r);
    VERIFIE(bl.deja == 1 && bl.absents == 1 && bl.plein == 0 && bl.noms == 4, "bilan déjà %d absents %d plein %d noms %d",
            bl.deja, bl.absents, bl.plein, bl.noms);
    VERIFIE(bloc_de(t, N, "Assets\\Sounds\\A.wav", 0) == 3, "A.wav → bloc de A.wav_");
    VERIFIE(bloc_de(t, N, "Assets\\Sounds\\B.wav", 0) == 5, "B.wav inchangé");
    VERIFIE(bloc_de(t, N, "Assets\\Sounds\\E.wav", 0) == 7 && bloc_de(t, N, "Assets\\Sounds\\E.wav", 0x40C) == 8,
            "E.wav : une entrée par locale");
    VERIFIE(bloc_de(t, N, "Assets\\Sounds\\D.wav", MPQ_H_LIBRE) == MPQ_H_LIBRE, "D.wav inventé");
    VERIFIE(bloc_de(t, N, "Assets\\Sounds\\C.og", MPQ_H_LIBRE) == MPQ_H_LIBRE, "alias hors .wav_");
    int modifs = 0;
    for (int i = 0; i < N; i++) if (avant[i].bloc != MPQ_H_LIBRE && memcmp(&avant[i], &t[i], sizeof t[i])) modifs++;
    VERIFIE(modifs == 0, "%d entrée(s) d'origine modifiée(s)", modifs);
    r = mpq_alias_wav(t, N, lst, strlen(lst), &bl);
    VERIFIE(r == 0 && bl.deja == 4, "2e passage : ajouts %d, déjà %d (attendu 0 / 4)", r, bl.deja);

    /* 4. réserve : table de 4 entrées, 3 occupées → la dernière case libre n'est jamais prise */
    mpq_hent p[4];
    vide(p, 4);
    poser(p, 4, "x1", 0, 1); poser(p, 4, "x2", 0, 2); poser(p, 4, "F.wav_", 0, 9);
    const char lst2[] = "F.wav_";
    r = mpq_alias_wav(p, 4, lst2, strlen(lst2), &bl);
    VERIFIE(r == 0 && bl.plein == 1, "table pleine : ajouts %d, plein %d", r, bl.plein);

    /* 5. case effacée réutilisée même quand il ne reste qu'une case libre : la 1re case de la chaîne de « F.wav »
     *    est effacée, une seule case reste jamais utilisée */
    vide(p, 4);
    uint32_t s0 = mpq_hache("F.wav", 0) & 3;
    p[s0] = (mpq_hent){ 1, 1, 0, 1 };                                  /* occupée pendant la pose de F.wav_ */
    int iF = poser(p, 4, "F.wav_", 0, 9);
    for (uint32_t i = 0; i < 4; i++) if (p[i].bloc == MPQ_H_LIBRE) { p[i] = (mpq_hent){ 2, 2, 0, 2 }; break; }
    p[s0].bloc = MPQ_H_EFFACE;
    VERIFIE(iF >= 0 && (uint32_t)iF != s0, "préparation");
    r = mpq_alias_wav(p, 4, lst2, strlen(lst2), &bl);
    VERIFIE(r == 1 && bloc_de(p, 4, "F.wav", 0) == 9 && p[s0].bloc == 9, "case effacée : ajouts %d", r);
    VERIFIE(bloc_de(p, 4, "F.wav_", 0) == 9, "F.wav_ toujours trouvé");

    /* 6. entrées invalides : taille non puissance de 2 → rien */
    VERIFIE(mpq_alias_wav(t, 48, lst, strlen(lst), &bl) == 0, "taille 48 refusée");

    if (g_echecs) { printf("test_mpq_alias : %d échec(s)\n", g_echecs); return 1; }
    printf("test_mpq_alias : conforme\n");
    return 0;
}
