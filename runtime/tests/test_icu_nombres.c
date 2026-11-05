/* test_icu_nombres.c — L5 (24/10) : NumberFormat d'ICU 4.4 en_US factice (recomp-sc2/runtime/sc2_icu_nombres.h).
 *
 * Valeurs attendues = règles d'ICU 4.4 (DecimalFormat « #,##0.### » et « #,##0% », 15 chiffres significatifs, arrondi
 * au pair) ; les séquences de réglages sont celles des appelants du jeu (L_00B6EF20 : groupes ; L_00B6EFC0 : groupes,
 * min puis max décimales, avec (min, max) = (0, 2) ou (2, 0)).
 * Recette : cc -Wall -Wextra -I recomp-sc2/runtime recomp-sc2/tests/test_icu_nombres.c -o test_icu_nombres -lm
 * && ./test_icu_nombres (sinon scripts/verifier.sh, fragment tests-hote/v3/l5-icu-nombres.cmake). */
#include "sc2_icu_nombres.h"

#include <stdio.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("ÉCHEC  "); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static const char *utf8(const uint16_t *s, int n)
{
    static char b[4][1024]; static int t;
    char *o = b[t++ & 3]; size_t k = 0;
    for (int i = 0; i < n && k + 4 < sizeof b[0]; i++) {
        unsigned ch = s[i];
        if (ch < 0x80) o[k++] = (char)ch;
        else if (ch < 0x800) { o[k++] = (char)(0xC0 | ch >> 6); o[k++] = (char)(0x80 | (ch & 63)); }
        else { o[k++] = (char)(0xE0 | ch >> 12); o[k++] = (char)(0x80 | (ch >> 6 & 63)); o[k++] = (char)(0x80 | (ch & 63)); }
    }
    o[k] = 0;
    return o;
}
static const char *fd(const sc2_nf *f, double v) { uint16_t t[SC2_NF_MAX_CAR]; return utf8(t, sc2_nf_format_double(f, v, t, SC2_NF_MAX_CAR)); }
static const char *fi(const sc2_nf *f, int64_t v) { uint16_t t[SC2_NF_MAX_CAR]; return utf8(t, sc2_nf_format_entier(f, v, t, SC2_NF_MAX_CAR)); }

/* réglages du flottant du jeu (L_00B6EFC0) */
static sc2_nf jeu(int genre, int groupe, int min, int max)
{ sc2_nf f; sc2_nf_init(&f, genre); f.groupe = groupe; sc2_nf_min_frac(&f, min); sc2_nf_max_frac(&f, max); return f; }

int main(void)
{
    sc2_nf n; sc2_nf_init(&n, SC2_NF_NOMBRE);
    sc2_nf p; sc2_nf_init(&p, SC2_NF_POURCENT);

    /* --- entiers (L_00B6EF20, groupes posés par l'appelant) --- */
    static const struct { int64_t v; const char *g, *s; } E[] = {
        { 200000, "200,000", "200000" }, { 0, "0", "0" }, { 7, "7", "7" }, { 999, "999", "999" },
        { 1000, "1,000", "1000" }, { 30000, "30,000", "30000" }, { -1234567, "-1,234,567", "-1234567" },
        { 2147483647, "2,147,483,647", "2147483647" }, { -2147483647 - 1, "-2,147,483,648", "-2147483648" },
        { INT64_MIN, "-9,223,372,036,854,775,808", "-9223372036854775808" },
    };
    for (unsigned k = 0; k < sizeof E / sizeof E[0]; k++) {
        sc2_nf f = n; const char *r = fi(&f, E[k].v);
        CHECK(!strcmp(r, E[k].g), "entier %lld groupé : « %s » (attendu « %s »)", (long long)E[k].v, r, E[k].g);
        f.groupe = 0; r = fi(&f, E[k].v);
        CHECK(!strcmp(r, E[k].s), "entier %lld sans groupes : « %s » (attendu « %s »)", (long long)E[k].v, r, E[k].s);
    }
    CHECK(!strcmp(fi(&p, 5), "500%"), "pourcentage entier 5 → « 500%% » (%s)", fi(&p, 5));
    { sc2_nf f = n; sc2_nf_min_frac(&f, 2); CHECK(!strcmp(fi(&f, 5), "5.00"), "entier 5, min 2 décimales → « 5.00 » (%s)", fi(&f, 5)); }

    /* --- flottants, motif par défaut « #,##0.### » --- */
    static const struct { double v; const char *s; } D[] = {
        { 1234.5678, "1,234.568" }, { 0.5, "0.5" }, { 2.0, "2" }, { -2.25, "-2.25" }, { 1e20, "100,000,000,000,000,000,000" },
        { 0.0005, "0" },          /* 15 chiffres « 5 » exact → pair (0) */
        { 0.0015, "0.002" },      /* 0,0015000000000000000312… ; 15 chiffres : 1,5e-03 → pair (2) */
        { 999.9996, "1,000" }, { 0.1, "0.1" }, { 123456789.0, "123,456,789" }, { -0.0, "-0" }, { -0.0001, "-0" },
        { 1.0 / 3.0, "0.333" }, { 2.0 / 3.0, "0.667" },
    };
    for (unsigned k = 0; k < sizeof D / sizeof D[0]; k++) {
        const char *r = fd(&n, D[k].v);
        CHECK(!strcmp(r, D[k].s), "double %.17g (#,##0.###) : « %s » (attendu « %s »)", D[k].v, r, D[k].s);
    }
    CHECK(!strcmp(fd(&n, NAN), "NaN"), "NaN → « NaN » (%s)", fd(&n, NAN));
    CHECK(!strcmp(fd(&n, INFINITY), "∞"), "+inf → « ∞ » (%s)", fd(&n, INFINITY));
    CHECK(!strcmp(fd(&p, -INFINITY), "-∞%"), "-inf en pourcentage → « -∞%% » (%s)", fd(&p, -INFINITY));

    /* --- séquences des appelants du flottant (valeurs float promues en double, comme fld dword / fstp qword) --- */
    static const struct { int genre, groupe, min, max; float v; const char *s; } J[] = {
        { 0, 0, 0, 2, 3.14159f, "3.14" },  { 0, 0, 0, 2, 2.5f, "2.5" },   { 0, 0, 0, 2, 10.0f, "10" },
        { 0, 0, 0, 2, 1.005f, "1" },       /* 1,00499999523… → 1,00 → « 1 » */
        { 0, 0, 0, 2, 0.125f, "0.12" },    /* exact, pair */
        { 0, 0, 0, 2, 0.375f, "0.38" },    /* exact, pair au-dessus */
        { 0, 1, 2, 0, 2.5f, "2" },         /* (min 2, max 0) → 0 décimale ; 2,5 → pair 2 */
        { 0, 1, 2, 0, 3.5f, "4" },         { 0, 1, 2, 0, 1234.4f, "1,234" }, { 0, 1, 0, 2, 12345.678f, "12,345.68" },
        { 2, 1, 2, 0, 0.5f, "50%" },       { 2, 1, 2, 0, 0.125f, "12%" },   /* 12,5 → pair */
        { 2, 1, 2, 0, 0.135f, "14%" },     /* 0,135f = 0,13500000536… → 13,5000005 → 14 */
        { 2, 0, 0, 2, 0.12345f, "12.35%" }, /* 0,123450003564… ×100 */
        { 2, 1, 2, 0, 12.0f, "1,200%" },   { 2, 1, 2, 0, -0.25f, "-25%" }, { 2, 1, 2, 0, 0.004f, "0%" },
    };
    for (unsigned k = 0; k < sizeof J / sizeof J[0]; k++) {
        sc2_nf f = jeu(J[k].genre, J[k].groupe, J[k].min, J[k].max);
        const char *r = fd(&f, (double)J[k].v);
        CHECK(!strcmp(r, J[k].s), "jeu genre %d groupes %d (min %d, max %d) %.9g : « %s » (attendu « %s »)",
              J[k].genre, J[k].groupe, J[k].min, J[k].max, (double)J[k].v, r, J[k].s);
    }

    /* --- setters (bornes et recalage) --- */
    { sc2_nf f = n; sc2_nf_min_frac(&f, 2); sc2_nf_max_frac(&f, 0);
      CHECK(f.min_frac == 0 && f.max_frac == 0, "min 2 puis max 0 → (0, 0) (obtenu (%d, %d))", f.min_frac, f.max_frac); }
    { sc2_nf f = n; sc2_nf_max_frac(&f, 0); sc2_nf_min_frac(&f, 2);
      CHECK(f.min_frac == 2 && f.max_frac == 2, "max 0 puis min 2 → (2, 2) (obtenu (%d, %d))", f.min_frac, f.max_frac);
      CHECK(!strcmp(fd(&f, 3.0), "3.00"), "(2, 2) : 3 → « 3.00 » (%s)", fd(&f, 3.0)); }
    { sc2_nf f = n; sc2_nf_max_frac(&f, -5); CHECK(f.max_frac == 0 && f.min_frac == 0, "max -5 → 0"); }
    { sc2_nf f = n; sc2_nf_max_frac(&f, 100000); CHECK(f.max_frac == 324, "max 100000 → 324 (obtenu %d)", f.max_frac); }
    { sc2_nf f = n; sc2_nf_min_frac(&f, 1000); CHECK(f.min_frac == 127 && f.max_frac == 127, "min 1000 → 127 (obtenu %d, %d)", f.min_frac, f.max_frac); }
    { sc2_nf f = n; sc2_nf_max_ent(&f, 2); CHECK(!strcmp(fi(&f, 1997), "97"), "max 2 chiffres entiers : 1997 → « 97 » (%s)", fi(&f, 1997)); }
    { sc2_nf f = n; sc2_nf_min_ent(&f, 0); CHECK(!strcmp(fd(&f, 0.5), ".5"), "min 0 chiffre entier : 0,5 → « .5 » (%s)", fd(&f, 0.5));
      CHECK(!strcmp(fd(&f, 0.0), "0"), "min 0 chiffre entier : 0 → « 0 » (%s)", fd(&f, 0.0)); }
    { sc2_nf f = n; sc2_nf_min_ent(&f, 4); CHECK(!strcmp(fi(&f, 7), "0,007"), "min 4 chiffres entiers : 7 → « 0,007 » (%s)", fi(&f, 7)); }
    { uint16_t t[4]; sc2_nf f = n; int k = sc2_nf_format_entier(&f, 1234567, t, 4);
      CHECK(k == 4 && !strcmp(utf8(t, k), "1,23"), "sortie bornée à 4 unités (obtenu %d « %s »)", k, utf8(t, k)); }

    printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
