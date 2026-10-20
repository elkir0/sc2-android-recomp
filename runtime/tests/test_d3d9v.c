/* test_d3d9v.c — backend Vulkan headless du D3D9 (passe 5c), sans CPU invité.
 *
 * Device headless (Lavapipe) → back buffer 1280×720 X8R8G8B8 → Clear couleur
 * plein → relecture ; Clear borné au viewport et à un rectangle ; profondeur ;
 * draw « mode lumière » (pipeline réel, aucun pixel modifié) ; Present → PPM.
 * Vulkan absent : le test le dit et sort en SKIP (code 77, convention ctest).
 */
#include "sc2_d3d9v.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- passe 10t : table par époque des plages volatiles — inclusion « table seule » ------------
 * sc2_d3d9v.c ne compile que son bloc autonome de table quand SC2_D3D9V_TABLE_VOLATILS est
 * défini (le backend entier est sous #ifndef) : les fonctions static de la table deviennent
 * testables ici, sans Vulkan ni device — la cible CMake lie par ailleurs le backend complet. */
#define SC2_D3D9V_TABLE_VOLATILS
#include "../runtime/sc2_d3d9v.c"

static int g_pass, g_fail;
#define CHECK(cond, msg) do { if (cond) { g_pass++; printf("PASS  %s\n", msg); } \
                              else { g_fail++; printf("FAIL  %s\n", msg); } } while (0)

#define W 1280
#define H 720

static int pixel_est(const uint8_t *px, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t *p = px + 4 * ((size_t)y * W + x);
    return p[0] == r && p[1] == g && p[2] == b;
}

/* ---- passe 7l : textures — décodeur BC logiciel (règles de la spec), BC natif vs logiciel, mips ---- */
static int px_egal(const uint8_t *p, int r, int g, int b, int a) { return p[0] == r && p[1] == g && p[2] == b && p[3] == a; }
static uint32_t alea = 0x7A11u;
static uint8_t octet(void) { alea = alea * 1103515245u + 12345u; return (uint8_t)(alea >> 16); }
#define DXT1 0x31545844u
#define DXT3 0x33545844u
#define DXT5 0x35545844u
static void tests_textures(void)
{
    uint8_t o[64 * 4];
    /* BC1, c0 > c1 : 4 couleurs ; indices 0,1,2,3 sur les 4 premiers pixels */
    const uint8_t bc1[8] = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0, 0, 0 };
    CHECK(d3dv_decoder_rgba(DXT1, bc1, 4, 4, o) == 0 && px_egal(o, 255, 0, 0, 255) && px_egal(o + 4, 0, 0, 255, 255)
          && px_egal(o + 8, 170, 0, 85, 255) && px_egal(o + 12, 85, 0, 170, 255), "BC1 4 couleurs : c0, c1, 2/3, 1/3");
    /* BC1, c0 <= c1 : 3 couleurs + noir transparent */
    const uint8_t bc1t[8] = { 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0, 0, 0 };
    CHECK(d3dv_decoder_rgba(DXT1, bc1t, 4, 4, o) == 0 && px_egal(o + 8, 127, 0, 127, 255) && px_egal(o + 12, 0, 0, 0, 0),
          "BC1 3 couleurs : moyenne et noir TRANSPARENT");
    /* BC2 : alpha explicite i×17 ; bloc couleur TOUJOURS 4 couleurs (même c0 <= c1) */
    uint8_t bc2[16] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0, 0, 0 };
    CHECK(d3dv_decoder_rgba(DXT3, bc2, 4, 4, o) == 0 && o[3] == 0 && o[4 * 15 + 3] == 255 && o[4 * 7 + 3] == 7 * 17
          && px_egal(o + 12, 170, 0, 85, 51), "BC2 : alpha 4 bits, couleur en mode 4 couleurs (c0 bleu ≤ c1 rouge)");
    /* BC3 : a0 > a1 → 8 niveaux ; a0 <= a1 → 6 niveaux + 0 + 255 */
    uint8_t bc3[16] = { 255, 0, 0x88, 0xC6, 0xFA, 0, 0, 0, 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0 };   /* indices 0..7 */
    CHECK(d3dv_decoder_rgba(DXT5, bc3, 4, 4, o) == 0 && o[3] == 255 && o[7] == 0 && o[11] == 218 && o[31] == 36,
          "BC3 8 niveaux d'alpha (255, 0, 218 … 36)");
    bc3[0] = 0; bc3[1] = 255;
    CHECK(d3dv_decoder_rgba(DXT5, bc3, 4, 4, o) == 0 && o[11] == 51 && o[27] == 0 && o[31] == 255, "BC3 6 niveaux + 0 + 255");
    /* taille non multiple de 4 : 5×3 en DXT1 = 2×1 blocs */
    uint8_t bc5x3[16] = { 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0, 0x1F, 0x00, 0x1F, 0x00, 0, 0, 0, 0 };
    CHECK(d3dv_decoder_rgba(DXT1, bc5x3, 5, 3, o) == 0 && px_egal(o + 4 * (2 * 5 + 3), 255, 0, 0, 255)
          && px_egal(o + 4 * (2 * 5 + 4), 0, 0, 255, 255), "DXT1 5×3 : blocs partiels");
    /* formats non compressés convertis */
    const uint8_t r8g8b8[3] = { 0x30, 0x20, 0x10 }, a4r4[2] = { 0x4C, 0xF8 }, a8l8[2] = { 0x80, 0x40 };
    CHECK(d3dv_decoder_rgba(20, r8g8b8, 1, 1, o) == 0 && px_egal(o, 0x10, 0x20, 0x30, 255), "R8G8B8 → RGBA");
    CHECK(d3dv_decoder_rgba(26, a4r4, 1, 1, o) == 0 && px_egal(o, 0x88, 0x44, 0xCC, 0xFF), "A4R4G4B4 → RGBA");
    CHECK(d3dv_decoder_rgba(51, a8l8, 1, 1, o) == 0 && px_egal(o, 0x80, 0x80, 0x80, 0x40), "A8L8 → RGBA");

    /* BC natif (pilote) contre décodeur logiciel : 64×32, 2 niveaux, données aléatoires (tout bloc est valide) */
    printf("      DXTn : %s\n", d3dv_bc_natif() ? "BC NATIF (décodeur du pilote)" : "décodés par le CPU (repli)");
    static const uint32_t fmts[3] = { DXT1, DXT3, DXT5 };
    for (int f = 0; f < 3; f++) {
        uint32_t tb = f ? 16 : 8, n0 = 16 * 8 * tb, n1 = 8 * 4 * tb;
        uint8_t *src = malloc(n0 + n1), *gpu = malloc(64 * 32 * 4), *cpu = malloc(64 * 32 * 4);
        for (uint32_t i = 0; i < n0 + n1; i++) src[i] = octet();
        int id = d3dv_texture_niveaux(0x7100 + f, 2, 64, 32, 1, fmts[f], 2, src, n0 + n1, 1);
        int ecart = 0, ok = id >= 0;
        for (uint32_t niv = 0; niv < 2 && ok; niv++) {
            uint32_t w = 64 >> niv, h = 32 >> niv;
            ok = d3dv_texture_relire(id, 0, niv, gpu) == 0 && d3dv_decoder_rgba(fmts[f], src + (niv ? n0 : 0), w, h, cpu) == 0;
            for (uint32_t i = 0; ok && i < w * h * 4; i++) { int e = abs(gpu[i] - cpu[i]); if (e > ecart) ecart = e; }
        }
        char m[160];
        snprintf(m, sizeof m, "%s 64×32 + mip 32×16 : GPU = logiciel à ±%d près (tolérance 8 : arrondis d'interpolation)",
                 f == 0 ? "DXT1" : f == 1 ? "DXT3" : "DXT5", ecart);
        CHECK(ok && ecart <= 8, m);
        free(src); free(gpu); free(cpu);
    }
    /* chaîne de mips complète : A8R8G8B8 8×8 → 4 niveaux d'une couleur chacun */
    uint8_t chaine[(64 + 16 + 4 + 1) * 4], *q = chaine;
    static const uint8_t coul[4][4] = { { 10, 20, 30, 255 }, { 40, 50, 60, 255 }, { 70, 80, 90, 255 }, { 100, 110, 120, 255 } };
    for (int l = 0; l < 4; l++) for (int i = 0; i < (8 >> l) * (8 >> l); i++, q += 4) { q[0] = coul[l][2]; q[1] = coul[l][1]; q[2] = coul[l][0]; q[3] = coul[l][3]; }
    int idm = d3dv_texture_niveaux(0x7200, 2, 8, 8, 1, 21, 9 /* trop : borné à 4 */, chaine, sizeof chaine, 1);
    int okm = idm >= 0;
    for (uint32_t l = 0; l < 4 && okm; l++) okm = d3dv_texture_relire(idm, 0, l, o) == 0 && px_egal(o, coul[l][0], coul[l][1], coul[l][2], 255);
    CHECK(okm, "A8R8G8B8 8×8 : 4 niveaux de mip téléversés, chacun relu à sa couleur");
    CHECK(d3dv_texture_relire(idm, 0, 4, o) != 0, "niveau 4 inexistant : refusé");
    /* données tronquées : seuls les niveaux complets passent */
    int idt = d3dv_texture_niveaux(0x7201, 2, 8, 8, 1, 21, 4, chaine, (64 + 16) * 4 + 3, 1);
    CHECK(idt >= 0 && d3dv_texture_relire(idt, 0, 1, o) == 0 && d3dv_texture_relire(idt, 0, 2, o) != 0, "données tronquées : 2 niveaux sur 4");
    /* R8G8B8 (sans équivalent Vulkan) : converti par le CPU au téléversement */
    uint8_t rgb[4 * 4 * 3]; for (int i = 0; i < 16; i++) { rgb[3 * i] = 0x33; rgb[3 * i + 1] = 0x22; rgb[3 * i + 2] = 0x11; }
    int idr = d3dv_texture_niveaux(0x7300, 2, 4, 4, 1, 20, 1, rgb, sizeof rgb, 1);
    CHECK(idr >= 0 && d3dv_texture_relire(idr, 0, 0, o) == 0 && px_egal(o + 4 * 5, 0x11, 0x22, 0x33, 255), "R8G8B8 4×4 : converti et téléversé");
    /* cube : 6 faces × 2 niveaux (4×4 puis 2×2) en A8R8G8B8, une couleur par face et par niveau, ordre DDS */
    uint8_t cube[6 * (16 + 4) * 4], *c = cube;
    for (int f = 0; f < 6; f++) for (int l = 0; l < 2; l++) for (int i = 0; i < (l ? 4 : 16); i++, c += 4)
        { c[0] = (uint8_t)(l * 100); c[1] = (uint8_t)(f * 40); c[2] = (uint8_t)(200 - f * 30); c[3] = 255; }   /* B, G, R, A */
    int idc = d3dv_texture_niveaux(0x7400, 3, 4, 4, 1, 21, 2, cube, sizeof cube, 1), okc = idc >= 0;
    for (uint32_t f = 0; f < 6 && okc; f++) for (uint32_t l = 0; l < 2 && okc; l++)
        okc = d3dv_texture_relire(idc, f, l, o) == 0 && px_egal(o, 200 - f * 30, f * 40, l * 100, 255);
    CHECK(okc, "cube 4×4 A8R8G8B8 : 6 faces × 2 niveaux téléversés, chaque face/niveau relu à sa couleur");
    /* cube DXT1 en repli ou natif : face 5 relue = décodage logiciel */
    uint8_t cdx[6 * 8]; for (int i = 0; i < 48; i++) cdx[i] = octet();
    int idx = d3dv_texture_niveaux(0x7401, 3, 4, 4, 1, DXT1, 1, cdx, sizeof cdx, 1), e5 = 0;
    uint8_t g5[64], c5[64];
    int okx = idx >= 0 && d3dv_texture_relire(idx, 5, 0, g5) == 0 && d3dv_decoder_rgba(DXT1, cdx + 40, 4, 4, c5) == 0;
    for (int i = 0; okx && i < 64; i++) { int e = abs(g5[i] - c5[i]); if (e > e5) e5 = e; }
    CHECK(okx && e5 <= 8, "cube DXT1 : face 5 = décodage logiciel (±8)");
}

/* ---- passe 10t : table des plages volatiles (insertion, recherche, époque périmée, collision) ---- */
static void tests_table_volatils(void)
{
    memset(g_volatils, 0, sizeof g_volatils);
    uint64_t off = 0;
    volatils_noter(0x1111, 3, 64, 512, VOLATILS_FLUX, 7, 4096);
    CHECK(volatils_trouver(0x1111, 3, 64, 512, VOLATILS_FLUX, 7, &off) == 1 && off == 4096,
          "table volatils : insertion puis recherche à la même époque");
    CHECK(!volatils_trouver(0x1111, 3, 64, 512, VOLATILS_FLUX, 8, &off), "table volatils : époque périmée → absente");
    CHECK(!volatils_trouver(0x1111, 4, 64, 512, VOLATILS_FLUX, 7, &off), "table volatils : génération différente → absente");
    CHECK(!volatils_trouver(0x1111, 3, 128, 512, VOLATILS_FLUX, 7, &off), "table volatils : décalage différent → absent");
    CHECK(!volatils_trouver(0x1111, 3, 64, 256, VOLATILS_FLUX, 7, &off), "table volatils : taille différente → absente");
    volatils_noter(0x1111, 3, 64, 512, VOLATILS_INDEX, 7, 2048);   /* le genre fait partie de la clé */
    CHECK(volatils_trouver(0x1111, 3, 64, 512, VOLATILS_INDEX, 7, &off) && off == 2048
          && volatils_trouver(0x1111, 3, 64, 512, VOLATILS_FLUX, 7, &off) && off == 4096,
          "table volatils : index ≠ flux pour la même plage — les deux cohabitent");
    volatils_noter(0x1111, 3, 64, 512, VOLATILS_FLUX, 8, 8192);     /* réécrite à la nouvelle époque */
    CHECK(volatils_trouver(0x1111, 3, 64, 512, VOLATILS_FLUX, 8, &off) && off == 8192
          && !volatils_trouver(0x1111, 3, 64, 512, VOLATILS_FLUX, 7, &off),
          "table volatils : réécriture à la nouvelle époque (case rafraîchie, l'ancienne époque reste absente)");

    /* collision déterministe : clés adverses de la même case de départ (sondage linéaire) */
    uint32_t h0 = volatils_hache(0x2222, 1, 0, 32, VOLATILS_FLUX), adv[2] = { 0, 0 };
    for (uint32_t o = 0x2223, n = 0; o < 0x2222 + 100000u && n < 2; o++)
        if (volatils_hache(o, 1, 0, 32, VOLATILS_FLUX) == h0) adv[n++] = o;
    CHECK(adv[0] && adv[1], "table volatils : collision construite (trois clés de même hachage)");
    if (adv[0] && adv[1]) {
        memset(g_volatils, 0, sizeof g_volatils);
        volatils_noter(0x2222, 1, 0, 32, VOLATILS_FLUX, 9, 100);    /* case h0 */
        volatils_noter(adv[0], 1, 0, 32, VOLATILS_FLUX, 9, 200);    /* case h0 + 1 */
        CHECK(volatils_trouver(adv[0], 1, 0, 32, VOLATILS_FLUX, 9, &off) && off == 200
              && volatils_trouver(0x2222, 1, 0, 32, VOLATILS_FLUX, 9, &off) && off == 100,
              "table volatils : collision — les deux clés cohabitent et se retrouvent chacune à sa place");
        /* époque suivante : les deux cases sont périmées ; la troisième clé recycle h0, la
         * seconde se rafraîchit en h0 + 1 — la première (écrasée) disparaît pour de bon */
        volatils_noter(adv[1], 1, 0, 32, VOLATILS_FLUX, 10, 300);
        CHECK(volatils_trouver(adv[1], 1, 0, 32, VOLATILS_FLUX, 10, &off) && off == 300
              && !volatils_trouver(0x2222, 1, 0, 32, VOLATILS_FLUX, 10, &off)
              && !volatils_trouver(adv[0], 1, 0, 32, VOLATILS_FLUX, 10, &off),
              "table volatils : case périmée recyclée par la clé suivante, l'ancienne clé est absente");
        volatils_noter(adv[0], 1, 0, 32, VOLATILS_FLUX, 10, 400);   /* rafraîchie au-delà de la case courante */
        CHECK(volatils_trouver(adv[0], 1, 0, 32, VOLATILS_FLUX, 10, &off) && off == 400
              && volatils_trouver(adv[1], 1, 0, 32, VOLATILS_FLUX, 10, &off) && off == 300,
              "table volatils : rafraîchissement derrière une case de l'époque courante");
    }
}

int main(void)
{
    tests_table_volatils();   /* passe 10t : pure, sans Vulkan — tourne même si le backend est absent */
    if (!d3dv_init()) { printf("SKIP  backend Vulkan indisponible\n"); return 77; }
    printf("device : %s\n", d3dv_nom_device());
    CHECK(d3dv_actif(), "backend actif");

    int bb = d3dv_cible(0x1000, W, H, 22 /* X8R8G8B8 */, 0);
    int ds = d3dv_cible(0x2000, W, H, 75 /* D24S8 */, 0);
    CHECK(bb >= 0, "back buffer 1280×720 créé");
    CHECK(ds >= 0, "surface de profondeur D24S8 créée");
    CHECK(d3dv_cible(0x1000, W, H, 22, 0) == bb, "même clé → même cible");
    d3dv_rt(bb); d3dv_ds(ds);
    d3dv_viewport(0, 0, W, H, 0.f, 1.f);

    uint8_t *px = malloc((size_t)W * H * 4);
    d3dv_scene(1);
    d3dv_clear(0, NULL, D3DV_CLEAR_TARGET | D3DV_CLEAR_ZBUFFER | D3DV_CLEAR_STENCIL, 0xFF336699u, 1.f, 0);
    d3dv_scene(0);
    CHECK(d3dv_lire(bb, px) == 0, "relecture du back buffer");
    CHECK(pixel_est(px, 0, 0, 0x33, 0x66, 0x99) && pixel_est(px, W - 1, H - 1, 0x33, 0x66, 0x99)
          && pixel_est(px, W / 2, H / 2, 0x33, 0x66, 0x99), "Clear plein : 0xFF336699 partout");

    /* Viewport réduit + rectangle : seule l'intersection change. */
    d3dv_viewport(100, 100, 200, 200, 0.f, 1.f);
    int32_t r[4] = { 0, 0, 200, 150 };      /* ∩ viewport = [100,200) × [100,150) */
    d3dv_clear(1, r, D3DV_CLEAR_TARGET, 0xFFFF0000u, 1.f, 0);
    CHECK(d3dv_lire(bb, px) == 0, "relecture après Clear partiel");
    CHECK(pixel_est(px, 150, 120, 0xFF, 0, 0), "Clear partiel : pixel dans viewport ∩ rectangle rouge");
    CHECK(pixel_est(px, 50, 120, 0x33, 0x66, 0x99), "Clear partiel : pixel hors viewport inchangé");
    CHECK(pixel_est(px, 150, 160, 0x33, 0x66, 0x99), "Clear partiel : pixel hors rectangle inchangé");

    /* Draw « lumière » : pipeline réel, triangle dégénéré → aucun pixel. */
    d3dv_viewport(0, 0, W, H, 0.f, 1.f);
    for (int i = 0; i < 10; i++) d3dv_draw(4 /* TRIANGLELIST */, 100);
    CHECK(d3dv_lire(bb, px) == 0, "relecture après 10 draws « lumière »");
    CHECK(pixel_est(px, 640, 360, 0x33, 0x66, 0x99) && pixel_est(px, 150, 120, 0xFF, 0, 0),
          "draws « lumière » exécutés sans modifier les pixels");

    /* Cible flottante (A16B16G16R16F) : création, Clear, draw — sans relecture. */
    int hdr = d3dv_cible(0x3000, 512, 512, 113, 0);
    CHECK(hdr >= 0, "cible A16B16G16R16F créée");
    d3dv_rt(hdr);
    d3dv_viewport(0, 0, 512, 512, 0.f, 1.f);
    d3dv_clear(0, NULL, D3DV_CLEAR_TARGET, 0x80808080u, 1.f, 0);
    d3dv_draw(4, 2);
    d3dv_rt(bb);

    /* Present → PPM, moyenne RGB. */
    double moy[3] = { 0, 0, 0 };
    CHECK(d3dv_present(bb, "test_d3d9v.ppm", moy) == 0, "Present avec écriture PPM");
    printf("      moyenne RGB du PPM : %.2f %.2f %.2f\n", moy[0], moy[1], moy[2]);
    CHECK(moy[0] > 50 && moy[1] > 100 && moy[2] > 150, "moyenne RGB non nulle (≈ 0x33/0x66/0x99)");
    FILE *f = fopen("test_d3d9v.ppm", "rb");
    char tete[32] = ""; size_t lu = f ? fread(tete, 1, 16, f) : 0;
    if (f) fclose(f);
    CHECK(lu == 16 && !strncmp(tete, "P6\n1280 720\n255\n", 16), "en-tête PPM P6 1280×720");
    CHECK(d3dv_present(bb, NULL, NULL) == 0, "Present sans relecture");

    tests_textures();
    d3dv_bilan();
    free(px);
    printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
