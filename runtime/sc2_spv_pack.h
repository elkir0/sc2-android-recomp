/* sc2_spv_pack.h — pack SPIR-V externe sc2-spirv.pak (passe 10t).
 *
 * Traductions SPIR-V des shaders D3D9 du jeu HORS de la table embarquée
 * (SC2_SHADERS_SPV, ~2 900 shaders) : des dizaines de milliers d'entrées,
 * générées hors-ligne (scripts/d3d9-vers-spirv.py). Format imposé, petit-boutiste,
 * sans compression ; ouvert par mmap lecture seule et JAMAIS défait (durée du
 * processus) ; consulté seulement quand la table embarquée ne contient pas le
 * shader — sans pack, le comportement est inchangé (dessins « lumière »).
 *
 * Format (offsets en octets) :
 *   - en-tête 64 o : magic « SC2SPV01 » (8 o), version 1 (u32), n entrées (u32),
 *     off_index = 64 (u64), taille totale du fichier (u64), 32 o réservés à zéro ;
 *   - index à off_index : n entrées de 96 o, triées par (hash, ps) croissants,
 *     sans doublon — hash (u64, 0), ps (u8, 8), nentrees ≤ 16 (u8, 9), nconst ≤ 256 (u16, 10 : registres c
 *     lus par le shader, 0 = bloc entier — passe 10t, octets à zéro dans les packs plus anciens),
 *     nmots (u32, 12), nmots_ombre (u32, 16), u32 à zéro (20), off_spv (u64, 24),
 *     entrees[16] de {reg, usage, index} (32), samplers[16] (80) ;
 *   - données : blobs à offsets multiples de 16, premier mot 0x07230203 ; la
 *     variante « ombres » suit immédiatement le blob normal.
 */
#ifndef RECOMP_SC2_SPV_PACK_H
#define RECOMP_SC2_SPV_PACK_H

#include <stdint.h>

/* Entrée trouvée : champs recopiés de l'index ; spv/spv_ombre pointent DANS le mmap
 * du pack (durée du processus), spv_ombre est NULL sans variante « ombres ». */
typedef struct { uint64_t hash; uint8_t ps, nentrees; uint16_t nconst; uint8_t entrees[16][3]; uint8_t samplers[16];
                 const uint32_t *spv; uint32_t nmots; const uint32_t *spv_ombre; uint32_t nmots_ombre; } sc2_spv_pack_entree;

int sc2_spv_pack_ouvrir(const char *chemin);          /* 0 : ouvert et valide ; -1 : absent ou invalide (raison journalisée) */
int sc2_spv_pack_chercher(uint64_t hash, int ps, sc2_spv_pack_entree *e);   /* 1 trouvé, 0 sinon */
uint32_t sc2_spv_pack_n(void);                        /* entrées du pack ouvert, 0 sinon */
const char *sc2_spv_pack_chemin(void);                /* chemin du pack ouvert, ou NULL */
const char *sc2_spv_pack_raison(void);                /* pourquoi le pack est absent ([BILAN]), NULL s'il est ouvert */

#endif
