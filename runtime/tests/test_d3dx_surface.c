/* test_d3dx_surface.c — passe 9h : cœur de D3DXLoadSurfaceFromMemory (runtime/sc2_d3dx_surface.h), sur l'hôte.
 * Octets écrits vérifiés (A8R8G8B8, X8R8G8B8, R5G6B5, DXT1 brut et décodé), rectangles partiels exacts, gardes de 64
 * octets intactes autour de la destination, entrées invalides refusées SANS lecture hors borne (source posée contre
 * une page PROT_NONE : toute lecture de trop ferait SIGSEGV).
 *   cc -O1 -Wall -Wextra -I runtime tests/test_d3dx_surface.c runtime/sc2_d3d9v.c -lpthread -ldl -o /tmp/t && /tmp/t
 * (recette sans CMake : rien d'ajouté au build). Code de sortie 0 : tout conforme. */
#include "sc2_d3dx_surface.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int g_ok, g_ko;
#define CHECK(cond, msg) do { if (cond) { g_ok++; printf("PASS  %s\n", msg); } else { g_ko++; printf("ÉCHEC %s (ligne %d)\n", msg, __LINE__); } } while (0)

#define GARDE 64
/* destination : GARDE octets 0xCD | taille octets remplis de `fond` | GARDE octets 0xCD */
static uint8_t *dst_neuve(size_t taille, uint8_t fond)
{
    uint8_t *b = malloc(taille + 2 * GARDE);
    memset(b, 0xCD, GARDE); memset(b + GARDE, fond, taille); memset(b + GARDE + taille, 0xCD, GARDE);
    return b + GARDE;
}
static int gardes_ok(const uint8_t *d, size_t taille)
{
    for (int i = 0; i < GARDE; i++) if (d[-1 - i] != 0xCD || d[taille + (size_t)i] != 0xCD) return 0;
    return 1;
}
/* source contre une page interdite : les n octets se terminent exactement au bord */
static uint8_t *src_bornee(const void *donnees, size_t n)
{
    long pg = sysconf(_SC_PAGESIZE);
    size_t pages = (n + (size_t)pg - 1) / (size_t)pg + 1;
    uint8_t *m = mmap(NULL, pages * (size_t)pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(m + (pages - 1) * (size_t)pg, (size_t)pg, PROT_NONE);
    uint8_t *s = m + (pages - 1) * (size_t)pg - n;
    memcpy(s, donnees, n);
    return s;
}
static void argb(uint8_t *o, uint32_t v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24); }
static uint32_t lire32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int main(void)
{
    char note[160];
    /* image source A8R8G8B8 8×4, pitch 40 (8 octets de bourrage par ligne) : pixel (x, y) = 0xAA000000 | y << 16 | x << 8 | (x ^ y) */
    uint8_t img[40 * 4];
    memset(img, 0xEE, sizeof img);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) argb(img + y * 40 + x * 4, 0xAA000000u | (uint32_t)y << 16 | (uint32_t)x << 8 | (uint32_t)(x ^ y));
    sc2x_rect tout = { 0, 0, 8, 4 };
    uint64_t deb, tai;

    /* 1. étendue source */
    CHECK(!sc2x_etendue_source(21, 40, &tout, &deb, &tai) && deb == 0 && tai == 3 * 40 + 32, "étendue source A8R8G8B8 8×4 pitch 40 = [0, 152)");
    sc2x_rect p1 = { 2, 1, 6, 3 };
    CHECK(!sc2x_etendue_source(21, 40, &p1, &deb, &tai) && deb == 40 + 8 && tai == 40 + 16, "étendue source rect (2,1)-(6,3) = [48, 104)");

    /* 2. A8R8G8B8 → A8R8G8B8, tout, pitch 40 → 32 */
    {
        sc2x_etendue_source(21, 40, &tout, &deb, &tai);
        uint8_t *s = src_bornee(img + deb, tai), *d = dst_neuve(32 * 4, 0x11);
        int r = sc2x_copier(s, tai, 21, 40, &tout, d, 21, 32, 8, 4, NULL, 0, note, sizeof note);
        int ok = 1; for (int y = 0; y < 4; y++) if (memcmp(d + y * 32, img + y * 40, 32)) ok = 0;
        CHECK(r == SC2X_OK && ok && gardes_ok(d, 32 * 4), "A8R8G8B8 → A8R8G8B8 : octets exacts, gardes intactes, S_OK");
        printf("      note : %s\n", note);
    }
    /* 3. A8R8G8B8 → X8R8G8B8 : alpha forcé à 0xFF, B G R conservés */
    {
        sc2x_etendue_source(21, 40, &tout, &deb, &tai);
        uint8_t *s = src_bornee(img + deb, tai), *d = dst_neuve(32 * 4, 0x11);
        int r = sc2x_copier(s, tai, 21, 40, &tout, d, 22, 32, 8, 4, NULL, 0, note, sizeof note);
        int ok = 1;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++)
            if (lire32(d + y * 32 + x * 4) != (0xFF000000u | (uint32_t)y << 16 | (uint32_t)x << 8 | (uint32_t)(x ^ y))) ok = 0;
        CHECK(r == SC2X_OK && ok && gardes_ok(d, 32 * 4), "A8R8G8B8 → X8R8G8B8 : alpha 0xFF, couleurs exactes");
    }
    /* 4. A8R8G8B8 → R5G6B5 : rouge, vert, bleu, gris 0x80 */
    {
        uint8_t c[16]; argb(c, 0xFFFF0000u); argb(c + 4, 0xFF00FF00u); argb(c + 8, 0xFF0000FFu); argb(c + 12, 0x80808080u);
        sc2x_rect r4 = { 0, 0, 4, 1 };
        uint8_t *s = src_bornee(c, 16), *d = dst_neuve(8, 0x11);
        int r = sc2x_copier(s, 16, 21, 16, &r4, d, 23, 8, 4, 1, NULL, 0, note, sizeof note);
        uint16_t v[4]; memcpy(v, d, 8);
        CHECK(r == SC2X_OK && v[0] == 0xF800 && v[1] == 0x07E0 && v[2] == 0x001F && v[3] == 0x8410 && gardes_ok(d, 8),
              "A8R8G8B8 → R5G6B5 : F800 07E0 001F 8410");
        printf("      R5G6B5 obtenus : %04X %04X %04X %04X\n", v[0], v[1], v[2], v[3]);
    }
    /* 5. R5G6B5 → R5G6B5 brut, pitch source 10 → destination 8 */
    {
        uint8_t s565[10 * 2]; for (int i = 0; i < 20; i++) s565[i] = (uint8_t)(0x30 + i);
        sc2x_rect r2 = { 0, 0, 4, 2 };
        sc2x_etendue_source(23, 10, &r2, &deb, &tai);
        uint8_t *s = src_bornee(s565, tai), *d = dst_neuve(16, 0x11);
        int r = sc2x_copier(s, tai, 23, 10, &r2, d, 23, 8, 4, 2, NULL, 0, note, sizeof note);
        CHECK(r == SC2X_OK && !memcmp(d, s565, 8) && !memcmp(d + 8, s565 + 10, 8) && gardes_ok(d, 16), "R5G6B5 → R5G6B5 brut, pitches différents");
    }
    /* 6. DXT1 8×8 (2×2 blocs) : bloc k = couleur0 = rouge F800, couleur1 = bleu 001F, indices 0 (blocs 0, 3) ou 1 (1, 2) */
    uint8_t dxt[32];
    for (int k = 0; k < 4; k++) {
        uint8_t *b = dxt + 8 * k; b[0] = 0x00; b[1] = 0xF8; b[2] = 0x1F; b[3] = 0x00;
        memset(b + 4, (k == 1 || k == 2) ? 0x55 : 0x00, 4);
    }
    sc2x_rect t8 = { 0, 0, 8, 8 };
    {
        sc2x_etendue_source(0x31545844u, 16, &t8, &deb, &tai);
        uint8_t *s = src_bornee(dxt, tai), *d = dst_neuve(32, 0x11);
        int r = sc2x_copier(s, tai, 0x31545844u, 16, &t8, d, 0x31545844u, 16, 8, 8, NULL, 0, note, sizeof note);
        CHECK(r == SC2X_OK && tai == 32 && !memcmp(d, dxt, 32) && gardes_ok(d, 32), "DXT1 → DXT1 brut : 4 blocs exacts");
    }
    {
        uint8_t *s = src_bornee(dxt, 32), *d = dst_neuve(32 * 8, 0x11);
        int r = sc2x_copier(s, 32, 0x31545844u, 16, &t8, d, 21, 32, 8, 8, NULL, 0, note, sizeof note);
        int ok = 1;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int bleu = (x >= 4) != (y >= 4);
            if (lire32(d + y * 32 + x * 4) != (bleu ? 0xFF0000FFu : 0xFFFF0000u)) ok = 0;
        }
        CHECK(r == SC2X_OK && ok && gardes_ok(d, 32 * 8), "DXT1 → A8R8G8B8 décodé : rouge / bleu par bloc, exact");
        printf("      note : %s\n", note);
    }
    /* 7. rectangles partiels : source (2,1)-(6,3) → destination (1,2)-(5,4) d'une surface 8×4 ; le reste intact */
    {
        sc2x_etendue_source(21, 40, &p1, &deb, &tai);
        uint8_t *s = src_bornee(img + deb, tai), *d = dst_neuve(32 * 4, 0x11);
        sc2x_rect dr = { 1, 2, 5, 4 };
        int r = sc2x_copier(s, tai, 21, 40, &p1, d, 21, 32, 8, 4, &dr, 0, note, sizeof note);
        int ok = 1;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
            uint32_t v = lire32(d + y * 32 + x * 4);
            int dedans = x >= 1 && x < 5 && y >= 2 && y < 4;
            uint32_t att = dedans ? lire32(img + (y - 2 + 1) * 40 + (x - 1 + 2) * 4) : 0x11111111u;
            if (v != att) ok = 0;
        }
        CHECK(r == SC2X_OK && ok && gardes_ok(d, 32 * 4), "rect source (2,1)-(6,3) → rect destination (1,2)-(5,4) : exact, reste intact");
    }
    /* 8. DXT1 partiel : bloc (1, 0) de la source → bloc (0, 1) de la destination */
    {
        sc2x_rect sb = { 4, 0, 8, 4 }, db = { 0, 4, 4, 8 };
        sc2x_etendue_source(0x31545844u, 16, &sb, &deb, &tai);
        uint8_t *s = src_bornee(dxt + deb, tai), *d = dst_neuve(32, 0x11);
        int r = sc2x_copier(s, tai, 0x31545844u, 16, &sb, d, 0x31545844u, 16, 8, 8, &db, 0, note, sizeof note);
        uint8_t att[32]; memset(att, 0x11, 32); memcpy(att + 16, dxt + 8, 8);
        CHECK(r == SC2X_OK && deb == 8 && tai == 8 && !memcmp(d, att, 32) && gardes_ok(d, 32), "DXT1 rect (4,0)-(8,4) → (0,4)-(4,8) : un bloc, exact");
    }
    /* 9. clé de couleur : les pixels magenta 0xFFFF00FF deviennent 0 */
    {
        uint8_t c[16]; argb(c, 0xFFFF00FFu); argb(c + 4, 0xFF123456u); argb(c + 8, 0xFFFF00FFu); argb(c + 12, 0x7F000000u);
        sc2x_rect r4 = { 0, 0, 4, 1 };
        uint8_t *s = src_bornee(c, 16), *d = dst_neuve(16, 0x11);
        int r = sc2x_copier(s, 16, 21, 16, &r4, d, 21, 16, 4, 1, NULL, 0xFFFF00FFu, note, sizeof note);
        CHECK(r == SC2X_OK && lire32(d) == 0 && lire32(d + 4) == 0xFF123456u && lire32(d + 8) == 0 && lire32(d + 12) == 0x7F000000u && gardes_ok(d, 16),
              "clé de couleur 0xFFFF00FF → noir transparent, autres pixels intacts");
    }
    /* 10. tailles différentes : source 8×4 dans un rectangle destination 2×2 → la partie commune, notée */
    {
        sc2x_etendue_source(21, 40, &tout, &deb, &tai);
        uint8_t *s = src_bornee(img, tai), *d = dst_neuve(32 * 4, 0x11);
        sc2x_rect dr = { 6, 2, 8, 4 };
        int r = sc2x_copier(s, tai, 21, 40, &tout, d, 21, 32, 8, 4, &dr, 0, note, sizeof note);
        int ok = lire32(d + 2 * 32 + 6 * 4) == lire32(img) && lire32(d + 3 * 32 + 7 * 4) == lire32(img + 40 + 4) && lire32(d + 2 * 32 + 5 * 4) == 0x11111111u;
        CHECK(r == SC2X_OK && ok && strstr(note, "tailles différentes") && gardes_ok(d, 32 * 4), "source 8×4 → rect 2×2 : partie commune, notée");
        printf("      note : %s\n", note);
    }
    /* 11. non gérés : DXT5 en destination depuis A8R8G8B8 ; format source inconnu */
    {
        sc2x_etendue_source(21, 40, &tout, &deb, &tai);
        uint8_t *s = src_bornee(img, tai), *d = dst_neuve(64, 0x11);
        int r = sc2x_copier(s, tai, 21, 40, &tout, d, 0x35545844u, 32, 8, 4, NULL, 0, note, sizeof note);
        int intact = 1; for (int i = 0; i < 64; i++) if (d[i] != 0x11) intact = 0;
        CHECK(r == SC2X_NON_GERE && intact && gardes_ok(d, 64), "A8R8G8B8 → DXT5 : NON GÉRÉ, destination intacte");
        printf("      note : %s\n", note);
    }
    /* 12. entrées invalides : refus, aucune écriture, aucune lecture hors borne (la page qui suit est PROT_NONE) */
    {
        uint8_t *d = dst_neuve(32 * 4, 0x11);
        sc2x_etendue_source(21, 40, &tout, &deb, &tai);
        uint8_t *s = src_bornee(img, tai);
        sc2x_rect inv = { 6, 0, 2, 4 }, neg = { -1, 0, 4, 4 }, hors = { 0, 0, 9, 4 }, mal = { 2, 0, 6, 4 };
        int r1 = sc2x_copier(s, tai, 21, 16, &tout, d, 21, 32, 8, 4, NULL, 0, note, sizeof note);           /* pitch 16 < 32 */
        int r2 = sc2x_copier(s, tai, 21, 40, &inv, d, 21, 32, 8, 4, NULL, 0, note, sizeof note);            /* rect inversé */
        int r3 = sc2x_copier(s, tai, 21, 40, &neg, d, 21, 32, 8, 4, NULL, 0, note, sizeof note);            /* négatif */
        int r4 = sc2x_copier(s, tai - 1, 21, 40, &tout, d, 21, 32, 8, 4, NULL, 0, note, sizeof note);       /* plage tronquée */
        int r5 = sc2x_copier(s, tai, 21, 40, &tout, d, 21, 32, 8, 4, &hors, 0, note, sizeof note);          /* destination hors surface */
        int r6 = sc2x_copier(dxt, 32, 0x31545844u, 16, &mal, d, 21, 32, 8, 4, NULL, 0, note, sizeof note);  /* DXT non aligné */
        int r7 = sc2x_copier(NULL, 0, 21, 40, &tout, d, 21, 32, 8, 4, NULL, 0, note, sizeof note);          /* source NULL */
        int r8 = sc2x_copier(s, tai, 21, 40, &tout, NULL, 21, 32, 8, 4, NULL, 0, note, sizeof note);        /* destination NULL */
        int r9 = sc2x_copier(s, tai, 21, 40, &tout, d, 21, 16, 8, 4, NULL, 0, note, sizeof note);           /* pitch destination court */
        int intact = 1; for (int i = 0; i < 32 * 4; i++) if (d[i] != 0x11) intact = 0;
        CHECK(r1 == SC2X_INVALIDE && r2 == SC2X_INVALIDE && r3 == SC2X_INVALIDE && r4 == SC2X_INVALIDE && r5 == SC2X_INVALIDE &&
              r6 == SC2X_INVALIDE && r7 == SC2X_INVALIDE && r8 == SC2X_INVALIDE && r9 == SC2X_INVALIDE && intact && gardes_ok(d, 32 * 4),
              "9 entrées invalides → INVALIDE, destination intacte, aucune faute");
        uint64_t x1, x2;
        CHECK(sc2x_etendue_source(21, 16, &tout, &x1, &x2) == SC2X_INVALIDE && sc2x_etendue_source(0x31545844u, 16, &mal, &x1, &x2) == SC2X_INVALIDE,
              "étendue refusée : pitch < ligne, DXTn non aligné");
    }
    printf("%s (%d/%d)\n", g_ko ? "ÉCHEC" : "PASS", g_ok, g_ok + g_ko);
    return g_ko != 0;
}
