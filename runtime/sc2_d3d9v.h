/* sc2_d3d9v.h — backend de rendu RÉEL du D3D9 de recomp-sc2 : Vulkan headless (passe 5c).
 *
 * sc2_d3d9.c garde le dispatch COM, les objets invités et le suivi des états ;
 * il appelle ce backend pour ce qui produit des pixels : cibles de rendu (back
 * buffer, surfaces de textures RENDERTARGET, surfaces de profondeur), Clear,
 * BeginScene/EndScene, Draw* et Present. API en C pur, sans CPU invité : elle se
 * teste seule (tests/test_d3d9v.c).
 *
 * Vulkan est chargé par dlopen("libvulkan.so.1") : aucun lien, le binaire tourne
 * sans Vulkan (backend inactif → device factice de la passe 4, inchangé).
 * Cible du POC : Lavapipe (Mesa, rendu CPU), sans surface ni fenêtre — le
 * « swapchain » est une image VkImage relue en mémoire hôte à Present.
 *
 *   SC2_VULKAN=0          backend désactivé (factice seul)
 *   SC2_FRAMES_DIR=<dir>  dossier des PPM (défaut « frames »)
 *   SC2_VK_SYNCHRONE=1    une seule frame en vol : soumission + vkQueueWaitIdle (avant la
 *                         passe 10r v13) ; par défaut deux emplacements, fences par emplacement
 */
#ifndef RECOMP_SC2_D3D9V_H
#define RECOMP_SC2_D3D9V_H
#include <stdint.h>

#define D3DV_CLEAR_TARGET  1u
#define D3DV_CLEAR_ZBUFFER 2u
#define D3DV_CLEAR_STENCIL 4u

typedef void (*d3dv_journal_fn)(const char *fmt, ...);
extern d3dv_journal_fn d3dv_journal;       /* défaut : printf + '\n' */

int  d3dv_init(void);                      /* 1 si le backend est actif (idempotent) */
int  d3dv_actif(void);
const char *d3dv_nom_device(void);

/* Cible de rendu identifiée par une clé stable (VA de surface, ou conteneur +
 * taille). Créée au premier usage, recréée si sa taille change. fmt = D3DFORMAT.
 * Rend un identifiant ≥ 0, ou -1. */
int  d3dv_cible(uint64_t cle, uint32_t w, uint32_t h, uint32_t fmt, int profondeur);
int  d3dv_ombres(void);   /* passe 7s/7v3 ; 21/10 : ACTIVES par défaut (SC2_OMBRES=0 coupe ; refusées sans 34 échantillonneurs) */
void d3dv_rt(int id);                      /* RT0 courante (-1 : aucune) */
void d3dv_ds(int id);                      /* surface de profondeur courante */
void d3dv_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float zmin, float zmax);
/* rects : n × (x1, y1, x2, y2), ou NULL : tout le viewport. */
void d3dv_clear(uint32_t n, const int32_t *rects, uint32_t flags, uint32_t couleur, float z, uint32_t stencil);
void d3dv_scene(int debut);                /* BeginScene (1) / EndScene (0) */
void d3dv_draw(uint32_t prim, uint32_t nprim);   /* « mode lumière » : pipeline réel, triangle dégénéré */

/* Present : soumet le travail de la frame. Si chemin != NULL, relit la cible id
 * et l'écrit en PPM (P6) ; *moy reçoit la moyenne R, G, B (0..255). 0 si OK. */
int  d3dv_present(int id, const char *chemin, double moy[3]);
/* Relecture directe (tests) : rgba = w*h*4 octets R, G, B, A. 0 si OK. */
int  d3dv_lire(int id, uint8_t *rgba);
void d3dv_vider_cibles(const char *dir, uint32_t frame);   /* diagnostic 7m : toutes les cibles couleur en PPM */
void d3dv_bilan(void);

/* ---- Passe 5d : draws RÉELS (shaders du jeu traduits hors-ligne en SPIR-V) ---- */
int  d3dv_module(const uint32_t *spv, uint32_t nmots, int ps);        /* id ≥ 0, ou -1 */
/* Texture du jeu (2 = 2D, 3 = cube, 4 = volume), niveau 0 seulement (cf. d3dv_texture_niveaux) ; re-téléversée
 * quand gen change. fmt = D3DFORMAT. Rend un id ≥ 0, ou -1 (format non géré). */
int  d3dv_texture(uint64_t cle, int genre, uint32_t w, uint32_t h, uint32_t d, uint32_t fmt,
                  const void *donnees, uint32_t taille, uint32_t gen);
/* Passe 7l : TOUS les niveaux de mip — donnees = niveaux serrés dans l'ordre d'un DDS (niveau 0 d'abord),
 * taille = octets disponibles (les niveaux qui n'y tiennent pas sont omis). DXTn natifs (BC1..3) si le
 * device a la compression BC, sinon décodés par le CPU en RGBA8 (SC2_VK_SANS_BC=1 force ce repli). */
int  d3dv_texture_niveaux(uint64_t cle, int genre, uint32_t w, uint32_t h, uint32_t d, uint32_t fmt, uint32_t niveaux,
                          const void *donnees, uint32_t taille, uint32_t gen);
/* Décodeur logiciel seul (tests) : un niveau w×h du format D3D fmt → rgba (w*h*4). 0 si fait. */
int  d3dv_decoder_rgba(uint32_t fmt, const void *src, uint32_t w, uint32_t h, uint8_t *rgba);
/* Relecture d'un niveau d'une texture 2D ou d'une face (couche 0..5) de cube (tests) : RGBA8 décodé par le
 * GPU (blit), canaux bruts. 0 si fait. */
int  d3dv_texture_relire(int id, uint32_t couche, uint32_t niveau, uint8_t *rgba);
int  d3dv_bc_natif(void);                        /* 1 : DXTn en BC natif ; 0 : décodés par le CPU */

#define D3DV_TEX_AUCUNE 0
#define D3DV_TEX_TEXTURE 1
#define D3DV_TEX_CIBLE 2
typedef struct {
    int vs, ps;                                   /* modules */
    uint32_t topo;                                /* D3DPRIMITIVETYPE */
    uint32_t nattr;
    struct { uint32_t loc, flux, vkfmt, offset; } attr[16];
    uint32_t nflux;
    /* passe 10r : derrière un flux il y a un tampon invité (VB) — obj = son adresse, gen = sa
     * génération de contenu, usage = D3DUSAGE, taille_tot = O_W, donnees_tot = tout le tampon,
     * decalage = octets entre son début et les données de CE dessin. obj = 0 (draw « user
     * pointer », attribut constant) : donnees va dans l'anneau, comme avant. */
    struct { uint32_t stride; const void *donnees; uint32_t taille;
             uint32_t obj, gen, usage, taille_tot, decalage; const void *donnees_tot; } flux[16];
    const void *index; uint32_t index_taille; int index32;
    /* passe 10r : tampon d'indices invité (IB), mêmes champs que flux[] ; index_obj = 0 : UP */
    uint32_t index_obj, index_gen, index_usage, index_taille_tot, index_decalage;
    const void *index_donnees_tot;
    uint32_t nombre;                              /* sommets (sans index) ou index */
    int32_t decalage_sommet;                      /* vertexOffset de vkCmdDrawIndexed */
    /* V2.2 (22/10) : instanciation D3D9 (SetStreamSourceFreq). Zéro par défaut (memset de dessin_reel, dessins de test
     * qui ne les nomment pas) : clé de pipeline, liaisons et appel d'avant, à l'octet. */
    uint32_t instances;                           /* instanceCount ; 0 ou 1 = une instance (comme avant) */
    uint32_t flux_inst;                           /* bit i = flux[i] lu PAR INSTANCE (VK_VERTEX_INPUT_RATE_INSTANCE, diviseur 1) */
    const void *cvs, *cps;                        /* blocs de constantes (D3DV_CONST_TAILLE octets) */
    uint32_t gen_vs, gen_ps;                      /* passe 10r : génération du contenu AU MOMENT de l'assemblage
                                                   * de cvs/cps — même génération ⇒ mêmes octets, le backend réutilise
                                                   * l'emplacement de l'anneau sans recopier (épargne memmove) */
    uint16_t nconst_vs, nconst_ps;                /* passe 10t : registres c lus par le VS / le PS (scripts/d3d9-vers-spirv.py) :
                                                   * seuls les 16 × nconst premiers octets du bloc sont recopiés dans l'anneau ;
                                                   * 0 = bloc entier (registres i/b, adressage relatif, table sans le champ) */
    struct { int genre_shader; int source; int id; uint32_t adresse[3], mag, min, mip;
             uint32_t biais, maxmip, aniso, bordure; } tex[16];   /* D3DSAMP_* (passe 7l : biais = bits d'un float) */
    uint32_t blend, src, dst, op, src_a, dst_a, op_a, masque_couleur;
    float hp[4], at[4];
    uint32_t zt, zw, zf, cull;                    /* passe 7n : D3DRS ZENABLE, ZWRITEENABLE, ZFUNC, CULLMODE */
    uint32_t zbiais, zpente;                      /* passe 7v3 : D3DRS DEPTHBIAS (195), SLOPESCALEDEPTHBIAS (175), bits de float */
    /* 21/10 : stencil D3D9 — les 13 D3DRS (valeurs D3D9 brutes : D3DSTENCILOP/D3DCMP, REF hors clé
     * de pipeline car état dynamique). Surlignage de sélection du jeu : REPLACE 128 puis EQUAL 0. */
    uint32_t st_en, st_fail, st_zfail, st_pass, st_func, st_ref, st_masque, st_ecriture,
             st_2faces, st_ccw_fail, st_ccw_zfail, st_ccw_pass, st_ccw_func;
} d3dv_dessin;
#define D3DV_CONST_TAILLE (256 * 16 + 16 * 16 + 16 * 16)
int  d3dv_dessiner(const d3dv_dessin *d);
/* V2.2 : compteurs du backend (tests, bilan) — dessins à flux d'instance ou instanceCount > 1, Σ instances, maximum,
 * pipelines créés au total et pipelines à flux d'instance. Pointeurs NULL ignorés. */
void d3dv_compteurs_inst(uint64_t *dessins, uint64_t *instances, uint32_t *max, int *pipes, int *pipes_inst);
/* Passe 7w : StretchRect entre cibles (blit Vulkan ; rects RECT D3D9 ou NULL) ; 0 si copié. */
int  d3dv_copier(int src, const int32_t *rs, int dst, const int32_t *rd, int lineaire);
int  d3dv_rt_courante(void);
/* Passe 7v2 : drapeau « sampler de profondeur » (SC2_OMBRES=1) : bit k = slot k lié à une profondeur, par module PS. */
uint32_t d3dv_profondeur_module(int ps);        /* 0 : dessiné ; < 0 : refusé (compté) */

/* ---- Passe 10r (v13) : deux frames en vol — synchronisation avec la présentation ----
 * La présentation (Android) blitte le back buffer vers la swapchain sur une AUTRE file
 * Vulkan ; l'ordre passe par des sémaphores binaires (poignées Vulkan en uint64_t, cet
 * en-tête n'inclut pas vulkan.h). Un sémaphore binaire signalé doit être attendu exactement
 * une fois : la présentation consomme « rendu » (même quand elle n'affiche rien), le backend
 * attend le sien à SA prochaine soumission, une seule fois. En mode synchrone
 * (SC2_VK_SYNCHRONE=1), ces fonctions ne font rien (d3dv_semaphore_rendu rend 0). */
void d3dv_semaphore_rendu_demander(void);   /* la prochaine soumission de d3dv_present signera le sémaphore « rendu » */
uint64_t d3dv_semaphore_rendu(void);        /* sémaphore signalé par la dernière présentation demandée, puis oublié ; 0 si aucun */
void d3dv_attendre_semaphore(uint64_t s, uint32_t etape);   /* la prochaine soumission du backend, quelle qu'elle soit, attendra s à l'étape etape (VkPipelineStageFlags) */
int d3dv_en_vol(void);                      /* 1 : deux frames en vol ; 0 : synchrone (ou backend inactif) */
/* V5.1 (24/10) : objets invités (textures, tampons de sommets/index, surfaces) rendus à l'arène — leurs entrées de cache
 * (clé = adresse invitée ; cibles : adresse, ou adresse << 32 | taille) sont oubliées, images et tampons détruits après
 * les frames en vol (une cible en usage est gardée) ; compteurs pour [MEM]. */
void d3dv_oublier(const uint32_t *objs, int n);
void d3dv_oublis(uint64_t *tex, uint64_t *tam, int *tex_vivantes, int *tam_vivants, uint64_t *cibles, int *cibles_vivantes);

#endif
