/* sc2_mpq_alias.h — alias « X.wav » → « X.wav_ » dans une table de hachage MPQ (24/10, port runtime de la passe 7z2).
 *
 * Les archives d'assets rebâties depuis le Tome (mpqrepack) ne contiennent les bruitages que sous « X.wav_ » ; les
 * données du jeu (SoundData.xml) demandent « X.wav ». Le Storm du moteur (lié en statique dans SC2.exe) cherche ses
 * fichiers dans la table de hachage qu'il lit par ReadFile : sc2_mpq.c sert à sa place une table AUGMENTÉE, où chaque
 * entrée vivante de « X.wav_ » reçoit une entrée « X.wav » sur le même bloc (même locale, même plate-forme), posée
 * dans une case libre de la chaîne de sondage de « X.wav ». Aucune entrée existante ne change : une recherche qui
 * réussissait réussit à l'identique ; « X.wav » déjà présent (même locale) n'est jamais doublé.
 *
 * Logique pure (format MPQ public : table de chiffrement, hachage, chiffrement des tables), en-tête seul, sans
 * StormLib : sc2_mpq.c l'inclut, tests-hote/v3/mpq-alias.cmake la teste (recomp-sc2/tests/test_mpq_alias.c).
 */
#ifndef SC2_MPQ_ALIAS_H
#define SC2_MPQ_ALIAS_H

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define MPQ_H_LIBRE   0xFFFFFFFFu   /* case jamais utilisée : fin de chaîne de sondage */
#define MPQ_H_EFFACE  0xFFFFFFFEu   /* case d'un fichier effacé : la chaîne continue */

typedef struct { uint32_t a, b, loc, bloc; } mpq_hent;   /* nomA, nomB, locale | plate-forme << 16, bloc */
typedef struct { int ajouts, deja, absents, plein, noms; } mpq_alias_bilan;

static uint32_t g_mpq_crypt[0x500];
static void mpq_crypt_init(void)
{
    if (g_mpq_crypt[0]) return;
    uint32_t s = 0x00100001u;
    for (int i = 0; i < 0x100; i++)
        for (int j = i, k = 0; k < 5; k++, j += 0x100) {
            s = (s * 125 + 3) % 0x2AAAAB; uint32_t t1 = (s & 0xFFFF) << 16;
            s = (s * 125 + 3) % 0x2AAAAB; g_mpq_crypt[j] = t1 | (s & 0xFFFF);
        }
}
/* type 0 = position dans la table, 1/2 = noms A/B, 3 = clé ; nom de longueur n (pas forcément terminé par 0). */
static uint32_t mpq_hache_n(const char *nom, size_t n, int type)
{
    mpq_crypt_init();
    uint32_t s1 = 0x7FED7FEDu, s2 = 0xEEEEEEEEu;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)nom[i];
        uint32_t ch = (uint32_t)toupper(c == '/' ? '\\' : c);
        s1 = g_mpq_crypt[type * 0x100 + ch] ^ (s1 + s2);
        s2 = ch + s1 + s2 + (s2 << 5) + 3;
    }
    return s1;
}
static uint32_t mpq_hache(const char *nom, int type) { return mpq_hache_n(nom, strlen(nom), type); }
static void mpq_dechiffrer(uint32_t *d, size_t n, uint32_t cle)
{
    mpq_crypt_init();
    uint32_t s2 = 0xEEEEEEEEu;
    for (size_t i = 0; i < n; i++) {
        s2 += g_mpq_crypt[0x400 + (cle & 0xFF)];
        uint32_t ch = d[i] ^ (cle + s2);
        cle = ((~cle << 0x15) + 0x11111111u) | (cle >> 0x0B);
        s2 = ch + s2 + (s2 << 5) + 3; d[i] = ch;
    }
}
static void mpq_chiffrer(uint32_t *d, size_t n, uint32_t cle)
{
    mpq_crypt_init();
    uint32_t s2 = 0xEEEEEEEEu;
    for (size_t i = 0; i < n; i++) {
        s2 += g_mpq_crypt[0x400 + (cle & 0xFF)];
        uint32_t ch = d[i]; d[i] = ch ^ (cle + s2);
        cle = ((~cle << 0x15) + 0x11111111u) | (cle >> 0x0B);
        s2 = ch + s2 + (s2 << 5) + 3;
    }
}
/* Index de l'entrée vivante (nom, locale|plate-forme `loc`) ; loc = MPQ_H_LIBRE : n'importe laquelle ; -1 si absente. */
static int mpq_h_trouve(const mpq_hent *t, uint32_t taille, const char *nom, size_t n, uint32_t loc)
{
    uint32_t m = taille - 1, i = mpq_hache_n(nom, n, 0) & m, a = mpq_hache_n(nom, n, 1), b = mpq_hache_n(nom, n, 2);
    for (uint32_t k = 0; k < taille && t[i].bloc != MPQ_H_LIBRE; k++, i = (i + 1) & m)
        if (t[i].a == a && t[i].b == b && t[i].bloc != MPQ_H_EFFACE && (loc == MPQ_H_LIBRE || t[i].loc == loc)) return (int)i;
    return -1;
}
/* Ajoute dans `t` (table DÉCHIFFRÉE de `taille` entrées, puissance de 2) les alias « X.wav » des noms « X.wav_ » du
 * (listfile) `lst` (n octets, séparateurs \r \n ;). Une case jamais utilisée reste toujours libre (fin de sondage du
 * Storm du moteur). Rend le nombre d'entrées ajoutées ; détail dans *bl (facultatif). */
static int mpq_alias_wav(mpq_hent *t, uint32_t taille, const char *lst, size_t n, mpq_alias_bilan *bl)
{
    mpq_alias_bilan z = { 0, 0, 0, 0, 0 };
    if (!t || !taille || (taille & (taille - 1))) { if (bl) *bl = z; return 0; }
    uint32_t m = taille - 1, libres = 0;
    for (uint32_t i = 0; i < taille; i++) libres += t[i].bloc == MPQ_H_LIBRE;
    for (size_t p = 0; p < n;) {
        size_t q = p;
        while (q < n && lst[q] != '\r' && lst[q] != '\n' && lst[q] != ';' && lst[q]) q++;
        const char *nom = lst + p; size_t l = q - p;
        p = q + 1;
        if (l < 6 || strncasecmp(nom + l - 5, ".wav_", 5)) continue;
        z.noms++;
        uint32_t i = mpq_hache_n(nom, l, 0) & m, sa = mpq_hache_n(nom, l, 1), sb = mpq_hache_n(nom, l, 2);
        int src = 0;
        for (uint32_t k = 0; k < taille && t[i].bloc != MPQ_H_LIBRE; k++, i = (i + 1) & m) {
            if (t[i].a != sa || t[i].b != sb || t[i].bloc == MPQ_H_EFFACE) continue;
            src++;                                          /* une entrée par locale : chacune reçoit son alias */
            if (mpq_h_trouve(t, taille, nom, l - 1, t[i].loc) >= 0) { z.deja++; continue; }
            uint32_t j = mpq_hache_n(nom, l - 1, 0) & m, kk = 0;
            while (kk < taille && t[j].bloc != MPQ_H_LIBRE && t[j].bloc != MPQ_H_EFFACE) { j = (j + 1) & m; kk++; }
            if (kk == taille || (t[j].bloc == MPQ_H_LIBRE && libres <= 1)) { z.plein++; continue; }
            if (t[j].bloc == MPQ_H_LIBRE) libres--;
            t[j].a = mpq_hache_n(nom, l - 1, 1); t[j].b = mpq_hache_n(nom, l - 1, 2);
            t[j].loc = t[i].loc; t[j].bloc = t[i].bloc;
            z.ajouts++;
        }
        z.absents += !src;
    }
    if (bl) *bl = z;
    return z.ajouts;
}

#endif
