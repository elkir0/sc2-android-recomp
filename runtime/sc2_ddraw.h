/* sc2_ddraw.h — V2.5a (22-23/10) : décisions de l'IDirectDraw7 factice de sc2_iat_p4.c, en logique pure.
 *
 * Le jeu mesure la mémoire vidéo par DirectDrawCreateEx + IDirectDraw7::GetAvailableVidMem (sonde L_00A00FC0) et
 * n'en tire que la SUGGESTION de GraphicsOptionTextureQuality (L_00BFB810 : 3 au-dessus de 768 Mo, 2 au-dessus de 384,
 * 1 au-dessus de 192, 0 sinon, seuils stricts, borné par le niveau du GPU). Ce fichier décide si DirectDraw est servi
 * et ce que GetAvailableVidMem rend ; sc2_iat_p4.c ne fait qu'appeler ces fonctions avec getenv() et le DeviceId
 * annoncé (sc2_gpu_device_id, sc2_d3d9.c : la grammaire de SC2_GPU_PROFIL n'existe qu'à un endroit).
 *
 * Règle (garde du canon) :
 *   SC2_DDRAW=0 → refusé ; SC2_DDRAW=1 → servi (diagnostic, tous profils, toutes plateformes) ;
 *   sinon servi SEULEMENT en profil moderne (DeviceId annoncé ≠ 0x0402, la 8600 GT du profil historique) ET hors
 *   Android. Historique (canon) : refus d'avant, à l'octet. Android : refus par défaut, comme avant V2.5a, tant que
 *   le PSS et le LMK du Fold ne sont pas mesurés en textures High/Ultra (revue du 23/10 : le lanceur pose dx10 par
 *   défaut, et une suggestion — « Restore Defaults », Variables.txt absent ou d'un autre DeviceId — donnerait
 *   l'Ultra au lieu du Low).
 * Mémoire : SC2_VRAM_MO (1 à 4095 Mo) ; défaut 1024 en profil moderne, 512 en historique (= GetAvailableTextureMem
 * des deux profils de sc2_d3d9.c ; l'historique n'est servi que sous SC2_DDRAW=1).
 *
 * Pur calcul, sans CPU invité : test hôte recomp-sc2/tests/test_ddraw.c (scripts/verifier.sh, ctest). */
#ifndef SC2_DDRAW_H
#define SC2_DDRAW_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SC2_DDRAW_DEVICE_HISTORIQUE 0x0402u    /* GeForce 8600 GT, profil historique (canon) : DirectDraw refusé */
#define SC2_DDRAW_DDSCAPS_NONLOCAL  0x20000000u /* DDSCAPS2.dwCaps : DDSCAPS_NONLOCALVIDMEM (AGP) */

/* SC2_DDRAW : « 0 » → 0 (refus forcé), « 1 » → 1 (service forcé), absente ou vide → -1 (règle par défaut) ;
 * toute autre valeur → -1 et *invalide = 1 (l'appelant le journalise). */
static inline int sc2_ddraw_forcage(const char *e, int *invalide)
{
    if (invalide) *invalide = 0;
    if (!e || !*e) return -1;
    if (!strcmp(e, "0")) return 0;
    if (!strcmp(e, "1")) return 1;
    if (invalide) *invalide = 1;
    return -1;
}

/* Profil moderne : tout DeviceId annoncé autre que celui du profil historique. */
static inline int sc2_ddraw_moderne(unsigned device_id) { return device_id != SC2_DDRAW_DEVICE_HISTORIQUE; }

/* DirectDraw servi ? forcage (sc2_ddraw_forcage) l'emporte ; sinon profil moderne et hors Android. */
static inline int sc2_ddraw_servi_decider(unsigned device_id, int forcage, int android)
{
    if (forcage == 0 || forcage == 1) return forcage;
    return sc2_ddraw_moderne(device_id) && !android;
}

/* Mémoire annoncée (Mo) : SC2_VRAM_MO décimale de 1 à 4095, sinon le défaut du profil (1024 moderne, 512
 * historique) ; *invalide = 1 si la variable est posée mais refusée (l'appelant le journalise). */
static inline uint32_t sc2_ddraw_vram_mo(const char *e, unsigned device_id, int *invalide)
{
    uint32_t defaut = sc2_ddraw_moderne(device_id) ? 1024u : 512u;
    if (invalide) *invalide = 0;
    if (!e || !*e) return defaut;
    char *f = NULL;
    unsigned long v = (*e >= '0' && *e <= '9') ? strtoul(e, &f, 10) : 0;
    if (!f || *f || v < 1 || v > 4095) {
        if (invalide) *invalide = 1;
        return defaut;
    }
    return (uint32_t)v;
}

/* Octets rendus par GetAvailableVidMem (total = libre) pour ces DDSCAPS2.dwCaps : 0 en mémoire non locale (pas
 * d'AGP, le jeu range ce total en [+0x2BC4]), sinon mo Mio (en [+0x2BC0], celle que lit L_00BFB810). mo ≤ 4095 :
 * pas de débordement sur 32 bits. */
static inline uint32_t sc2_ddraw_vidmem_octets(uint32_t caps, uint32_t mo)
{
    return (caps & SC2_DDRAW_DDSCAPS_NONLOCAL) ? 0u : mo << 20;
}

#endif
