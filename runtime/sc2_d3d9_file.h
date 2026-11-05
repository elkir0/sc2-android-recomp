/* sc2_d3d9_file.h — V5.1 (24/10) : file de libération des ressources D3D9, logique PURE (aucune mémoire invitée, aucun
 * Vulkan) : sc2_d3d9.c l'appelle avec ses accès (compte de références, genre, surfaces de niveaux, liaisons, oubli
 * Vulkan, rendu à l'arène) ; recomp-sc2/tests/test_d3d9_file.c la teste sur l'hôte.
 *
 * Un objet tombé à zéro référence entre en file avec le numéro du Present en cours. Au Present n, zf_trier :
 *   - le GARDE si n − frame < ZF_DELAI (au moins ZF_DELAI − 1 frames COMPLÈTES depuis son Release) ou s'il est encore
 *     lié au device (le vrai D3D9 tient une référence sur chaque ressource liée ; le device factice n'en prend pas) ;
 *   - le SORT de la file sans le rendre s'il a été repris (référence > 0), s'il n'est plus d'un genre rendable (conteneur
 *     marqué « jamais » : une surface hors du cache plein) ou si l'une de ses surfaces de niveau est tenue (−1 : la
 *     surface, rendue à zéro, remettra son conteneur en file) ; une surface liée (−2) le garde ;
 *   - le rend PRÊT sinon.
 * zf_rendre : d'abord l'oubli Vulkan de TOUS les prêts (leurs adresses, clés des caches, vont être resservies), puis,
 * pour chacun, les surfaces de niveaux (conteneurs) et l'objet. */
#ifndef SC2_D3D9_FILE_H
#define SC2_D3D9_FILE_H
#include <stdint.h>

#define ZF_DELAI 3u                                  /* Presents : rendu au Present frame + 3, soit ≥ 2 frames complètes */
enum { ZF_AUTRE = 0, ZF_CONTENEUR = 1, ZF_TAMPON = 2, ZF_SURFACE = 3 };
typedef struct { uint32_t o, frame; } zf_entree;
typedef struct { uint32_t tex[20], flux[16], rt[4], indices, ds, backbuf; } zf_liaisons;
typedef struct {
    uint32_t (*ref)(void *ctx, uint32_t o);
    int (*genre)(void *ctx, uint32_t o);                     /* ZF_* */
    int (*surfaces)(void *ctx, uint32_t o, int retirer);     /* conteneur : −1 tenue, −2 liée, sinon nombre (retirées) */
    int (*lie)(void *ctx, uint32_t o);
    void (*hors_file)(void *ctx, uint32_t o);                /* efface la marque « en file » de l'objet */
    void (*oublier)(void *ctx, const uint32_t *objs, int n); /* caches Vulkan */
    void (*rendre)(void *ctx, uint32_t o);                   /* données et objet rendus à l'arène */
} zf_ops;
typedef struct { uint64_t reprises, lies, retenus, rendus, pleine; } zf_compteurs;

static inline int zf_lie(const zf_liaisons *l, uint32_t o)
{
    if (!o) return 0;
    for (int k = 0; k < 20; k++) if (l->tex[k] == o) return 1;
    for (int k = 0; k < 16; k++) if (l->flux[k] == o) return 1;
    for (int k = 0; k < 4; k++) if (l->rt[k] == o) return 1;
    return l->indices == o || l->ds == o || l->backbuf == o;
}
/* ajoute o (la marque « en file » est à l'appelant) ; 0 si la file est pleine (objet gardé, compté) */
static inline int zf_mettre(zf_entree *f, uint32_t *nf, uint32_t max, uint32_t o, uint32_t frame, zf_compteurs *c)
{
    if (*nf >= max) { c->pleine++; return 0; }
    f[*nf].o = o; f[*nf].frame = frame; (*nf)++;
    return 1;
}
/* trie la file (compactée en place) ; rend le nombre de prêts écrits dans prets[] */
static inline uint32_t zf_trier(zf_entree *f, uint32_t *nf, uint32_t n, const zf_ops *ops, void *ctx, uint32_t *prets,
                                zf_compteurs *c)
{
    uint32_t m = 0, np = 0;
    for (uint32_t i = 0; i < *nf; i++) {
        uint32_t o = f[i].o;
        if (n - f[i].frame < ZF_DELAI) { f[m++] = f[i]; continue; }
        if (ops->ref(ctx, o)) { ops->hors_file(ctx, o); c->reprises++; continue; }
        if (ops->lie(ctx, o)) { f[m++] = f[i]; c->lies++; continue; }
        int g = ops->genre(ctx, o);
        if (g == ZF_AUTRE) { ops->hors_file(ctx, o); c->retenus++; continue; }   /* plus rendable (marqué « jamais ») */
        if (g == ZF_CONTENEUR) {
            int r = ops->surfaces(ctx, o, 0);
            if (r == -2) { f[m++] = f[i]; c->lies++; continue; }
            if (r == -1) { ops->hors_file(ctx, o); c->retenus++; continue; }
        }
        prets[np++] = o;
    }
    *nf = m;
    return np;
}
static inline void zf_rendre(const uint32_t *prets, uint32_t np, const zf_ops *ops, void *ctx, zf_compteurs *c)
{
    if (!np) return;
    ops->oublier(ctx, prets, (int)np);               /* avant tout réemploi de ces adresses */
    for (uint32_t i = 0; i < np; i++) {
        if (ops->genre(ctx, prets[i]) == ZF_CONTENEUR) ops->surfaces(ctx, prets[i], 1);
        ops->rendre(ctx, prets[i]);
        c->rendus++;
    }
}
#endif
