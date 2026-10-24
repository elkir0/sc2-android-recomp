/* sc2_d3d9_inst.h — V2.2 (22/10) : instanciation D3D9 (SetStreamSourceFreq) → plan Vulkan. Fonctions PURES : ni
 * mémoire invitée, ni Vulkan. Utilisées par dessin_reel (sc2_d3d9.c) ; tests/test_instanciation.c les juge contre une
 * implémentation de référence de la lecture des sommets D3D9 (200 000 cas tirés, scripts/injecter-bugs-v22.sh pour
 * les mutants). Conception : ~/dev/sc2-bancs/20261022/conception/v2-2-instanciation.md §1.6 et §2.1.
 *
 * Sémantique retenue (DXVK, Wine et la règle Vulkan concordent) :
 *   - flux en fréquence 0, 1 ou INDEXEDDATA | N : élément BaseVertexIndex + index (par sommet, comme avant) ;
 *   - flux en INSTANCEDATA | 1 : élément j pour l'instance j, SANS BaseVertexIndex ni MinIndex
 *     (VK_VERTEX_INPUT_RATE_INSTANCE, diviseur 1 : cœur de Vulkan 1.0, aucune extension) ;
 *   - instanceCount = N (valeur INDEXEDDATA) ;
 *   - INSTANCEDATA | d, d > 1 (0 cas sur 9 runs, V1.3), deux N différents, drapeaux doubles : REPLI = plan neutre,
 *     le dessin d'avant à l'octet. */
#ifndef SC2_D3D9_INST_H
#define SC2_D3D9_INST_H
#include <stdint.h>
#define D3DSSF_INDEXEDDATA  0x40000000u
#define D3DSSF_INSTANCEDATA 0x80000000u
#define D3DSSF_VALEUR       0x3FFFFFFFu
enum { INST_OK = 0, INST_REPLI_DIVISEUR = 1, INST_REPLI_N = 2, INST_REPLI_DRAPEAUX = 3, INST_NREPLIS = 4 };
typedef struct {
    uint32_t n;          /* instanceCount (≥ 1) */
    uint32_t par_inst;   /* bit s : flux D3D9 s lu PAR INSTANCE (INSTANCEDATA | 1) */
    uint32_t indexes;    /* bit s : flux D3D9 s en INDEXEDDATA (bilan) */
    uint32_t somme_idx;  /* Σ des valeurs INDEXEDDATA, comme le « Σ » de V1.3 (comparaison au bilan) */
    uint32_t repli;      /* INST_* ; ≠ 0 → plan NEUTRE (n = 1, par_inst = 0) : le dessin d'avant */
    uint32_t vu;         /* valeur fautive (diviseur > 1, second N, fréquence à drapeaux doubles), pour le journal */
} inst_plan;

/* masque = flux de fréquence ∉ {0, 1} (g_freq_non1, V1.3) ; nul → plan neutre sans lire freq[] */
static inline inst_plan inst_planifier(const uint32_t freq[16], uint32_t masque)
{
    inst_plan p = { 1, 0, 0, 0, INST_OK, 0 };
    uint32_t n = 0;
    for (uint32_t b = masque & 0xFFFFu; b; b &= b - 1) {
        unsigned s = (unsigned)__builtin_ctz(b);
        uint32_t f = freq[s], v = f & D3DSSF_VALEUR;
        if ((f & D3DSSF_INSTANCEDATA) && (f & D3DSSF_INDEXEDDATA)) {          /* invalide en D3D9 */
            if (!p.repli) { p.repli = INST_REPLI_DRAPEAUX; p.vu = f; }
        } else if (f & D3DSSF_INSTANCEDATA) {
            p.par_inst |= 1u << s;                                              /* d = 0 lu comme 1 */
            if (v > 1 && !p.repli) { p.repli = INST_REPLI_DIVISEUR; p.vu = v; }
        } else if (f & D3DSSF_INDEXEDDATA) {
            uint32_t m = v ? v : 1;
            p.indexes |= 1u << s; p.somme_idx += v;
            if (n && m != n && !p.repli) { p.repli = INST_REPLI_N; p.vu = m; }
            if (m > n) n = m;
        }
        /* sans drapeau, valeur ≥ 2 : ancienne fréquence de sommets (« autres » 0 en V1.3) — ignorée, comme avant */
    }
    if (p.repli) { p.n = 1; p.par_inst = 0; return p; }
    p.n = n ? n : 1;                 /* INSTANCEDATA sans INDEXEDDATA : une instance, enregistrement 0 (DXVK idem) */
    return p;
}
/* plage copiée d'un flux. Par instance : éléments [0, n) depuis le décalage du flux, SANS BaseVertexIndex ni MinIndex.
 * Par sommet : [premier, premier + nsommets) — la formule d'avant (dessin_reel), à l'octet. */
static inline void inst_plage(int par_instance, uint32_t premier, uint32_t nsommets, uint32_t n, uint32_t stride,
                              uint64_t *debut, uint64_t *octets)
{
    if (par_instance) { *debut = 0; *octets = (uint64_t)n * stride; }
    else { *debut = (uint64_t)premier * stride; *octets = (uint64_t)nsommets * stride; }
}
/* instances ENTIÈRES lisibles dans `octets` (flux d'instance tronqué par la fin du tampon invité) */
static inline uint32_t inst_borne(uint32_t n, uint64_t octets, uint32_t stride)
{
    if (!stride) return n;                       /* stride 0 : toutes les instances lisent l'élément 0 */
    uint64_t k = octets / stride;
    return k < n ? (uint32_t)k : n;
}
#endif
