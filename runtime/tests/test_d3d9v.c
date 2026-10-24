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
#include "../sc2_d3d9v.c"

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

/* ---- V5.1 (24/10) : oubli d'une texture rendue à l'arène (d3dv_oublier) ----
 * Une clé (adresse invitée) reprise par un objet NEUF de même génération : sans oubli, l'image de l'ancien est servie
 * (génération égale → pas de re-téléversement) ; après oubli, l'objet neuf a la sienne et l'indice libéré est repris.
 * 3 000 cycles création/oubli : pierres tombales et tables refaites, une texture vivante témoin reste trouvée. */
static void tests_oubli(void)
{
    uint8_t a[4 * 4 * 4], b[4 * 4 * 4], o[4 * 4 * 4];
    for (int i = 0; i < 16; i++) { a[4 * i] = 10; a[4 * i + 1] = 20; a[4 * i + 2] = 30; a[4 * i + 3] = 255;
                                   b[4 * i] = 200; b[4 * i + 1] = 100; b[4 * i + 2] = 50; b[4 * i + 3] = 255; }
    int id = d3dv_texture_niveaux(0x7500, 2, 4, 4, 1, 21, 1, a, sizeof a, 2);
    int id2 = d3dv_texture_niveaux(0x7500, 2, 4, 4, 1, 21, 1, b, sizeof b, 2);
    CHECK(id >= 0 && id2 == id && d3dv_texture_relire(id, 0, 0, o) == 0 && px_egal(o, 30, 20, 10, 255),
          "même clé, même génération, sans oubli : l'image de l'ancien objet reste (le danger)");
    uint32_t cle = 0x7500; d3dv_oublier(&cle, 1);
    int id3 = d3dv_texture_niveaux(0x7500, 2, 4, 4, 1, 21, 1, b, sizeof b, 2);
    CHECK(id3 == id && d3dv_texture_relire(id3, 0, 0, o) == 0 && px_egal(o, 50, 100, 200, 255),
          "après d3dv_oublier : objet neuf à la même clé téléversé, indice libéré repris");
    int temoin = d3dv_texture_niveaux(0x7600, 2, 4, 4, 1, 21, 1, a, sizeof a, 1), ok = temoin >= 0;
    for (uint32_t k = 0; k < 3000 && ok; k++) {
        uint32_t c = 0x10000u + 16u * k;
        ok = d3dv_texture_niveaux(c, 2, 4, 4, 1, 21, 1, (k & 1) ? a : b, sizeof a, 1) >= 0;
        d3dv_oublier(&c, 1);
        if (!(k % 500)) ok = ok && d3dv_texture_niveaux(0x7600, 2, 4, 4, 1, 21, 1, a, sizeof a, 1) == temoin;
    }
    CHECK(ok && d3dv_texture_relire(temoin, 0, 0, o) == 0 && px_egal(o, 30, 20, 10, 255),
          "3 000 créations/oublis : pierres tombales, tables refaites, texture témoin intacte et trouvée");
    d3dv_oublier(&cle, 1); cle = 0x7600; d3dv_oublier(&cle, 1);
    /* revue adverse (MAJEUR 1) : CIBLES de rendu d'un objet rendu — clé adresse << 32 | taille (niveaux d'une texture RT)
     * ou adresse (surface autonome). 700 objets successifs, chacun oublié : sans oubli, MAX_CIBLES (512) atteint → −1. */
    int ok_c = 1, premiere = -1, id_max = -1;
    for (uint32_t k = 0; k < 700 && ok_c; k++) {
        uint32_t obj = 0x20000000u + 64u * k;
        int a1 = d3dv_cible(((uint64_t)obj << 32) | (64u << 16) | 64u, 64, 64, 21, 0);
        int a2 = d3dv_cible(((uint64_t)obj << 32) | (32u << 16) | 32u, 32, 32, 21, 0);
        int a3 = d3dv_cible(obj + 16u, 16, 16, 75 /* D24S8 */, 0);
        ok_c = a1 >= 0 && a2 >= 0 && a3 >= 0;
        if (!k) premiere = a1;
        if (a1 > id_max) id_max = a1; if (a2 > id_max) id_max = a2; if (a3 > id_max) id_max = a3;
        uint32_t lot[2] = { obj, obj + 16u };
        d3dv_oublier(lot, 2);
    }
    int id_c = d3dv_cible(((uint64_t)0x20000000u << 32) | (64u << 16) | 64u, 64, 64, 21, 0);
    char m_c[240]; snprintf(m_c, sizeof m_c, "cibles : 700 objets × 3 cibles (niveaux d'une texture RT, surface de profondeur), "
             "chacun oublié — jamais −1 (MAX_CIBLES), indices repris : plus grand %d, %d au départ", id_max, premiere);
    CHECK(ok_c && id_c >= 0 && id_max <= premiere + 2, m_c);
    /* une cible EN USAGE (RT courante) n'est pas oubliée : même indice, image gardée */
    int rt = d3dv_cible(0x7700, 32, 32, 22, 0); d3dv_rt(rt);
    d3dv_viewport(0, 0, 32, 32, 0.f, 1.f);
    d3dv_scene(1); d3dv_clear(0, NULL, D3DV_CLEAR_TARGET, 0xFF123456u, 1.f, 0); d3dv_scene(0);
    cle = 0x7700; d3dv_oublier(&cle, 1);
    uint8_t *px32 = malloc(32 * 32 * 4);
    CHECK(rt >= 0 && d3dv_cible(0x7700, 32, 32, 22, 0) == rt && px32 && d3dv_lire(rt, px32) == 0 && px32[4 * (5 * 32 + 5)] == 0x12
          && px32[4 * (5 * 32 + 5) + 1] == 0x34 && px32[4 * (5 * 32 + 5) + 2] == 0x56,
          "cible en usage (RT courante) gardée par l'oubli : même indice, contenu intact (0x123456)");
    free(px32); d3dv_rt(-1); d3dv_oublier(&cle, 1);
    uint32_t lot0 = 0x20000000u; d3dv_oublier(&lot0, 1);
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

/* Passe 10t : plage de constantes lue (nconst) — étendue recopiée et réutilisation du bloc en cache. Pur. */
static void tests_plage_consts(void)
{
    CHECK(consts_a_copier(10, 0) == 160 && consts_a_copier(1, 0) == 16 && consts_a_copier(256, 0) == 4096,
          "plage consts : 16 × nconst octets recopiés");
    CHECK(consts_a_copier(0, 0) == D3DV_CONST_TAILLE && consts_a_copier(257, 0) == D3DV_CONST_TAILLE,
          "plage consts : 0 (bloc entier) et valeur hors borne → bloc entier");
    CHECK(consts_a_copier(10, 1) == D3DV_CONST_TAILLE, "plage consts : SC2_VK_CONSTS_COMPLETES → bloc entier");
    /* bloc recopié à l'époque 7, génération 42 */
    CHECK(!bloc_consts_reutilisable(7, 42, consts_a_copier(10, 0), 7, 42, consts_a_copier(50, 0)),
          "cache consts : nconst 10 recopié, dessin à 50 → recopie");
    CHECK(bloc_consts_reutilisable(7, 42, consts_a_copier(50, 0), 7, 42, consts_a_copier(20, 0)),
          "cache consts : 50 recopiés, dessin à 20 → réutilisé");
    CHECK(!bloc_consts_reutilisable(7, 42, consts_a_copier(50, 0), 7, 42, consts_a_copier(0, 0)),
          "cache consts : dessin à bloc entier → recopie");
    CHECK(bloc_consts_reutilisable(7, 42, consts_a_copier(0, 0), 7, 42, consts_a_copier(256, 0)),
          "cache consts : bloc entier recopié → sert tout dessin de même génération");
    CHECK(!bloc_consts_reutilisable(7, 42, consts_a_copier(50, 0), 7, 43, consts_a_copier(20, 0)),
          "cache consts : autre génération → recopie");
    CHECK(!bloc_consts_reutilisable(7, 42, consts_a_copier(50, 0), 8, 42, consts_a_copier(20, 0)),
          "cache consts : autre époque de l'anneau → recopie");
}

/* ---- V2.2 (22/10) : instanciation — UN dessin instancié contre N dessins de référence (MoltenVK au Mac) ------------
 * Shaders du test (les nôtres, tests/shaders/inst.{vert,frag}) : coin du quad par sommet (liaison 0) ; centre, demi-
 * taille et teinte par instance (liaison 1, enregistrement de 24 octets). Référence : N dessins NON instanciés
 * (instances 0, flux_inst 0) dont le flux 1 porte 4 copies de l'enregistrement i, lues par sommet (stride 24 : même
 * clé de pipeline au taux près). Scènes jugées octet par octet (d3dv_lire) :
 *   S1 grille 8×8 opaque ; S2 516 instances qui se chevauchent, en mélange alpha (ordre des instances = ordre des
 *   primitives) ; S3 flux d'instance par le cache statique (création puis réutilisation, décalage 48 dans le VB) ;
 *   S4 chemin d'avant (instances 0 : le flux d'instance lu par sommet) ≠ référence — le test discrimine ;
 *   S5 vertexOffset 4 : le flux d'instance n'est pas décalé ; S6 le taux est dans la clé (pipeline RATE_VERTEX créé
 *   d'abord par la référence, puis un pipeline RATE_INSTANCE distinct). Compteurs : +1 dessin et +N instances par
 *   dessin instancié, 0 pour la référence. */
#include <vulkan/vulkan.h>
#include "shaders/inst_vert.h"
#include "shaders/inst_frag.h"
#define IW 256
#define IH 256
typedef struct { float cx, cy, dx, dy; uint8_t rgba[4]; uint32_t bourre; } inst_rec;   /* 24 octets */
static uint8_t g_cvs_nul[D3DV_CONST_TAILLE], g_cps_nul[D3DV_CONST_TAILLE];
static int g_ivs = -1, g_ips = -1, g_icible = -1;
static const float g_coins[16] = { 9, 9, -9, 9, 9, -9, -9, -9,     /* éléments 0-3 : leurres (S5 lit 4-7) */
                                   -1, -1, 1, -1, -1, 1, 1, 1 };
static const uint16_t g_quad[6] = { 0, 1, 2, 2, 1, 3 };
typedef struct { uint32_t obj, decalage, taille_tot; const void *tot; int32_t dsommet; uint32_t instances, flux_inst; } inst_opt;
static void inst_base(d3dv_dessin *d, int blend)
{
    memset(d, 0, sizeof *d);
    d->vs = g_ivs; d->ps = g_ips; d->topo = 4;                   /* TRIANGLELIST */
    d->nattr = 4;
    d->attr[0].loc = 0; d->attr[0].flux = 0; d->attr[0].vkfmt = VK_FORMAT_R32G32_SFLOAT; d->attr[0].offset = 0;
    d->attr[1].loc = 1; d->attr[1].flux = 1; d->attr[1].vkfmt = VK_FORMAT_R32G32_SFLOAT; d->attr[1].offset = 0;
    d->attr[2].loc = 2; d->attr[2].flux = 1; d->attr[2].vkfmt = VK_FORMAT_R32G32_SFLOAT; d->attr[2].offset = 8;
    d->attr[3].loc = 3; d->attr[3].flux = 1; d->attr[3].vkfmt = VK_FORMAT_R8G8B8A8_UNORM; d->attr[3].offset = 16;
    d->nflux = 2;
    d->flux[0].stride = 8; d->flux[0].donnees = g_coins + 8; d->flux[0].taille = 32;
    d->index = g_quad; d->index_taille = sizeof g_quad; d->nombre = 6;
    d->cvs = g_cvs_nul; d->cps = g_cps_nul;
    d->masque_couleur = 0xF; d->cull = 1;                        /* D3DCULL_NONE, ni profondeur ni stencil */
    if (blend) { d->blend = 1; d->src = d->src_a = 5; d->dst = d->dst_a = 6; d->op = d->op_a = 1; }   /* SRCALPHA, INVSRCALPHA */
    else { d->src = d->src_a = 2; d->dst = d->dst_a = 1; d->op = d->op_a = 1; }
}
static void inst_effacer(void)
{
    d3dv_rt(g_icible); d3dv_ds(-1); d3dv_viewport(0, 0, IW, IH, 0.f, 1.f);
    d3dv_clear(0, NULL, D3DV_CLEAR_TARGET, 0xFF000000u, 1.f, 0);
}
static int inst_reference(const inst_rec *r, uint32_t n, int blend, uint8_t *px)
{
    inst_effacer();
    inst_rec q[4];
    for (uint32_t i = 0; i < n; i++) {
        d3dv_dessin d; inst_base(&d, blend);
        for (int k = 0; k < 4; k++) q[k] = r[i];
        d.flux[1].stride = 24; d.flux[1].donnees = q; d.flux[1].taille = sizeof q;   /* recopié dans l'anneau au dessin */
        if (d3dv_dessiner(&d)) return -1;
    }
    return d3dv_lire(g_icible, px);
}
static int inst_dessin(const inst_rec *r, uint32_t n, int blend, const inst_opt *o, uint8_t *px)
{
    inst_effacer();
    d3dv_dessin d; inst_base(&d, blend);
    if (o->dsommet) { d.flux[0].donnees = g_coins; d.flux[0].taille = sizeof g_coins; d.decalage_sommet = o->dsommet; }
    d.flux[1].stride = 24; d.flux[1].donnees = r; d.flux[1].taille = 24 * n;
    if (o->obj) {                                                /* passe 10r : tampon invité statique (cache, dalle) */
        d.flux[1].obj = o->obj; d.flux[1].gen = 1; d.flux[1].usage = 0; d.flux[1].taille_tot = o->taille_tot;
        d.flux[1].donnees_tot = o->tot; d.flux[1].decalage = o->decalage;
    }
    d.instances = o->instances; d.flux_inst = o->flux_inst;
    if (d3dv_dessiner(&d)) return -1;
    return d3dv_lire(g_icible, px);
}
static uint32_t inst_ecarts(const uint8_t *a, const uint8_t *b)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < IW * IH * 4; i++) n += a[i] != b[i];
    return n;
}
#define CHECKF(cond, ...) do { char m_[320]; snprintf(m_, sizeof m_, __VA_ARGS__); CHECK(cond, m_); } while (0)
static uint32_t g_inst_alea;
static uint32_t inst_alea(void) { g_inst_alea = g_inst_alea * 1103515245u + 12345u; return (g_inst_alea >> 8) & 0xFFFF; }
static void tests_instanciation_gpu(void)
{
    g_ivs = d3dv_module(spv_inst_vert, sizeof spv_inst_vert / 4, 0);
    g_ips = d3dv_module(spv_inst_frag, sizeof spv_inst_frag / 4, 1);
    g_icible = d3dv_cible(0x5000, IW, IH, 22 /* X8R8G8B8 */, 0);
    CHECKF(g_ivs >= 0 && g_ips >= 0 && g_icible >= 0, "V2.2 : modules inst.vert/inst.frag et cible 256×256 créés");
    if (g_ivs < 0 || g_ips < 0 || g_icible < 0) return;
    uint8_t *ref = malloc(IW * IH * 4), *img = malloc(IW * IH * 4);
    static inst_rec grille[64], nuee[516];
    for (int i = 0; i < 64; i++)
        grille[i] = (inst_rec){ -1.f + (2.f * (float)(i % 8) + 1.f) / 8.f, -1.f + (2.f * (float)(i / 8) + 1.f) / 8.f, 0.1f, 0.1f,
                                { (uint8_t)(32 * (i % 8) + 16), (uint8_t)(32 * (i / 8) + 16), 200, 255 }, 0xDEADBEEFu };
    uint64_t d0, s0, d1, s1, d2, s2; int p0, p1, p2, q0, q2; uint32_t ecarts;
    d3dv_compteurs_inst(&d0, &s0, NULL, &p0, &q0);
    /* S1 et S6 : la référence crée d'abord le pipeline RATE_VERTEX */
    int ok = inst_reference(grille, 64, 0, ref) == 0;
    d3dv_compteurs_inst(&d1, &s1, NULL, &p1, NULL);
    int cellules = 0;
    for (int i = 0; i < 64; i++) {
        const uint8_t *p = ref + 4 * ((size_t)(32 * (i / 8) + 16) * IW + 32 * (i % 8) + 16);
        cellules += p[0] == grille[i].rgba[0] && p[1] == grille[i].rgba[1] && p[2] == 200;
    }
    const uint8_t *coin = ref + 4 * ((size_t)2 * IW + 2);
    CHECKF(ok && cellules == 64 && !coin[0] && !coin[1] && !coin[2] && d1 == d0 && s1 == s0,
          "V2.2 S1 référence : 64 dessins non instanciés, 64 cellules à leur teinte, fond noir, compteurs d'instances inchangés");
    inst_opt o = { .instances = 64, .flux_inst = 2 };
    ok = inst_dessin(grille, 64, 0, &o, img) == 0;
    d3dv_compteurs_inst(&d2, &s2, NULL, &p2, &q2);
    ecarts = inst_ecarts(img, ref);
    CHECKF(ok && !ecarts, "V2.2 S1 : UN dessin de 64 instances (grille 8×8) = 64 dessins de référence, octet pour octet (%u octets "
                         "différents)", ecarts);
    CHECKF(d2 == d1 + 1 && s2 == s1 + 64, "V2.2 compteurs : +1 dessin à flux d'instance, +64 instances (%llu, %llu)",
          (unsigned long long)(d2 - d1), (unsigned long long)(s2 - s1));
    CHECKF(p1 - p0 == 1 && p2 - p1 == 1 && q2 == q0 + 1, "V2.2 S6 : taux dans la clé — pipeline RATE_VERTEX (référence, +%d) puis "
          "pipeline RATE_INSTANCE distinct (+%d, dont %d à flux d'instance)", p1 - p0, p2 - p1, q2 - q0);
    /* S4 : chemin d'avant — le test discrimine */
    inst_opt avant = { 0 };
    ok = inst_dessin(grille, 64, 0, &avant, img) == 0;
    ecarts = inst_ecarts(img, ref);
    CHECKF(ok && ecarts > 1000, "V2.2 S4 : chemin d'avant (instances 0, flux d'instance lu par sommet) ≠ référence (%u octets)", ecarts);
    /* S5 : vertexOffset 4 sur la géométrie, flux d'instance non décalé */
    inst_opt o5 = { .dsommet = 4, .instances = 64, .flux_inst = 2 };
    ok = inst_dessin(grille, 64, 0, &o5, img) == 0;
    ecarts = inst_ecarts(img, ref);
    CHECKF(ok && !ecarts, "V2.2 S5 : vertexOffset 4 (géométrie aux éléments 4-7), flux d'instance non décalé : image de S1 (%u)", ecarts);
    /* S3 : flux d'instance servi par le cache statique (création, puis réutilisation), à 48 octets du début du VB */
    static uint8_t vb[48 + 64 * 24 + 24];
    memset(vb, 0x5A, sizeof vb); memcpy(vb + 48, grille, sizeof grille);
    inst_opt o3 = { .obj = 0x77770000u, .decalage = 48, .taille_tot = sizeof vb, .tot = vb, .instances = 64, .flux_inst = 2 };
    uint32_t e3a, e3b;
    ok = inst_dessin((const inst_rec *)(vb + 48), 64, 0, &o3, img) == 0; e3a = inst_ecarts(img, ref);
    ok &= inst_dessin((const inst_rec *)(vb + 48), 64, 0, &o3, img) == 0; e3b = inst_ecarts(img, ref);
    CHECKF(ok && !e3a && !e3b, "V2.2 S3 : flux d'instance par le cache statique (décalage 48), création puis réutilisation : image "
                              "de S1 (%u, %u)", e3a, e3b);
    /* V5.1 (revue adverse, MAJEUR 2) : branche TAMPONS de d3dv_oublier — même objet, même génération, AUTRES données
     * (un VB neuf à l'adresse d'un VB rendu) : sans oubli, le cache sert l'ancien tampon ; après oubli, le nouveau. */
    static inst_rec grille2[64]; static uint8_t vb2[sizeof vb]; uint8_t *ref2 = malloc(IW * IH * 4);
    for (int i = 0; i < 64; i++) { grille2[i] = grille[i]; grille2[i].rgba[2] = 40; grille2[i].rgba[1] = (uint8_t)(255 - grille[i].rgba[1]); }
    memset(vb2, 0x5A, sizeof vb2); memcpy(vb2 + 48, grille2, sizeof grille2);
    int okr = inst_reference(grille2, 64, 0, ref2) == 0;
    inst_opt o3b = o3; o3b.tot = vb2;
    uint32_t avant_oubli, apres_oubli;
    ok = inst_dessin((const inst_rec *)(vb2 + 48), 64, 0, &o3b, img) == 0; avant_oubli = inst_ecarts(img, ref2);
    uint32_t xvb = 0x77770000u; d3dv_oublier(&xvb, 1);
    ok &= inst_dessin((const inst_rec *)(vb2 + 48), 64, 0, &o3b, img) == 0; apres_oubli = inst_ecarts(img, ref2);
    CHECKF(okr && ok && avant_oubli > 1000 && !apres_oubli, "V5.1 tampons : même VB et même génération, autres données — sans "
          "oubli l'ancien tampon est servi (%u octets d'écart), après d3dv_oublier le nouveau (%u)", avant_oubli, apres_oubli);
    free(ref2); d3dv_oublier(&xvb, 1);
    /* S2 : 516 instances qui se chevauchent, en mélange alpha */
    g_inst_alea = 0x22102026u;
    for (int i = 0; i < 516; i++) {
        nuee[i].cx = (float)inst_alea() / 65535.f * 1.2f - 0.6f; nuee[i].cy = (float)inst_alea() / 65535.f * 1.2f - 0.6f;
        nuee[i].dx = 0.05f + (float)inst_alea() / 65535.f * 0.35f; nuee[i].dy = 0.05f + (float)inst_alea() / 65535.f * 0.35f;
        for (int c = 0; c < 3; c++) nuee[i].rgba[c] = (uint8_t)inst_alea();
        nuee[i].rgba[3] = (uint8_t)(64 + inst_alea() % 128); nuee[i].bourre = 0;
    }
    ok = inst_reference(nuee, 516, 1, ref) == 0;
    uint32_t nonnoirs = 0;
    for (uint32_t i = 0; i < IW * IH; i++) nonnoirs += ref[4 * i] || ref[4 * i + 1] || ref[4 * i + 2];
    inst_opt o2 = { .instances = 516, .flux_inst = 2 };
    ok &= inst_dessin(nuee, 516, 1, &o2, img) == 0;
    ecarts = inst_ecarts(img, ref);
    CHECKF(ok && !ecarts && nonnoirs > IW * IH / 4, "V2.2 S2 : 516 instances en mélange alpha = 516 dessins de référence, octet pour "
          "octet (%u différents ; %u pixels couverts sur %u)", ecarts, nonnoirs, IW * IH);
    free(ref); free(img);
}

int main(void)
{
    tests_table_volatils();   /* passe 10t : pure, sans Vulkan — tourne même si le backend est absent */
    tests_plage_consts();     /* passe 10t : idem */
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
    tests_oubli();               /* V5.1 */
    tests_instanciation_gpu();   /* V2.2 */
    d3dv_bilan();
    free(px);
    printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
