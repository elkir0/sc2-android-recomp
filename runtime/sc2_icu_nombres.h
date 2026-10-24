/* sc2_icu_nombres.h — L5 (24/10) : NumberFormat d'ICU 4.4 (DecimalFormat, locale en_US) en logique pure, pour les
 * objets factices que sc2_iat_p4.c rend à NumberFormat::createInstance / createPercentInstance (icuin44).
 *
 * Usage par le jeu (lift ; rapport sc2-bancs/20261024/icu-nombres/rapport.md) : l'objet n'est atteint que par le cache
 * (genre, locale) → NumberFormat* de L_00B6F930 (tableau [0x0158595C], entrées de 12 o ; L_00B6F560 crée : genre 0
 * createInstance, 1 createCurrencyInstance, 2 createPercentInstance, résultat jeté si UErrorCode > 0), et deux aides :
 *   L_00B6EF20 (entier)   : vtable+0x50 setGroupingUsed(UBool), puis format(int32_t, UnicodeString&) importé ;
 *   L_00B6EFC0 (flottant) : +0x50 setGroupingUsed, +0x60 setMinimumFractionDigits, +0x5C setMaximumFractionDigits,
 *                           puis format(double, UnicodeString&) importé ;
 * L_00B6FB00 détruit les objets à l'arrêt (vtable[0], drapeau 1). Offsets vérifiés dans icuin44.dll de l'installation
 * (??_7DecimalFormat@icu_44@@6B@ : [20] setGroupingUsed, [23] setMaximumFractionDigits, [24] setMinimumFractionDigits).
 * Les appelants du flottant passent (min, max) = (0, 2) ou (2, 0) : min posé AVANT max, (2, 0) donne donc 0 décimale.
 *
 * Règles d'ICU 4.4 reproduites (decimfmt.cpp, digitlst.cpp, numfmt.cpp) :
 *  - motifs en_US (CLDR 1.8) : nombre « #,##0.### » (au moins 1 chiffre entier, 0 à 3 décimales, groupes de 3 par
 *    « , », virgule décimale « . ») ; pourcentage « #,##0% » (×100, 0 décimale, suffixe « % ») ; négatif : « - » ;
 *  - double → 15 chiffres significatifs (DigitList::set : sprintf « %+1.14e »), ×100 exact en décimal pour le
 *    pourcentage, arrondi à max_frac décimales AU PAIR (kRoundHalfEven, DigitList::roundFixedPoint), zéros finaux
 *    retirés puis décimales complétées jusqu'à min_frac ; le signe est pris AVANT l'arrondi (-0,001 → « -0 », -0.0 →
 *    « -0 ») ; NaN → « NaN » sans affixe ; infini → « ∞ » entre les affixes ;
 *  - entier : exact (int64), mêmes affixes, complété à min_frac décimales ;
 *  - setters : bornes de NumberFormat (au moins 0 ; min et max recalés l'un sur l'autre) et de DecimalFormat (340
 *    décimales, 309 chiffres entiers) — sans effet sur les valeurs du jeu (0 et 2) ;
 *  - plus de chiffres entiers que max_ent : les max_ent de poids faible, comme ICU.
 * Monnaie (genre 1) : non fournie (aucun appelant dans le lift), le runtime rend NULL comme avant.
 * Pur calcul, sans CPU invité : test hôte recomp-sc2/tests/test_icu_nombres.c (scripts/verifier.sh, ctest). */
#ifndef SC2_ICU_NOMBRES_H
#define SC2_ICU_NOMBRES_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SC2_NF_NOMBRE = 0, SC2_NF_MONNAIE = 1, SC2_NF_POURCENT = 2 };   /* genre = ecx de L_00B6F560 */
#define SC2_NF_MAX_CAR 800   /* « - » + 309 chiffres + 102 séparateurs + « . » + 340 décimales + « % » < 800 */

typedef struct { int32_t genre, groupe, max_ent, min_ent, max_frac, min_frac, entier_seul; } sc2_nf;

/* État après applyPattern du motif en_US du genre (DecimalFormat d'ICU 4.4). */
static inline void sc2_nf_init(sc2_nf *f, int genre)
{
    f->genre = genre; f->groupe = 1; f->max_ent = 309; f->min_ent = 1;
    f->max_frac = genre == SC2_NF_POURCENT ? 0 : 3; f->min_frac = 0; f->entier_seul = 0;
}
static inline int32_t sc2_nf_borne(int32_t v, int32_t borne, int32_t haut)
{ if (v > borne) v = borne; return v < 0 ? 0 : v > haut ? haut : v; }
/* DecimalFormat::set… (borne 309 / 340) puis NumberFormat::set… (324 = DBL_MAX_10_EXP + DBL_DIG + 1, 127) */
static inline void sc2_nf_max_ent(sc2_nf *f, int32_t n)
{ f->max_ent = sc2_nf_borne(n, 309, 324); if (f->min_ent > f->max_ent) f->min_ent = f->max_ent; }
static inline void sc2_nf_min_ent(sc2_nf *f, int32_t n)
{ f->min_ent = sc2_nf_borne(n, 309, 127); if (f->min_ent > f->max_ent) f->max_ent = f->min_ent; }
static inline void sc2_nf_max_frac(sc2_nf *f, int32_t n)
{ f->max_frac = sc2_nf_borne(n, 340, 324); if (f->max_frac < f->min_frac) f->min_frac = f->max_frac; }
static inline void sc2_nf_min_frac(sc2_nf *f, int32_t n)
{ f->min_frac = sc2_nf_borne(n, 340, 127); if (f->max_frac < f->min_frac) f->max_frac = f->min_frac; }

/* Liste de chiffres (DigitList) : valeur = 0,c[0]c[1]…c[n-1] × 10^dp, sans zéro final ; n = 0 : zéro (signé). */
typedef struct { uint8_t c[24]; int n, dp, neg; } sc2_nf_chiffres;

static inline void sc2_nf_rogner(sc2_nf_chiffres *d)
{ while (d->n > 0 && !d->c[d->n - 1]) d->n--; if (!d->n) d->dp = 0; }

static inline void sc2_nf_de_double(sc2_nf_chiffres *d, double v)   /* v fini */
{
    char t[48];
    snprintf(t, sizeof t, "%+.14e", v);                  /* « ±d.dddddddddddddde±XX » : 15 chiffres significatifs */
    const char *p = t + 1;
    d->neg = t[0] == '-'; d->n = 0;
    for (; *p && *p != 'e' && d->n < 15; p++) if (*p >= '0' && *p <= '9') d->c[d->n++] = (uint8_t)(*p - '0');
    while (*p && *p != 'e') p++;
    d->dp = (*p == 'e' ? (int)strtol(p + 1, NULL, 10) : 0) + 1;
    sc2_nf_rogner(d);
}
static inline void sc2_nf_de_entier(sc2_nf_chiffres *d, int64_t v)
{
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    char t[24];
    int k = snprintf(t, sizeof t, "%llu", (unsigned long long)u);
    d->neg = v < 0; d->n = 0;
    for (int i = 0; i < k; i++) d->c[d->n++] = (uint8_t)(t[i] - '0');
    d->dp = k;
    sc2_nf_rogner(d);
}
/* Arrondi au pair à max_frac décimales (decNumberQuantize, DEC_ROUND_HALF_EVEN), puis zéros finaux retirés. */
static inline void sc2_nf_arrondir(sc2_nf_chiffres *d, int max_frac)
{
    int garde = d->dp + max_frac;                        /* chiffres conservés */
    if (d->n <= garde) return;
    int haut = 0;
    if (garde >= 0) {
        int r = d->c[garde], reste = 0;
        for (int i = garde + 1; i < d->n; i++) reste |= d->c[i];
        haut = r > 5 || (r == 5 && (reste || (garde > 0 && (d->c[garde - 1] & 1))));
    }
    d->n = garde < 0 ? 0 : garde;
    if (haut) {
        int i = d->n - 1;
        while (i >= 0 && d->c[i] == 9) d->c[i--] = 0;
        if (i >= 0) d->c[i]++;
        else { memmove(d->c + 1, d->c, (size_t)d->n); d->c[0] = 1; d->n++; d->dp++; }
    }
    sc2_nf_rogner(d);
}
/* DecimalFormat::subformat : affixes, chiffres entiers groupés, décimales ; rend le nombre d'unités UTF-16 écrites
 * (au plus cap). */
static inline int sc2_nf_ecrire(const sc2_nf *f, const sc2_nf_chiffres *d, uint16_t *o, int cap)
{
    int k = 0;
#define SC2_NF_MET(ch) do { if (k < cap) o[k] = (uint16_t)(ch); k++; } while (0)
    if (d->neg) SC2_NF_MET('-');
    int ent = d->dp > 0 ? d->dp : 0, nb = ent > f->min_ent ? ent : f->min_ent, debut = k;
    if (nb > f->max_ent) nb = f->max_ent;                /* les nb chiffres de poids faible */
    for (int i = nb - 1; i >= 0; i--) {                  /* i : rang du chiffre (10^i) */
        int j = d->dp - 1 - i;
        SC2_NF_MET('0' + (j >= 0 && j < d->n ? d->c[j] : 0));
        if (f->groupe && i > 0 && i % 3 == 0) SC2_NF_MET(',');
    }
    int decimales = d->n > ent || f->min_frac > 0;
    if (!decimales && k == debut) SC2_NF_MET('0');
    if (decimales) {
        SC2_NF_MET('.');
        for (int i = 0; i < f->max_frac; i++) {
            int j = d->dp + i;
            if (i >= f->min_frac && j >= d->n) break;
            SC2_NF_MET('0' + (j >= 0 && j < d->n ? d->c[j] : 0));
        }
    }
    if (f->genre == SC2_NF_POURCENT) SC2_NF_MET('%');
#undef SC2_NF_MET
    return k < cap ? k : cap;
}

/* NumberFormat::format(double, UnicodeString&) (DecimalFormat::_format) */
static inline int sc2_nf_format_double(const sc2_nf *f, double v, uint16_t *o, int cap)
{
    int k = 0;
    if (isnan(v)) { static const char nan[] = "NaN"; for (; k < 3 && k < cap; k++) o[k] = (uint16_t)nan[k]; return k; }
    if (isinf(v)) {
        uint16_t t[3]; int n = 0;
        if (v < 0) t[n++] = '-';
        t[n++] = 0x221E;
        if (f->genre == SC2_NF_POURCENT) t[n++] = '%';
        for (; k < n && k < cap; k++) o[k] = t[k];
        return k;
    }
    sc2_nf_chiffres d;
    sc2_nf_de_double(&d, v);
    if (f->genre == SC2_NF_POURCENT && d.n) d.dp += 2;
    sc2_nf_arrondir(&d, f->max_frac);
    return sc2_nf_ecrire(f, &d, o, cap);
}
/* NumberFormat::format(int32_t, UnicodeString&) → format(int64_t, …) */
static inline int sc2_nf_format_entier(const sc2_nf *f, int64_t v, uint16_t *o, int cap)
{
    sc2_nf_chiffres d;
    sc2_nf_de_entier(&d, v);
    if (f->genre == SC2_NF_POURCENT && d.n) d.dp += 2;
    sc2_nf_arrondir(&d, f->max_frac);                    /* sans effet sur un entier */
    return sc2_nf_ecrire(f, &d, o, cap);
}
#endif
