/* test_ddraw.c — V2.5a (23/10) : décisions de l'IDirectDraw7 factice (recomp-sc2/runtime/sc2_ddraw.h).
 *
 * Garde du canon : en profil historique (DeviceId 0x0402) sans SC2_DDRAW, DirectDraw est REFUSÉ, sur toutes les
 * plateformes ; en profil moderne il est servi hors Android ; Android le refuse par défaut (garde-fou tant que le PSS
 * du Fold n'est pas mesuré en textures High/Ultra) ; SC2_DDRAW=0|1 force le refus ou le service, tous profils.
 * Mémoire : SC2_VRAM_MO de 1 à 4095, sinon 1024 (moderne) / 512 (historique) ; GetAvailableVidMem rend 0 en mémoire
 * non locale. Enfin, les octets rendus, passés au modèle de L_00BFB810 (seuils stricts lus dans SC2.exe), donnent les
 * paliers de textures OBSERVÉS au banc le 22/10 (docs/CAPS.md, section Adaptateur) : 1024 → 3, 768 → 2, 384 → 1,
 * refus → 0, avec la GTX 285 (Ultra).
 * Recette : cc -Wall -Wextra -I recomp-sc2/runtime recomp-sc2/tests/test_ddraw.c -o test_ddraw && ./test_ddraw
 * (sinon scripts/verifier.sh, fragment tests-hote/v3/v2-5a-ddraw.cmake). */
#include "sc2_ddraw.h"

#include <stdio.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("ÉCHEC  "); printf(__VA_ARGS__); putchar('\n'); } } while (0)

#define HIST 0x0402u   /* GeForce 8600 GT (historique, canon) */
#define DX10 0x05E3u   /* GeForce GTX 285 (dx10) */

/* Modèle de L_00BFB810 (recomp_15_2.c:5103-5194) : paliers stricts 192 / 384 / 768 Mo (flottants de SC2.exe en
 * 0x0148F130/34/38), TotalVirtual ≥ 3 Go (vrai : le runtime annonce 8 Go), borné par le niveau du GPU. */
static int palier_textures(uint32_t vidmem_o, int niveau_gpu)
{
    int t = vidmem_o > 805306368u ? 3 : vidmem_o > 402653184u ? 2 : vidmem_o > 201326592u ? 1 : 0;
    return t < niveau_gpu ? t : niveau_gpu;
}

int main(void)
{
    /* --- décision : (DeviceId, SC2_DDRAW, Android) → servi, invalide --- */
    static const struct { unsigned dev; const char *env; int android, servi, invalide; const char *quoi; } D[] = {
        { HIST, NULL,  0, 0, 0, "canon : historique sans SC2_DDRAW → refusé" },
        { HIST, "",    0, 0, 0, "historique, SC2_DDRAW vide → refusé" },
        { HIST, NULL,  1, 0, 0, "historique sur Android → refusé" },
        { DX10, NULL,  0, 1, 0, "dx10 hors Android → servi" },
        { DX10, NULL,  1, 0, 0, "dx10 sur Android → refusé (garde-fou du Fold)" },
        { DX10, "1",   1, 1, 0, "dx10 sur Android, SC2_DDRAW=1 → servi" },
        { DX10, "0",   0, 0, 0, "dx10, SC2_DDRAW=0 → refusé" },
        { DX10, "0",   1, 0, 0, "dx10 sur Android, SC2_DDRAW=0 → refusé" },
        { HIST, "1",   0, 1, 0, "historique, SC2_DDRAW=1 → servi (diagnostic)" },
        { HIST, "1",   1, 1, 0, "historique sur Android, SC2_DDRAW=1 → servi" },
        { HIST, "0",   0, 0, 0, "historique, SC2_DDRAW=0 → refusé" },
        { 0x1234u, NULL, 0, 1, 0, "profil moderne futur (autre DeviceId) → servi" },
        { DX10, "2",   0, 1, 1, "SC2_DDRAW=2 invalide → règle par défaut (dx10 : servi)" },
        { HIST, "oui", 0, 0, 1, "SC2_DDRAW=oui invalide → règle par défaut (historique : refusé)" },
        { HIST, " 1",  0, 0, 1, "SC2_DDRAW=« 1» invalide → refusé" },
        { DX10, "00",  1, 0, 1, "SC2_DDRAW=00 invalide sur Android → refusé" },
    };
    for (unsigned k = 0; k < sizeof D / sizeof D[0]; k++) {
        int inv = -7, f = sc2_ddraw_forcage(D[k].env, &inv);
        int s = sc2_ddraw_servi_decider(D[k].dev, f, D[k].android);
        CHECK(s == D[k].servi && inv == D[k].invalide, "%s : servi %d (attendu %d), invalide %d (attendu %d)",
              D[k].quoi, s, D[k].servi, inv, D[k].invalide);
    }
    CHECK(sc2_ddraw_forcage("1", NULL) == 1 && sc2_ddraw_forcage("x", NULL) == -1, "forcage sans pointeur invalide");

    /* --- mémoire : (SC2_VRAM_MO, DeviceId) → Mo, invalide --- */
    static const struct { const char *env; unsigned dev; uint32_t mo; int invalide; } M[] = {
        { NULL, DX10, 1024, 0 }, { NULL, HIST, 512, 0 }, { "", DX10, 1024, 0 }, { "", HIST, 512, 0 },
        { "768", DX10, 768, 0 }, { "384", HIST, 384, 0 }, { "1", DX10, 1, 0 }, { "4095", DX10, 4095, 0 },
        { "0768", DX10, 768, 0 },
        { "0", DX10, 1024, 1 }, { "4096", DX10, 1024, 1 }, { "-5", DX10, 1024, 1 }, { "+5", DX10, 1024, 1 },
        { " 12", DX10, 1024, 1 }, { "12x", HIST, 512, 1 }, { "1e3", DX10, 1024, 1 },
        { "99999999999999999999999", DX10, 1024, 1 }, { "Mo", HIST, 512, 1 },
    };
    for (unsigned k = 0; k < sizeof M / sizeof M[0]; k++) {
        int inv = -7;
        uint32_t mo = sc2_ddraw_vram_mo(M[k].env, M[k].dev, &inv);
        CHECK(mo == M[k].mo && inv == M[k].invalide, "SC2_VRAM_MO « %s » (DeviceId 0x%04X) : %u Mo (attendu %u), "
              "invalide %d (attendu %d)", M[k].env ? M[k].env : "(absente)", M[k].dev, mo, M[k].mo, inv, M[k].invalide);
    }

    /* --- octets de GetAvailableVidMem : DDSCAPS2 de la sonde du jeu (L_00A00FC0 : 0x10004000 puis 0x20005000) --- */
    CHECK(sc2_ddraw_vidmem_octets(0x10004000u, 1024) == 0x40000000u, "locale 1024 Mo → 0x40000000");
    CHECK(sc2_ddraw_vidmem_octets(0x20005000u, 1024) == 0u, "non locale (0x20005000) → 0 (pas d'AGP)");
    CHECK(sc2_ddraw_vidmem_octets(0x20000000u, 4095) == 0u, "NONLOCALVIDMEM seul → 0");
    CHECK(sc2_ddraw_vidmem_octets(0x10004000u, 4095) == 0xFFF00000u, "locale 4095 Mo → 0xFFF00000 (sans débordement)");
    CHECK(sc2_ddraw_vidmem_octets(0x10004000u, 768) == 805306368u, "locale 768 Mo → 805306368 octets exactement");

    /* --- paliers observés au banc (22/10, 600 frames sans Variables.txt, GTX 285 = Ultra) --- */
    static const struct { const char *env; int servi; int palier; } P[] = {
        { NULL, 1, 3 }, { "768", 1, 2 }, { "384", 1, 1 }, { "192", 1, 0 }, { "193", 1, 1 }, { "769", 1, 3 },
        { NULL, 0, 0 },   /* SC2_DDRAW=0 en dx10 : [+0x2BC0] = 0 */
    };
    for (unsigned k = 0; k < sizeof P / sizeof P[0]; k++) {
        uint32_t o = P[k].servi ? sc2_ddraw_vidmem_octets(0x10004000u, sc2_ddraw_vram_mo(P[k].env, DX10, NULL)) : 0u;
        int t = palier_textures(o, 3);
        CHECK(t == P[k].palier, "dx10, SC2_VRAM_MO « %s »%s : palier de textures %d (attendu %d)",
              P[k].env ? P[k].env : "(absente)", P[k].servi ? "" : ", refusé", t, P[k].palier);
    }
    CHECK(palier_textures(sc2_ddraw_vidmem_octets(0x10004000u, sc2_ddraw_vram_mo(NULL, HIST, NULL)), 1) == 1,
          "historique + SC2_DDRAW=1 (512 Mo, 8600 GT Medium) → palier 1");

    printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
