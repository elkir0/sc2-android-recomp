/* sc2_d3d9v.c — backend Vulkan headless du D3D9 de recomp-sc2 (passe 5c).
 *
 * Ce que le jeu fait réellement par frame (TRACE-PASSE4, 600 frames : Clear×4803,
 * SetRenderTarget×6614, SetDepthStencilSurface×3594, SetViewport×17987,
 * DrawIndexedPrimitive×5975, BeginScene/EndScene/Present×600) devient ici du
 * travail Vulkan réel :
 *
 *   cible de rendu D3D  → VkImage (couleur ou profondeur), layout GENERAL à vie
 *   Clear(TARGET)       → vkCmdClearAttachments dans un rendu dynamique sur RT0,
 *                         rectangles ∩ viewport (sémantique D3D9)
 *   Clear(Z|STENCIL)    → vkCmdClearDepthStencilImage (image entière)
 *   Draw*               → « mode lumière » : pipeline graphique réel (shaders
 *                         runtime/shaders/lumiere.*), triangle dégénéré hors du
 *                         clip : exécuté, aucun pixel. La traduction des shaders
 *                         SM2/3 du jeu vers SPIR-V est la passe suivante.
 *   Present             → soumission de la frame ; relecture de la cible en
 *                         mémoire hôte et écriture PPM si demandé.
 *
 * Synchronisation : deux frames en vol depuis la passe 10r (v13) — un emplacement par frame
 * (tampon de commandes, fence, moitié d'anneau, pool de descripteurs), barrière mémoire au
 * début de chaque tampon ; relectures CPU et destructions passent par attendre_tout().
 * SC2_VK_SYNCHRONE=1 rend le POC d'origine : une soumission + vkQueueWaitIdle par frame.
 * Référence de lecture : ~/tools/nfsu2-recompiled (d3d9_bridge + vulkan_driver,
 * fork DXVK) — l'organisation, pas le code : notre runtime est en C et le
 * périmètre de cette passe est Clear/Present.
 */
#define _GNU_SOURCE
#include "sc2_d3d9v.h"

/* ---- passe 10t : cache par ÉPOQUE des plages volatiles recopiées dans l'anneau -----------------
 * Un flux ou un index que le cache des tampons statiques ne sert pas (user pointer, dynamique,
 * tampon « volatile », plage hors bornes, plafond atteint) partait dans l'anneau À CHAQUE dessin
 * — or le jeu redessine souvent la MÊME plage d'un même tampon dans la même frame (carte
 * d'ombres puis scène : au banc, 552 tampons volatils pour 544 903 recopies sur 2 500 frames,
 * jusqu'à 7,5 ms/frame de memmove sur le téléphone). Cette table retient, pour une clé (obj,
 * gen, decalage, taille, genre), le décalage ANNEAU où la plage a été recopiée et l'époque de
 * cette écriture — les dessins suivants réutilisent l'emplacement, comme le fait R.cblk pour
 * les blocs de constantes :
 *   - (obj, gen) identifie le CONTENU : le device factice avance O_GEN à chaque Lock et le
 *     contenu ne change qu'entre Lock et Unlock — même (obj, gen) ⇒ mêmes octets, définitifs ;
 *   - decalage + taille délimitent la plage lue (donnees == donnees_tot + decalage, assemblage
 *     de sc2_d3d9.c) ; genre distingue flux (alignement 16) et index (alignement 4) ;
 *   - une entrée ne vaut QUE si son époque est l'époque courante de l'anneau (avancée à chaque
 *     bascule ou remise à zéro dans soumettre()) : au-delà, l'emplacement y est réutilisé. Les
 *     entrées périmées ne sont jamais effacées — leurs cases sont recyclées par la première
 *     insertion qui y passe : aucune remise à zéro, aucune allocation par dessin.
 * Adressage ouvert, sondage linéaire, case libre = obj == 0 (jamais une clé : une plage sans
 * tampon invité n'est pas mise en table). Bloc AUTONOME (stdint.h par l'en-tête seul) : testé
 * par inclusion dans tests/test_d3d9v.c (SC2_D3D9V_TABLE_VOLATILS). */
#define VOLATILS_BITS   13                    /* 8 192 cases : au banc, ~3 700 plages distinctes par frame */
#define VOLATILS_TAILLE (1u << VOLATILS_BITS)
#define VOLATILS_MASQUE (VOLATILS_TAILLE - 1u)
#define VOLATILS_FLUX   0u                    /* genre de la plage : flux (aligné 16) ou index (aligné 4) */
#define VOLATILS_INDEX  1u
typedef struct { uint32_t obj, gen, decalage, taille, genre; uint64_t epoque, off; } vol10;
static vol10 g_volatils[VOLATILS_TAILLE];
/* deux mots de 64 bits de clé + genre, double mélange Fibonacci, bits hauts du produit final
 * (analogue mono-mot : hache_cle64, plus bas dans le backend) */
static uint32_t volatils_hache(uint32_t obj, uint32_t gen, uint32_t decalage, uint32_t taille, uint32_t genre)
{
    uint64_t h = (uint64_t)obj << 32 | gen;
    h = h * 0x9E3779B97F4A7C15ull ^ ((((uint64_t)decalage << 32 | taille) + genre) * 0x9E3779B97F4A7C15ull);
    h *= 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)(h >> (64 - VOLATILS_BITS));
}
/* Cherche la plage de l'époque courante : 1 et *off si présente, 0 sinon. Le sondage s'arrête
 * sur une case VIDE ou PÉRIMÉE : une entrée de l'époque courante ne peut pas se trouver plus
 * loin — volatils_noter n'écrit jamais au-delà de la première case libre ou périmée de sa
 * chaîne, donc tout ce qui suit a été écrit avant l'époque courante. */
static int volatils_trouver(uint32_t obj, uint32_t gen, uint32_t decalage, uint32_t taille, uint32_t genre,
                            uint64_t epoque, uint64_t *off)
{
    uint32_t h = volatils_hache(obj, gen, decalage, taille, genre);
    for (uint32_t n = 0; n < VOLATILS_TAILLE; n++) {
        vol10 *e = &g_volatils[h];
        if (!e->obj || e->epoque != epoque) return 0;          /* fin de chaîne : libre ou périmée */
        if (e->obj == obj && e->gen == gen && e->decalage == decalage && e->taille == taille && e->genre == genre) {
            *off = e->off;
            return 1;
        }
        h = (h + 1) & VOLATILS_MASQUE;
    }
    return 0;                    /* table pleine d'entrées courantes (ne doit pas arriver à 8 192 cases) */
}
/* Entre la plage recopiée — à appeler après un volatils_trouver manqué DE LA MÊME ÉPOQUE :
 * écrit à la première case libre, périmée (recyclage) ou de la même clé (rafraîchissement en
 * place). Table pleine d'entrées courantes : n'insère pas, le prochain dessin recopiera
 * (dégradation sans échec). */
static void volatils_noter(uint32_t obj, uint32_t gen, uint32_t decalage, uint32_t taille, uint32_t genre,
                           uint64_t epoque, uint64_t off)
{
    uint32_t h = volatils_hache(obj, gen, decalage, taille, genre);
    for (uint32_t n = 0; n < VOLATILS_TAILLE; n++) {
        vol10 *e = &g_volatils[h];
        if (!e->obj || e->epoque != epoque
            || (e->obj == obj && e->gen == gen && e->decalage == decalage && e->taille == taille && e->genre == genre)) {
            e->obj = obj; e->gen = gen; e->decalage = decalage; e->taille = taille; e->genre = genre;
            e->epoque = epoque; e->off = off;
            return;
        }
        h = (h + 1) & VOLATILS_MASQUE;
    }
}
/* Passe 10t : plage de constantes lue par le shader (bloc autonome, testé par tests/test_d3d9v.c). Le bloc fait toujours
 * D3DV_CONST_TAILLE octets dans l'anneau (range du descripteur inchangé), mais seuls les 16 × nconst premiers sont
 * recopiés : le shader ne lit rien au-delà (nconst = plus grand registre c lu + 1, calculé par scripts/d3d9-vers-spirv.py
 * sur le GLSL). 0 — registres i/b, adressage relatif, ancienne table sans le champ — ou `completes`
 * (SC2_VK_CONSTS_COMPLETES=1) : bloc entier, comme avant. */
static uint64_t consts_a_copier(uint16_t nconst, int completes)
{
    return completes || nconst == 0 || nconst > 256 ? (uint64_t)D3DV_CONST_TAILLE : 16u * (uint64_t)nconst;
}
/* Un bloc déjà dans l'anneau (époque, génération, octets recopiés) sert-il un dessin (époque et génération courantes,
 * besoin en octets) ? Il faut la même génération, la même époque de l'anneau, et assez de registres recopiés. */
static int bloc_consts_reutilisable(uint64_t epoque_bloc, uint32_t gen_bloc, uint64_t etendue_bloc,
                                    uint64_t epoque, uint32_t gen, uint64_t besoin)
{
    return epoque_bloc == epoque && gen_bloc == gen && etendue_bloc >= besoin;
}
#ifndef SC2_D3D9V_TABLE_VOLATILS   /* inclusion « table seule » (tests/test_d3d9v.c) : s'arrête ici */
#define VK_NO_PROTOTYPES
#ifdef __APPLE__
#define VK_ENABLE_BETA_EXTENSIONS            /* passe 7u : VK_KHR_portability_subset (vulkan_beta.h) */
#endif
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>                          /* passe 10s (cache de pipelines) : cause d'un échec d'écriture */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>                            /* passe 10r (v13) : cumul du temps d'attente des fences */
#include <unistd.h>                          /* V5.1 : usleep (d3dv_oublis, verrou essayé) */
#include "sc2_attentes.h"                     /* SC2_ATTENTES : fences bloquées, ligne par image du fil du jeu */

#ifdef __APPLE__
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#endif

#include "shaders/lumiere_vert.h"
#include "shaders/lumiere_frag.h"

static void journal_defaut(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); fflush(stdout);
}
d3dv_journal_fn d3dv_journal = journal_defaut;
#define LOG(...) d3dv_journal(__VA_ARGS__)

/* ---- chargement dynamique ------------------------------------------------- */
static PFN_vkGetInstanceProcAddr gipa;
#define VK_GLOBAL(X)   X(vkCreateInstance) X(vkEnumerateInstanceVersion)
#define VK_INSTANCE(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceFeatures)
#define VK_DEVICE(X) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) \
    X(vkQueueWaitIdle) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCmdPipelineBarrier) X(vkCmdClearDepthStencilImage) \
    X(vkCmdClearAttachments) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) X(vkCmdCopyImageToBuffer) \
    X(vkCmdSetStencilReference)   /* 21/10 : stencil D3D9 (référence = état dynamique) */ \
    X(vkCreateShaderModule) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
    X(vkCreateDescriptorSetLayout) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCmdPushConstants) X(vkCreateSampler) X(vkCmdCopyBufferToImage) X(vkCmdClearColorImage) \
    X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBindDescriptorSets) X(vkCmdBlitImage) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkDestroyFence) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)   /* passe 10s (chrono GPU) */ \
    X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache)   /* passe 10s (cache de pipelines) */
#define DECL(f) static PFN_##f f;
VK_GLOBAL(DECL) VK_INSTANCE(DECL) VK_DEVICE(DECL)

/* ---- état ------------------------------------------------------------------- */
typedef struct {
    uint64_t cle;
    uint32_t w, h, fmt;
    int prof;
    VkFormat vf;
    VkImageAspectFlags aspect;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView vue;
    VkImageView vue_prof;                  /* passe 7n : vue aspect PROFONDEUR seul (attachement) */
    uint64_t n_clear, n_draw, n_present;   /* bilan par cible */
    uint64_t n_echant, n_copie_src, n_copie_dst;   /* passe 7w : lue comme texture, source/destination de StretchRect */
} cible;

#define MAX_CIBLES 512
/* passe 10r (v13) : libérations différées PAR EMPLACEMENT (vkDestroyBuffer après la fence de
 * l'emplacement qui les a vues naître) — cf. soumettre() / attendre_tout() plus bas */
#define A_LIBERER 512
static struct {
    int essaye, actif;
    void *lib;
    VkInstance inst;
    VkPhysicalDevice pd;
    VkPhysicalDeviceMemoryProperties memp;
    char nom[320];
    VkDevice dev;
    uint32_t qf;
    VkQueue q;
    VkCommandPool pool;
    /* passe 10r (v13) : DEUX frames en vol — le CPU enregistre la frame N+1 pendant que la
     * file exécute la N. Par emplacement : tampon de commandes, fence (créée SIGNALÉE : un
     * emplacement jamais soumis n'attend pas), sémaphore « rendu » (présentation, point 6 de
     * la passe), base de sa moitié d'anneau, libérations différées et pool de descripteurs
     * classiques. SC2_VK_SYNCHRONE=1 : un seul emplacement, vkQueueWaitIdle (comme avant). */
#define EN_VOL 2
    struct {
        VkCommandBuffer cb;
        VkFence fence;
        VkSemaphore sem_rendu;
        VkDeviceSize base;                   /* début de sa moitié de l'anneau */
        uint32_t n_lib;                      /* libérations différées de cet emplacement */
        struct { VkBuffer b; VkDeviceMemory m; int dalle; uint32_t off, taille; VkImage img; VkImageView vue, vue2; } lib[A_LIBERER];
        /* 21/10 : b nul + dalle ≥ 0 ⇒ PLAGE de dalle à rendre (le VkBuffer vit avec elle) ;
         * V5.1 : img non nulle ⇒ IMAGE de texture ou de cible oubliée (vue, vue2, image, mémoire m) */
        VkDescriptorPool pool;               /* voie des descripteurs classiques (sans push) */
        uint32_t pool_en_cours;              /* sets alloués depuis la dernière remise à zéro */
        int en_cours;                        /* soumission en vol : sa fence se signalera */
    } empl[EN_VOL];
    int cur;                                 /* emplacement en cours d'enregistrement */
    int n_empl;                              /* EN_VOL, ou 1 (SC2_VK_SYNCHRONE=1) */
    int cb_ouvert;
    int rendu;                     /* cible du rendu dynamique ouvert, -1 */
    int rendu_ds;                  /* passe 7n : surface de profondeur attachée à ce rendu, -1 */
    int rendu_st;                  /* 21/10 : ce rendu attache aussi le STENCIL (vue deux aspects) */
    cible c[MAX_CIBLES];
    int nc, rt, ds;
    uint32_t vp[4]; float zmin, zmax;
    VkPipelineLayout layout;
    VkShaderModule vs, fs;
    struct { VkFormat f; VkPipeline p; } pipe[16];
    int npipe;
    VkBuffer rb; VkDeviceMemory rbmem; VkDeviceSize rbtaille; void *rbmap;
    /* bilan */
    uint64_t n_clear_c, n_clear_p, n_rects, n_draw, n_draw_ignores, n_present, n_soumis, n_lectures;
    /* passe 10r (v13) : deux frames en vol — soumissions, attentes de fence, motifs d'attendre_tout */
    uint64_t n_soumis_vides, n_attentes_fence, n_at_relecture, n_at_destruction, n_at_liste;
    uint64_t n_soum_anneau_dessin, n_soum_anneau_tex;   /* passe 10s : soumissions forcées par une moitié d'anneau pleine */
    double ms_attentes;
    struct { uint32_t col; uint64_t n; } couleurs[16];
    int ncouleurs;
    /* passe 5d */
    PFN_vkCmdPushDescriptorSetKHR push_desc;
    int bc;
    int sans_push;                           /* passe 7c : SC2_VK_SANS_PUSH_DESCRIPTOR=1 (extension non demandée) */
    int ombres_refus;                        /* 21/10 : GPU sans assez d'échantillonneurs par étage → pas d'ombres */
    int bc_device, sans_bc;                  /* passe 7l : BC offert par le device ; SC2_VK_SANS_BC=1 (repli logiciel forcé) */
    float aniso_max, biais_max;              /* passe 7l : limites des samplers (0 : pas d'anisotropie) */
#ifdef __APPLE__
    int loader;                              /* passe 7u : bibliothèque chargée = loader Khronos (libvulkan) */
    int subset, fans;                        /* passe 7u : VK_KHR_portability_subset activé ; triangleFans */
#endif
} V = { .rendu = -1, .rendu_ds = -1, .rt = -1, .ds = -1 };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- passe 5d : état des draws réels ------------------------------------------ */
#define MAX_MODULES  16384  /* passe 7q : un module par shader traduit utilisé (mission > 1 024) ; passe 10t : ≈ 500 par mission,
                            * ils s'accumulent quand les missions s'enchaînent dans un même lancement */
#define MAX_TEX      4096
#define MAX_PIPES    16384  /* passe 7q ; passe 10t : 300 à 470 par mission (2 048 ≈ 5 missions d'affilée) */
#define MAX_SAMPLERS 64
/* sommets, index, constantes, téléversements : 1 frame par emplacement. Passe 10s : SC2_VK_ANNEAU_MO (Mio par
 * emplacement, 16 à 1024, défaut 256), lu une fois — une moitié qui déborde en cours de frame force une soumission
 * qui attend la frame précédente et coupe le recouvrement CPU/GPU des deux frames en vol. Fold, jeu actif : à 64 Mio,
 * 1,93 soumission par Present et 9,2 ms d'attente de fence par frame ; à 256 Mio, 1,07 et 0,4 ms (18,4 → 22,5 FPS) */
static VkDeviceSize anneau_taille(void)
{
    static VkDeviceSize v;
    if (!v) {
        const char *e = getenv("SC2_VK_ANNEAU_MO"); long mo = e ? strtol(e, NULL, 10) : 256;
        v = (VkDeviceSize)(mo < 16 ? 16 : mo > 1024 ? 1024 : mo) << 20;
    }
    return v;
}
#define ANNEAU_TAILLE anneau_taille()
/* passe 10r (v13) : tampon de commandes de l'emplacement courant ; fin de SA moitié d'anneau */
#define CB         (V.empl[V.cur].cb)
#define ANNEAU_FIN (V.empl[V.cur].base + ANNEAU_TAILLE)
typedef struct {
    int vs, ps; uint32_t topo; VkFormat cf;
    int diag;                                /* passe 7n : PS remplacé par le magenta « lumière » (SC2_VK_PS_DIAG) */
    VkFormat df;                             /* passe 7n : format de l'attachement de profondeur (UNDEFINED : aucun) */
    uint32_t zt, zw, zf, cull;               /* passe 7n : ZENABLE, ZWRITEENABLE, ZFUNC, CULLMODE (D3D9) */
    uint32_t nattr, nflux;
    struct { uint32_t loc, flux, vkfmt, offset; } attr[16];
    uint32_t stride[16];
    uint32_t blend, src, dst, op, src_a, dst_a, op_a, masque;
    uint32_t prof;                           /* passe 7v2 : bit k = le sampler k lit une PROFONDEUR (SC2_OMBRES=1 seulement) */
    uint32_t zbiais, zpente;                 /* passe 7v3 : D3DRS DEPTHBIAS / SLOPESCALEDEPTHBIAS (SC2_OMBRES=1 seulement) */
    /* 21/10 : stencil D3D9 (op = D3DSTENCILOP, func = D3DCMP). RÉFÉRENCE exclue : état dynamique
     * (vkCmdSetStencilReference). Inactif ⇒ tout à ZÉRO : memcmp ne voit que les états utiles,
     * les dessins sans stencil partagent les pipelines d'avant. */
    uint32_t st_func, st_fail, st_zfail, st_pass, st_masque, st_ecriture;
    uint32_t st_ccw_func, st_ccw_fail, st_ccw_zfail, st_ccw_pass;
    uint32_t sa;                             /* 21/10 : le rendu attache le stencil (format stencil déclaré) */
    uint32_t inst;                           /* V2.2 : bit i = liaison i en VK_VERTEX_INPUT_RATE_INSTANCE (diviseur 1) ;
                                              * 0 pour tout dessin non instancié : mêmes pipelines, même VkPipelineCache */
} cle_pipe;
typedef struct { uint64_t cle; int genre; uint32_t w, h, d, fmt, gen, niv; int conv; VkFormat vf;
                 VkImage img; VkDeviceMemory mem; VkImageView vue; } tex5;
/* passe 10r : tampon de sommets/index STATIQUE de la mémoire invitée → VkBuffer dédié copié une
 * fois (cle = adresse de l'objet invité, gen = sa génération de contenu, taille = O_W entier) */
#define MAX_TAMPONS    4096                   /* banc mission : 1 024 atteint (17,9 Mio) */
#define DALLE_TAILLE (8u << 20)               /* 21/10 : grandes dalles de sous-allocation des petits tampons */
#define MAX_DALLES   48
#define TAMPONS_PLAFOND (256u << 20)          /* octets mis en cache au plus : au-delà, anneau */
typedef struct { uint32_t cle, gen, taille; VkBuffer buf; VkDeviceMemory mem; uint8_t *map;
                 uint32_t remplacements; int volatil; int dalle; uint32_t off;
                 } tam10;   /* passe 10r : garde de volatilité, voir tampon_puiser ; 21/10 : dalle ≥ 0 =
                              sous-allouée dans D[dalle] à l'octet off (mem alors nulle), -1 = dédiée */
#define TAMPON_REMPL_MAX 2                    /* au-delà : tampon « volatile », servi par l'anneau pour de bon */
static struct {
    int pret;                                /* layout, anneau, factices créés */
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule mod[MAX_MODULES]; int ps[MAX_MODULES]; int nmod;
    tex5 t[MAX_TEX]; int nt;
    struct { cle_pipe k; VkPipeline p; } pipe[MAX_PIPES]; int npipe;
    struct { uint64_t k; VkSampler s; } smp[MAX_SAMPLERS]; int nsmp;
    uint16_t prof_module[MAX_MODULES];       /* passe 7v2 : par module PS, slots vus liés à une profondeur */
    uint64_t n_dessins_prof, n_ech_prof, n_slots_prof;   /* 7v2 : draws avec ≥1 sampler de profondeur, échantillonnages */
    VkBuffer anneau; VkDeviceMemory anneau_mem; uint8_t *anneau_ptr; VkDeviceSize pos;
    VkImage fimg[3]; VkDeviceMemory fmem[3]; VkImageView fvue[3];   /* factices blancs 2D, cube, 3D */
    uint64_t n_reels, n_refus_cible, n_refus_pipe, n_refus_place, n_tex_tel, o_tel, n_tex_ko;
    uint64_t n_niv_tel, n_tel_logiciel, o_logiciel, n_cubes_tel;   /* passe 7l : niveaux téléversés, téléversements décodés par le CPU */
    uint64_t n_fan;                          /* passe 7u : draws D3DPT_TRIANGLEFAN (portability_subset : triangleFans) */
    uint64_t n_stencil;                      /* 21/10 : draws avec test stencil actif (surlignage de sélection) */
    uint64_t n_inst_dessins, n_inst;         /* V2.2 : dessins à flux d'instance (ou instanceCount > 1), Σ instanceCount */
    uint32_t inst_max; int n_pipes_inst;     /* V2.2 : plus grand instanceCount ; pipelines à flux d'instance créés */
    /* passe 7c : descripteurs CLASSIQUES (sans VK_KHR_push_descriptor) — un set par draw, pris
     * dans le pool de SON emplacement (passe 10r v13 : V.empl[].pool), remis à zéro quand
     * l'emplacement revient (sa fence attendue : aucun set encore référencé par le GPU). */
    uint32_t pool_max;                       /* sets par soumission : maximum vu */
    uint64_t n_sets_alloues, n_sets_liberes, n_pool_raz, n_pool_plein;
    /* passe 10r : caches du chemin de dessin. cblk[] = dernier bloc de constantes (VS, PS)
     * envoyé : génération, emplacement dans l'anneau, époque de l'anneau — celle-ci est
     * incrémentée à chaque remise à zéro dans soumettre(), un emplacement réservé avant ne
     * vaut que pour son époque (0 = jamais envoyé, d'où R.epoque qui démarre à 1). */
    struct { uint32_t gen; VkDeviceSize off; uint64_t epoque; VkDeviceSize etendue; } cblk[2];   /* passe 10t : etendue = octets recopiés */
    uint64_t epoque;
    tam10 tam[MAX_TAMPONS]; int ntam; uint64_t o_tam;   /* cache des tampons statiques (octets : o_tam) */
    struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; } dalle[MAX_DALLES]; int ndalles;
    uint64_t o_dalles;                        /* 21/10 : octets réellement pris dans les dalles (bilan) */
    uint64_t n_cache_cvs, n_cache_cps, n_tam_flux, n_tam_index, o_evites, n_tam_remplaces;
    uint32_t n_tam_plafond;                  /* créations refusées par TAMPONS_PLAFOND */
    uint64_t n_tam_volatils, n_tam_volatils_servis;   /* passe 10r : tampons sortis du cache, dessins qu'ils ont renvoyés à l'anneau */
    uint64_t n_volatils_flux, n_volatils_index, o_volatils;   /* passe 10t : plages volatiles réutilisées sans recopie (octets : o_volatils) */
    uint64_t n_blocs, n_blocs_entiers, o_consts, o_plage;   /* passe 10t : blocs de constantes recopiés, dont entiers ; octets recopiés,
                                                             * octets épargnés par la plage lue (D3DV_CONST_TAILLE − recopié) */
} R;
#define POOL_SETS 4096                       /* sets par soumission (2 UBO + 16 samplers chacun) */

/* ---- passe 10r : recherche O(1) dans le chemin de dessin (profil sc2prof) --------
 * Cibles (V.c), textures (R.t) et pipelines (R.pipe) étaient parcourus linéairement
 * à chaque appel du chemin de dessin. Adressage ouvert, sondage linéaire, case vide
 * = -1, case occupée = indice dans le tableau du cache (modèle : lifted_find de
 * sc2_runtime.c). Ajouts en fin de tableau uniquement, insertion au même endroit ;
 * seule retraite : l'annulation immédiate d'un ajout de texture échoué (R.nt--),
 * qui vise le plus grand indice vivant — le dernier inséré, donc vider sa case ne
 * coupe aucune chaîne de sondage. Tout se passe sous g_mu : aucune synchronisation
 * propre. Tables ≥ 2 × la capacité des tableaux : débordement impossible en pratique ;
 * par prudence, drapeau + repli sur le parcours linéaire d'origine plutôt qu'échouer. */
#define HACHE_CIBLES_BITS 10                                 /* 1 024 cases ≥ 2 × MAX_CIBLES (512) */
#define HACHE_CIBLES_TAILLE (1u << HACHE_CIBLES_BITS)
#define HACHE_CIBLES_MASQUE (HACHE_CIBLES_TAILLE - 1u)
#define HACHE_TEX_BITS    13                                 /* 8 192 cases ≥ 2 × MAX_TEX (4 096) */
#define HACHE_TEX_TAILLE    (1u << HACHE_TEX_BITS)
#define HACHE_TEX_MASQUE    (HACHE_TEX_TAILLE - 1u)
#define HACHE_PIPE_BITS   15                                 /* 32 768 cases ≥ 2 × MAX_PIPES (16 384) */
#define HACHE_PIPE_TAILLE   (1u << HACHE_PIPE_BITS)
#define HACHE_PIPE_MASQUE   (HACHE_PIPE_TAILLE - 1u)
#define HACHE_TAM_BITS    13                                 /* passe 10r : 8 192 cases ≥ 2 × MAX_TAMPONS (4 096) */
#define HACHE_TAM_TAILLE    (1u << HACHE_TAM_BITS)
#define HACHE_TAM_MASQUE    (HACHE_TAM_TAILLE - 1u)
static int32_t g_hache_cibles[HACHE_CIBLES_TAILLE];
static int32_t g_hache_tex[HACHE_TEX_TAILLE];
static int32_t g_hache_pipe[HACHE_PIPE_TAILLE];
static int32_t g_hache_tam[HACHE_TAM_TAILLE];
static int g_hache_cibles_plein, g_hache_tex_plein, g_hache_pipe_plein, g_hache_tam_plein;
static int g_tex_tombes, g_tam_tombes, g_cib_tombes;   /* V5.1 : pierres tombales (-2) dans g_hache_tex / tam / cibles */
static int g_cib_libres[MAX_CIBLES], g_ncib_libres;     /* V5.1 : indices de cibles oubliées */
static uint64_t g_n_cib_oubliees, g_n_cib_gardees;
/* V5.1 (24/10) : entrées oubliées (d3dv_oublier) — clé morte, indice remis en service par la prochaine création */
#define CLE_MORTE (~0ull)
static int g_tex_libres[MAX_TEX], g_ntex_libres, g_tam_libres[MAX_TAMPONS], g_ntam_libres;
static uint64_t g_n_tex_oubliees, g_n_tam_oublies, g_n_hache_refaites;
__attribute__((constructor)) static void haches_init(void)   /* BSS à 0 or l'indice 0 est valide → tout à -1 */
{
    memset(g_hache_cibles, 0xFF, sizeof g_hache_cibles);
    memset(g_hache_tex, 0xFF, sizeof g_hache_tex);
    memset(g_hache_pipe, 0xFF, sizeof g_hache_pipe);
    memset(g_hache_tam, 0xFF, sizeof g_hache_tam);
}
/* Knuth 64 bits, bits hauts du produit (analogue 32 bits de lifted_find) */
static inline uint32_t hache_cle64(uint64_t cle, uint32_t bits)
{
    return (uint32_t)((cle * 0x9E3779B97F4A7C15ull) >> (64 - bits));
}
/* FNV-1a 64 sur les MÊMES octets que le memcmp — la clé est entièrement initialisée
 * (memset dans d3dv_dessiner avant remplissage), bourrage compris */
static uint32_t hache_cle_pipe(const cle_pipe *k, uint32_t bits)
{
    uint64_t h = 14695981039346656037ull;
    const uint8_t *o = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ o[i]) * 1099511628211ull;
    return (uint32_t)(h >> (64 - bits));
}
/* cibles : V.nc n'avance qu'à la réussite de cible_creer, aucune retraite ; une même
 * clé peut être réallouée en place (Reset) — l'indice et la clé ne changent pas */
static int cible_scan(uint64_t cle)
{
    for (int i = 0; i < V.nc; i++) if (V.c[i].cle == cle) return i;
    return -1;
}
static int cible_trouver(uint64_t cle)
{
    if (!g_hache_cibles_plein) {
        uint32_t h = hache_cle64(cle, HACHE_CIBLES_BITS);
        for (uint32_t n = 0; n < HACHE_CIBLES_TAILLE; n++) {
            int32_t e = g_hache_cibles[h];
            if (e == -1) return -1;                  /* case vide : absente (V5.1 : -2 = pierre tombale, on continue) */
            if (e >= 0 && V.c[e].cle == cle) return e;
            h = (h + 1) & HACHE_CIBLES_MASQUE;
        }
    }
    return cible_scan(cle);                          /* table pleine : parcours d'origine (ne doit pas arriver) */
}
static void cible_noter(int id)                      /* sous g_mu : V.c[id] vient d'être ajoutée */
{
    uint32_t h = hache_cle64(V.c[id].cle, HACHE_CIBLES_BITS);
    for (uint32_t n = 0; n < HACHE_CIBLES_TAILLE && g_hache_cibles[h] >= 0; n++) h = (h + 1) & HACHE_CIBLES_MASQUE;
    if (g_hache_cibles[h] == -2) g_cib_tombes--;      /* V5.1 : pierre tombale reprise */
    if (g_hache_cibles[h] < 0) g_hache_cibles[h] = id; else g_hache_cibles_plein = 1;
}
/* textures : la clé d'une entrée existante ne change jamais (réallocation en place
 * si dimensions différentes) ; R.nt peut redescendre (image_creer raté → R.nt--) */
static int tex_scan(uint64_t cle)
{
    for (int i = 0; i < R.nt; i++) if (R.t[i].cle == cle) return i;
    return -1;
}
static int tex_trouver(uint64_t cle)
{
    if (!g_hache_tex_plein) {
        uint32_t h = hache_cle64(cle, HACHE_TEX_BITS);
        for (uint32_t n = 0; n < HACHE_TEX_TAILLE; n++) {
            int32_t e = g_hache_tex[h];
            if (e == -1) return -1;                  /* V5.1 : -2 = pierre tombale (entrée oubliée), on continue */
            if (e >= 0 && R.t[e].cle == cle) return e;
            h = (h + 1) & HACHE_TEX_MASQUE;
        }
    }
    return tex_scan(cle);
}
static void tex_noter(int id)                        /* sous g_mu : R.t[id] vient d'être ajoutée */
{
    uint32_t h = hache_cle64(R.t[id].cle, HACHE_TEX_BITS);
    for (uint32_t n = 0; n < HACHE_TEX_TAILLE && g_hache_tex[h] >= 0; n++) h = (h + 1) & HACHE_TEX_MASQUE;
    if (g_hache_tex[h] == -2) g_tex_tombes--;         /* V5.1 : pierre tombale reprise */
    if (g_hache_tex[h] < 0) g_hache_tex[h] = id; else g_hache_tex_plein = 1;
}
/* retire le DERNIER indice inséré (annulation immédiate de tex_noter) : aucune
 * chaîne de sondage ne passe par sa case, la vider est exact — V5.1 : seulement tant qu'aucune pierre
 * tombale n'existe (l'insertion a pu en reprendre une, que des chaînes traversent) ; sinon pierre tombale.
 * Sert aussi à l'oubli d'une entrée quelconque (d3dv_oublier). */
static void tex_retirer_(int id, int tombe)
{
    uint32_t h = hache_cle64(R.t[id].cle, HACHE_TEX_BITS);
    for (uint32_t n = 0; n < HACHE_TEX_TAILLE; n++) {
        if (g_hache_tex[h] == id) { if (tombe || g_tex_tombes) { g_hache_tex[h] = -2; g_tex_tombes++; } else g_hache_tex[h] = -1; return; }
        if (g_hache_tex[h] == -1) return;            /* pas dans la table (insertion débordée) */
        h = (h + 1) & HACHE_TEX_MASQUE;
    }
}
static void tex_retirer(int id) { tex_retirer_(id, 0); }
/* pipelines : ajout en fin uniquement, jamais de retraite ni de réécriture ;
 * collision de hachage → le memcmp sur sizeof cle_pipe octets tranche */
static int pipe_scan(const cle_pipe *k)
{
    for (int i = 0; i < R.npipe; i++) if (!memcmp(&R.pipe[i].k, k, sizeof *k)) return i;
    return -1;
}
static int pipe_trouver(const cle_pipe *k)
{
    if (!g_hache_pipe_plein) {
        uint32_t h = hache_cle_pipe(k, HACHE_PIPE_BITS);
        for (uint32_t n = 0; n < HACHE_PIPE_TAILLE; n++) {
            int32_t e = g_hache_pipe[h];
            if (e < 0) return -1;
            if (!memcmp(&R.pipe[e].k, k, sizeof *k)) return e;
            h = (h + 1) & HACHE_PIPE_MASQUE;
        }
    }
    return pipe_scan(k);
}
static void pipe_noter(int id)                       /* sous g_mu : R.pipe[id] vient d'être ajouté */
{
    uint32_t h = hache_cle_pipe(&R.pipe[id].k, HACHE_PIPE_BITS);
    for (uint32_t n = 0; n < HACHE_PIPE_TAILLE && g_hache_pipe[h] >= 0; n++) h = (h + 1) & HACHE_PIPE_MASQUE;
    if (g_hache_pipe[h] < 0) g_hache_pipe[h] = id; else g_hache_pipe_plein = 1;
}
/* passe 10r tampons : clé = adresse de l'objet invité ; une entrée est REMPLACÉE en place (nouvelle
 * génération ou nouvelle taille), sa clé ne quitte jamais la table — pas de retraite, l'ancien
 * VkBuffer part en libération différée : l'emplacement en cours le détruira quand il revient,
 * après sa fence (passe 10r v13) */
static int tam_scan(uint64_t cle)
{
    for (int i = 0; i < R.ntam; i++) if (R.tam[i].cle == cle) return i;
    return -1;
}
static int tam_trouver(uint64_t cle)
{
    if (!g_hache_tam_plein) {
        uint32_t h = hache_cle64(cle, HACHE_TAM_BITS);
        for (uint32_t n = 0; n < HACHE_TAM_TAILLE; n++) {
            int32_t e = g_hache_tam[h];
            if (e == -1) return -1;                  /* case vide : absent (V5.1 : -2 = pierre tombale) */
            if (e >= 0 && R.tam[e].cle == cle) return e;
            h = (h + 1) & HACHE_TAM_MASQUE;
        }
    }
    return tam_scan(cle);                             /* table pleine : parcours d'origine */
}
static void tam_noter(int id)                        /* sous g_mu : R.tam[id] vient d'être ajoutée */
{
    uint32_t h = hache_cle64(R.tam[id].cle, HACHE_TAM_BITS);
    for (uint32_t n = 0; n < HACHE_TAM_TAILLE && g_hache_tam[h] >= 0; n++) h = (h + 1) & HACHE_TAM_MASQUE;
    if (g_hache_tam[h] == -2) g_tam_tombes--;         /* V5.1 : pierre tombale reprise */
    if (g_hache_tam[h] < 0) g_hache_tam[h] = id; else g_hache_tam_plein = 1;
}

static int echec(const char *quoi, VkResult r)
{
    LOG("[VK] backend inactif : %s (VkResult %d) — device D3D9 factice seul", quoi, (int)r);
    return 0;
}

static int32_t type_memoire(uint32_t bits, VkMemoryPropertyFlags voulu)
{
    for (uint32_t i = 0; i < V.memp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (V.memp.memoryTypes[i].propertyFlags & voulu) == voulu) return (int32_t)i;
    return -1;
}

/* ---- 21/10 : dalles ------------------------------------------------------------------------
 * tam_creer() faisait pour CHAQUE tampon statique un vkCreateBuffer + vkAllocateMemory +
 * vkMapMemory (≈ 1 000 par mission : sur Android, autant d'ioctls et d'à-coups au chargement).
 * Les petits tampons (≤ quart de dalle) sont désormais sous-alloués dans de grandes dalles :
 * un VkBuffer VERTEX|INDEX + une VkDeviceMemory HOST_VISIBLE|HOST_COHERENT (même type_memoire
 * que le chemin dédié), mappés une fois pour toutes. Dalles créées à la demande, JAMAIS
 * détruites avant la fin (les VkBuffer des plages meurent avec elles : pas de vkDestroyBuffer
 * en cours de route). La gestion des étendues libres (trouvée, prise, réinsertion, fusion) est
 * écrite en fonctions static pures sur la struct de la dalle, sans appel Vulkan — un test
 * unitaire pourra la viser plus tard. SC2_VK_DALLES=0 : chemin dédié partout, comme avant. */
#define DALLE_LIBRE_MAX 256                   /* étendues LIBRES suivies par dalle (fusionnées, en pratique ≪) */
static int g_dalles = -1;                     /* SC2_VK_DALLES, lu une fois ; 0 = chemin dédié partout */
typedef struct { uint32_t off, taille; } etendue;
typedef struct { etendue libre[DALLE_LIBRE_MAX]; int nlibre; } dalle_libres;   /* triées par décalage */

/* Trouve la première étendue ≥ t (first-fit) dans une liste triée : rend son indice, -1 si rien. */
static int libres_trouver(const dalle_libres *l, uint32_t t)
{
    for (int i = 0; i < l->nlibre; i++) if (l->libre[i].taille >= t) return i;
    return -1;
}

/* Prend t octets à l'étendue i (t ≤ sa taille) : l'étendue rétrécit par la gauche. */
static void libres_prendre(dalle_libres *l, int i, uint32_t t)
{
    l->libre[i].off += t; l->libre[i].taille -= t;
    if (l->libre[i].taille) return;
    memmove(&l->libre[i], &l->libre[i + 1], (size_t)(l->nlibre - i - 1) * sizeof *l->libre);
    l->nlibre--;
}

/* Réinsère [off, off+taille[ triée par décalage et fusionne avec les voisines immédiates.
 * Rend 0, -1 si la liste est pleine (l'appelant renonce : la plage est simplement perdue). */
static int libres_rendre(dalle_libres *l, uint32_t off, uint32_t taille)
{
    if (!taille) return 0;
    int i = 0;
    while (i < l->nlibre && l->libre[i].off < off) i++;
    int avant = i > 0 && l->libre[i - 1].off + l->libre[i - 1].taille == off;
    int apres = i < l->nlibre && off + taille == l->libre[i].off;
    if (avant) {
        l->libre[i - 1].taille += taille;                /* agrandit l'étendue précédente */
        if (apres) {                                     /* comble exactement le trou : avale aussi la suivante */
            l->libre[i - 1].taille += l->libre[i].taille;
            memmove(&l->libre[i], &l->libre[i + 1], (size_t)(l->nlibre - i - 1) * sizeof *l->libre);
            l->nlibre--;
        }
        return 0;
    }
    if (apres) { l->libre[i].off = off; l->libre[i].taille += taille; return 0; }
    if (l->nlibre >= DALLE_LIBRE_MAX) return -1;
    memmove(&l->libre[i + 1], &l->libre[i], (size_t)(l->nlibre - i) * sizeof *l->libre);
    l->libre[i].off = off; l->libre[i].taille = taille; l->nlibre++;
    return 0;
}
static dalle_libres g_libres[MAX_DALLES];     /* étendues libres de chaque dalle, sous g_mu */

static uint32_t arrondi256(uint32_t t)        /* granularité des plages (limite la fragmentation) */
{
    return (t + 0xffu) & ~0xffu;
}
static int dalles_actives(void)               /* SC2_VK_DALLES, lu une fois ; 0 = chemin dédié partout */
{
    if (g_dalles < 0) { const char *e = getenv("SC2_VK_DALLES"); g_dalles = !(e && *e == '0'); }
    return g_dalles;
}

/* Crée la dalle n° i : VkBuffer VERTEX|INDEX de DALLE_TAILLE + VkDeviceMemory HOST_VISIBLE|
 * HOST_COHERENT (même type_memoire que le chemin dédié de tam_creer), mappée une fois pour
 * toutes. Rend 0, -1 si un appel Vulkan échoue (alors rien à défaire du côté des plages). */
static int dalle_creer(int i)                 /* sous g_mu */
{
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = DALLE_TAILLE,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT };
    VkBuffer b;
    if (vkCreateBuffer(V.dev, &bi, NULL, &b)) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, b, &mr);
    int32_t ty = type_memoire(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = (uint32_t)ty };
    VkDeviceMemory m = VK_NULL_HANDLE; uint8_t *map = NULL;
    if (ty < 0 || vkAllocateMemory(V.dev, &ai, NULL, &m) || vkBindBufferMemory(V.dev, b, m, 0)
        || vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&map)) {
        vkDestroyBuffer(V.dev, b, NULL);
        if (m) vkFreeMemory(V.dev, m, NULL);
        return -1;
    }
    R.dalle[i].buf = b; R.dalle[i].mem = m; R.dalle[i].map = map;
    g_libres[i].nlibre = 1; g_libres[i].libre[0].off = 0; g_libres[i].libre[0].taille = DALLE_TAILLE;
    if (!i) LOG("[VK] 21/10 : petits tampons statiques sous-alloués dans des dalles de %u Mio "
                "(SC2_VK_DALLES=0 pour couper)", DALLE_TAILLE >> 20);
    return 0;
}

/* Sous-alloue taille octets (arrondis à 256) : first-fit dans les dalles existantes, sinon
 * nouvelle dalle créée à la demande. Rend 0 avec l'indice de dalle et le décalage, -1 si rien
 * de libre et plus de dalle possible (MAX_DALLES atteint ou création en échec). sous g_mu. */
static int dalle_prendre(uint32_t taille, int *dalle, uint32_t *off)
{
    taille = arrondi256(taille);
    if (!taille || taille > DALLE_TAILLE) return -1;
    for (int i = 0; i < R.ndalles; i++) {
        int j = libres_trouver(&g_libres[i], taille);
        if (j < 0) continue;
        *dalle = i; *off = g_libres[i].libre[j].off;
        libres_prendre(&g_libres[i], j, taille);
        R.o_dalles += taille;
        return 0;
    }
    int i = R.ndalles;
    if (i >= MAX_DALLES || dalle_creer(i)) return -1;
    R.ndalles = i + 1;
    *dalle = i; *off = 0;
    libres_prendre(&g_libres[i], 0, taille);
    R.o_dalles += taille;
    return 0;
}

/* Rend une plage prise par dalle_prendre : réinsertion triée + fusion des voisines. taille DOIT
 * être l'arrondi 256 passé à dalle_prendre (l'appelant l'a conservé). Liste pleine : la plage
 * est perdue (autant d'octets muets — 256 étendues fusionnées, irréaliste). */
static void dalle_rendre(int dalle, uint32_t off, uint32_t taille)   /* sous g_mu */
{
    if (dalle < 0 || dalle >= R.ndalles) return;
    R.o_dalles -= taille;
    libres_rendre(&g_libres[dalle], off, taille);
}

static int format_ok(VkFormat f, VkFormatFeatureFlags voulu)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(V.pd, f, &p);
    return (p.optimalTilingFeatures & voulu) == voulu;
}

/* passe 10r (v13) : SC2_VK_SYNCHRONE=1 — comportement d'avant (un seul emplacement, soumission
 * suivie de vkQueueWaitIdle) ; lu une fois */
static int mode_synchrone(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SC2_VK_SYNCHRONE"); v = e && *e == '1'; }
    return v;
}
static double maintenant_ms(void)   /* passe 10r (v13) : cumul du temps d'attente des fences ([BILAN] [VOL]) */
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* ---- passe 10s (chrono GPU) : temps GPU par segment de commandes ------------------
 * SC2_VK_CHRONO_GPU (lu une fois) : absent ou 0 → inactif (aucune commande Vulkan,
 * aucun coût) ; 1 → compte rendu toutes les 100 frames ; N > 1 → toutes les N frames.
 * Un VkQueryPool de timestamps par emplacement, remis à zéro en tête de chaque tampon ;
 * chrono_segment() écrit un timestamp (BOTTOM_OF_PIPE, toujours hors passe) qui FERME le
 * segment précédent et en ouvre un étiqueté (genre, cible). Les résultats sont relevés
 * (vkGetQueryPoolResults, 64 bits — la fence est passée) quand la fence de l'emplacement
 * est attendue, cumulés par (genre, cible) dans une fenêtre courante (ligne [GPU]
 * périodique) et sur tout le run ([BILAN] [GPU]). */
#define CHRONO_MAX 4096                     /* requêtes par pool = par tampon */
enum { CH_HORS_PASSE = 0, CH_PASSE, CH_CLEAR_PROF, CH_COPIE, CH_TELEVERSEMENT, CH_RELECTURE, CH_GENRES };
static uint32_t chrono_gpu_period(void)
{
    static uint32_t v; static int lu;
    if (!lu) {
        lu = 1; const char *e = getenv("SC2_VK_CHRONO_GPU");
        if (e && *e) { long n = strtol(e, NULL, 10); v = n == 1 ? 100 : n > 1 ? (uint32_t)n : 0; }
    }
    return v;
}
typedef struct { uint64_t ns, n; } chrono_cum;          /* un (genre, cible) : ns cumulés, segments */
typedef struct {
    uint64_t frames, tronques;                          /* frames présentées ; tampons au plafond de requêtes */
    chrono_cum g[CH_GENRES];                            /* total par genre (toutes cibles fondues) */
    chrono_cum p[3][MAX_CIBLES];                        /* par cible : PASSE, CLEAR_PROF, COPIE */
} chrono_fen;
static chrono_fen ch_win, ch_tot;
static struct {
    int actif;
    uint32_t period;                                    /* compte rendu toutes les N frames */
    uint32_t bits;                                      /* timestampValidBits de la file */
    double tick;                                        /* timestampPeriod : ns par tick */
    VkQueryPool pool[EN_VOL];
    uint32_t nq[EN_VOL];                                /* requêtes écrites dans le tampon de l'emplacement */
    uint32_t attente[EN_VOL];                           /* soumis, à relever quand sa fence est attendue */
    int plein[EN_VOL];                                  /* plafond atteint : plus d'écriture pour CE tampon */
    struct { uint8_t genre; int16_t cible; } etiq[EN_VOL][CHRONO_MAX];
} CH;

/* ---- passe 10s (cache de pipelines) : VkPipelineCache persistant --------------------
 * Le premier draw qui a besoin d'un pipeline le compile dans le pilote (2,5 % du temps du
 * fil de jeu dans libllvm-qgl.so sur l'Adreno cible, à-coups à chaque nouveauté).
 * SC2_VK_CACHE_PIPELINES, lue une fois : 0 → désactivé (VK_NULL_HANDLE aux créations,
 * comme avant) ; un chemin → ce fichier ; absente → $SC2_ROOT/vk-pipelines.bin si SC2_ROOT
 * est défini, sinon vk-pipelines.bin dans le dossier courant. Chargé au démarrage (fichier
 * de moins de 256 Mio en pInitialData — les données d'un autre pilote ou d'un autre GPU
 * sont rejetées par le pilote lui-même : cache vide), passé aux deux vkCreateGraphicsPipelines
 * (pipeline_pour, pipeline5), réécrit par écriture atomique (<chemin>.tmp puis rename :
 * l'app peut être tuée sans passer par le bilan, jamais de fichier tronqué au chemin final)
 * toutes les 600 frames dans d3dv_present et une dernière fois dans d3dv_bilan — seulement
 * si un pipeline a été créé depuis la dernière sauvegarde : une fois le jeu « chaud », plus
 * aucune écriture. Le pilote rendant souvent des données de même taille (pipelines recréés
 * déjà présents dans le cache), une taille égale à la référence — dernière sauvegarde, sinon
 * octets chargés au démarrage — suffit aussi à sauter l'écriture (compteur n_inchangees).
 * Tout l'état vit sous g_mu comme le reste du backend. */
#define PCACHE_PLAFOND (256ull << 20)        /* octets chargés au plus */
static struct {
    int desactive;                           /* SC2_VK_CACHE_PIPELINES=0 ou chemin trop long */
    const char *raison;                      /* pourquoi inactif : journal de démarrage et [BILAN] [PIPECACHE] */
    char chemin[512];                        /* résolu une fois ; la sauvegarde écrit chemin + ".tmp" */
    size_t octets_charges;                   /* octets du fichier acceptés au démarrage (0 : vide) */
    VkPipelineCache cache;                   /* VK_NULL_HANDLE : créations sans cache, exactement comme avant */
    uint64_t n_pipelines;                    /* pipelines créés par les deux vkCreateGraphicsPipelines */
    uint64_t depuis_sauvegarde;              /* créés depuis la dernière sauvegarde (garde d'écriture) */
    double ms, ms_max;                       /* temps cumulé / maximum d'un appel dans vkCreateGraphicsPipelines */
    uint32_t n_sauvegardes;                  /* écritures réussies */
    uint32_t n_inchangees;                   /* sauvegardes sautées : taille rendue égale à la référence */
    size_t derniere_taille;                  /* octets de la dernière sauvegarde */
    uint32_t n_echecs;                       /* écritures ratées : journalisées au plus 3 fois */
} PC;

/* résolution de SC2_VK_CACHE_PIPELINES, lue une seule fois ; NULL : désactivé */
static const char *pcache_chemin(void)
{
    static int lu;
    if (lu) return PC.desactive ? NULL : PC.chemin;
    lu = 1;
    const char *e = getenv("SC2_VK_CACHE_PIPELINES");
    if (e && *e && !strcmp(e, "0")) { PC.desactive = 1; PC.raison = "SC2_VK_CACHE_PIPELINES=0"; return NULL; }
    int n;
    if (e && *e) n = snprintf(PC.chemin, sizeof PC.chemin, "%s", e);
    else {
        const char *rac = getenv("SC2_ROOT");
        n = rac && *rac ? snprintf(PC.chemin, sizeof PC.chemin, "%s/vk-pipelines.bin", rac)
                        : snprintf(PC.chemin, sizeof PC.chemin, "vk-pipelines.bin");
    }
    if (n < 0 || n >= (int)sizeof PC.chemin) {
        PC.chemin[0] = 0; PC.desactive = 1; PC.raison = "chemin trop long";
        return NULL;
    }
    return PC.chemin;
}

/* chargement au démarrage (init_interne, juste après vkCreateDevice ; sous g_mu) : le
 * fichier en pInitialData s'il existe et fait moins de 256 Mio */
static void pcache_charger(void)
{
    const char *c = pcache_chemin();
    if (!c) { LOG("[VK] passe 10s (cache de pipelines) : désactivé (%s)", PC.raison); return; }
    void *d = NULL;
    size_t n = 0;
    const char *vide = "absent — cache vide créé";   /* pourquoi le cache démarre vide */
    char vide_gros[96];                      /* variante « trop gros », remplie ci-dessous si utile */
    struct stat st;
    if (!stat(c, &st) && st.st_size > 0) {
        if ((uint64_t)st.st_size >= PCACHE_PLAFOND) {
            snprintf(vide_gros, sizeof vide_gros, "trop gros (%lld octets ≥ plafond 256 Mio) — non chargé, cache vide créé",
                     (long long)st.st_size);
            vide = vide_gros;
        } else {
            FILE *f = fopen(c, "rb");
            if (!f) vide = "illisible — cache vide créé";
            else {
                d = malloc((size_t)st.st_size);
                if (d && fread(d, 1, (size_t)st.st_size, f) == (size_t)st.st_size) n = (size_t)st.st_size;
                else { free(d); d = NULL; vide = "illisible — cache vide créé"; }
                fclose(f);
            }
        }
    }
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = n, .pInitialData = d };
    VkResult r = vkCreatePipelineCache(V.dev, &ci, NULL, &PC.cache);
    if (r && n) {                            /* données rejetées (autre pilote, autre GPU) : recréé vide */
        ci.initialDataSize = 0; ci.pInitialData = NULL;
        r = vkCreatePipelineCache(V.dev, &ci, NULL, &PC.cache);
        vide = "rejeté par le pilote — cache vide recréé";
    }
    free(d);
    if (r) {
        PC.cache = VK_NULL_HANDLE; PC.raison = "vkCreatePipelineCache en échec";
        LOG("[VK] passe 10s (cache de pipelines) : vkCreatePipelineCache en échec (VkResult %d) — pipelines sans cache", (int)r);
        return;
    }
    PC.octets_charges = n;
    if (n) LOG("[VK] passe 10s (cache de pipelines) : %s : %llu octets chargés", c, (unsigned long long)n);
    else LOG("[VK] passe 10s (cache de pipelines) : %s : %s", c, vide);
}

/* les deux vkCreateGraphicsPipelines passent ici (sous g_mu) : pipelines créés et ms
 * comptés pour [BILAN] [PIPECACHE] ; crees = 0 si l'appel a échoué (aucun pipeline né) */
static void pcache_compter(int crees, double ms)
{
    PC.n_pipelines += (uint64_t)crees;
    PC.depuis_sauvegarde += (uint64_t)crees;
    PC.ms += ms;
    if (ms > PC.ms_max) PC.ms_max = ms;
}

/* échec d'écriture : une ligne de journal, au plus 3 (au-delà, silence), le jeu continue */
static void pcache_echec(const char *action, const char *cause)
{
    PC.n_echecs++;
    if (PC.n_echecs <= 3) LOG("[VK] passe 10s (cache de pipelines) : %s impossible — %s (%s)", action, PC.chemin, cause);
}

/* réécriture atomique (d3dv_present toutes les 600 frames sous g_mu, puis bilan) :
 * vkGetPipelineCacheData → <chemin>.tmp → rename. N'écrit rien sans cache, si aucun
 * pipeline n'a été créé depuis la dernière sauvegarde (jeu « chaud » : zéro écriture)
 * ni si la taille rendue égale la référence (dernière sauvegarde, sinon octets chargés) :
 * le pilote ne fait que recréer des pipelines déjà dans le cache, rien n'a changé. */
static void pcache_sauver(void)
{
    if (!PC.cache || !PC.depuis_sauvegarde) return;
    size_t n = 0;
    VkResult r = vkGetPipelineCacheData(V.dev, PC.cache, &n, NULL);
    if (r || !n) {
        char cause[40];
        snprintf(cause, sizeof cause, r ? "VkResult %d" : "données nulles", (int)r);
        pcache_echec("lecture du cache", cause);
        return;
    }
    size_t reference = PC.n_sauvegardes > 0 ? PC.derniere_taille : PC.octets_charges;
    if (n == reference) {                    /* rien de neuf : une écriture de plusieurs Mo évitée */
        static int dit;                      /* au plus une ligne de journal, la toute première fois */
        if (!dit) {
            dit = 1;
            LOG("[VK] passe 10s (cache de pipelines) : taille inchangée (%llu octets) — écriture sautée, les "
                "suivantes sont comptées au bilan", (unsigned long long)n);
        }
        PC.depuis_sauvegarde = 0;
        PC.n_inchangees++;
        return;
    }
    void *d = malloc(n);
    if (!d) { pcache_echec("lecture du cache", "pas assez de mémoire"); return; }
    if ((r = vkGetPipelineCacheData(V.dev, PC.cache, &n, d))) {
        char cause[40];
        snprintf(cause, sizeof cause, "VkResult %d", (int)r);
        pcache_echec("lecture du cache", cause);
        free(d);
        return;
    }
    char tmp[sizeof PC.chemin + 8];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", PC.chemin) >= (int)sizeof tmp) {
        pcache_echec("écriture", "chemin trop long");
        free(d);
        return;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) { pcache_echec("écriture", strerror(errno)); free(d); return; }
    size_t e = fwrite(d, 1, n, f);
    int ferme = fclose(f);                   /* toujours fermé, même après écriture courte */
    if (e != n || ferme) { pcache_echec("écriture", strerror(errno)); free(d); return; }
    if (rename(tmp, PC.chemin)) { pcache_echec("écriture (rename)", strerror(errno)); free(d); return; }
    free(d);
    PC.depuis_sauvegarde = 0;
    PC.n_sauvegardes++;
    PC.derniere_taille = n;
    LOG("[VK] passe 10s (cache de pipelines) : %llu octets sauvegardés → %s", (unsigned long long)n, PC.chemin);
}

static void chrono_cumuler(int genre, int cible, uint64_t ns)   /* sous g_mu */
{
    ch_win.g[genre].ns += ns; ch_win.g[genre].n++;
    ch_tot.g[genre].ns += ns; ch_tot.g[genre].n++;
    if (genre >= CH_PASSE && genre <= CH_COPIE && cible >= 0 && cible < MAX_CIBLES) {
        ch_win.p[genre - 1][cible].ns += ns; ch_win.p[genre - 1][cible].n++;
        ch_tot.p[genre - 1][cible].ns += ns; ch_tot.p[genre - 1][cible].n++;
    }
}
/* timestamp qui ferme le segment précédent et en ouvre un étiqueté (genre, cible) ; pool
 * plein : plus rien pour CE tampon, compté comme tronqué (une fois par tampon) */
static void chrono_segment(int genre, int cible)
{
    if (!CH.actif) return;
    int i = V.cur;
    if (CH.nq[i] >= CHRONO_MAX) {
        if (!CH.plein[i]) { CH.plein[i] = 1; ch_win.tronques++; ch_tot.tronques++; }
        return;
    }
    vkCmdWriteTimestamp(CB, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, CH.pool[i], CH.nq[i]);
    CH.etiq[i][CH.nq[i]].genre = (uint8_t)genre;
    CH.etiq[i][CH.nq[i]].cible = (int16_t)cible;
    CH.nq[i]++;
}
/* la fence de l'emplacement vient d'être attendue : ses timestamps sont lisibles — la
 * requête k ferme le segment étiqueté k-1 ; attente remise à zéro : jamais relu deux fois */
static void chrono_relever(int i)
{
    if (!CH.actif || !CH.attente[i]) return;
    uint32_t n = CH.attente[i];
    CH.attente[i] = 0;
    static uint64_t t[CHRONO_MAX];                      /* sous g_mu */
    if (vkGetQueryPoolResults(V.dev, CH.pool[i], 0, n, sizeof t, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT))
        return;                                         /* fence passée : ne devrait pas arriver */
    uint64_t m = CH.bits >= 64 ? ~0ull : (1ull << CH.bits) - 1ull;
    for (uint32_t k = 1; k < n; k++)
        chrono_cumuler(CH.etiq[i][k - 1].genre, CH.etiq[i][k - 1].cible,
                       (uint64_t)((double)((t[k] & m) - (t[k - 1] & m)) * CH.tick + 0.5));
}
/* append borné : o avance dans l au plus à taille-1 */
static int ch_cat(char *l, int taille, int o, const char *fmt, ...)
{
    if (o >= taille - 1) return o;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(l + o, (size_t)(taille - o), fmt, ap);
    va_end(ap);
    if (k < 0 || k > taille - o - 1) k = taille - o - 1;
    return o + k;
}
/* « <frames> frames : X ms/frame mesurés ; passes : <top cibles les plus chères> ;
 * hors passes … ; clears prof … ; copies … ; téléversements … ; relectures … ; tronqués » */
static void chrono_ligne(char *l, int taille, const chrono_fen *f, int top)
{
    double frames = f->frames ? (double)f->frames : 1.0;
    uint64_t total = 0;
    for (int g = 0; g < CH_GENRES; g++) total += f->g[g].ns;
    int o = ch_cat(l, taille, 0, "%llu frames : %.1f ms/frame mesurés ; passes :",
                   (unsigned long long)f->frames, (double)total / frames / 1e6);
    static uint64_t ns[MAX_CIBLES];                     /* passes de la fenêtre, consommées par le tri (sous g_mu) */
    for (int i = 0; i < MAX_CIBLES; i++) ns[i] = f->p[0][i].ns;
    int naff = 0;
    for (int rang = 0; rang < top; rang++) {            /* top cibles par temps de PASSE */
        int best = -1;
        for (int i = 0; i < MAX_CIBLES; i++) if (ns[i] && (best < 0 || ns[i] > ns[best])) best = i;
        if (best < 0) break;
        o = ch_cat(l, taille, o, "%s n°%d %ux%u fmt %u%s %.1f ms (%.1f/frame)", naff ? "," : "",
                   best, V.c[best].w, V.c[best].h, V.c[best].fmt, V.c[best].prof ? " prof" : "",
                   (double)ns[best] / frames / 1e6, (double)f->p[0][best].n / frames);
        ns[best] = 0; naff++;
    }
    if (!naff) o = ch_cat(l, taille, o, "aucune");
    ch_cat(l, taille, o, " ; hors passes %.1f ; clears prof %.1f ; copies %.1f ; téléversements %.1f ; "
           "relectures %.1f ; %llu tampon%s tronqué%s",
           (double)f->g[CH_HORS_PASSE].ns / frames / 1e6, (double)f->g[CH_CLEAR_PROF].ns / frames / 1e6,
           (double)f->g[CH_COPIE].ns / frames / 1e6, (double)f->g[CH_TELEVERSEMENT].ns / frames / 1e6,
           (double)f->g[CH_RELECTURE].ns / frames / 1e6, (unsigned long long)f->tronques,
           f->tronques > 1 ? "s" : "", f->tronques > 1 ? "s" : "");
}

/* ---- initialisation ------------------------------------------------------- */
#ifdef __APPLE__
/* Passe 7u — ordre de chargement explicite (macOS) : (1) SC2_VK_LIB (chemin complet ; s'il échoue, pas de repli :
 * le pilote demandé est celui qu'on mesure), (2) libMoltenVK.dylib à côté de l'exécutable (paquet embarqué),
 * (3) loader Khronos libvulkan.1.dylib (couches de validation), (4) MoltenVK Homebrew, (5) libMoltenVK.dylib du
 * chemin de recherche. MoltenVK exporte lui-même vkGetInstanceProcAddr : le loader n'est pas requis. */
static int charger_vk_apple(void)
{
    /* 21/10 : ombres — Metal n'offre que 16 états d'échantillonneur par étage en liaison directe, et le layout en déclare
     * 34 (16 D3D9 + 2 + 16 de comparaison) : les argument buffers de MoltenVK lèvent la limite. À poser AVANT le chargement
     * de MoltenVK (il lit sa configuration à l'ouverture) ; les descripteurs poussés, eux, restent en liaison directe :
     * coupés à la création du device. */
    if (d3dv_ombres() && !getenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS")) {
        setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 1);
        LOG("[OMBRES] MoltenVK : argument buffers activés (MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1), descripteurs classiques");
    }
    const char *e = getenv("SC2_VK_LIB");
    if (e && *e) {
        V.lib = dlopen(e, RTLD_NOW | RTLD_LOCAL);
        if (!V.lib) { LOG("[VK] backend inactif : SC2_VK_LIB=%s introuvable (%s)", e, dlerror()); return 0; }
        return 1;
    }
    char exe[PATH_MAX], voisin[PATH_MAX + 32] = ""; uint32_t t = sizeof exe;
    if (_NSGetExecutablePath(exe, &t) == 0) snprintf(voisin, sizeof voisin, "%s/libMoltenVK.dylib", dirname(exe));
    const char *essais[] = { voisin, "libvulkan.1.dylib", "/opt/homebrew/lib/libMoltenVK.dylib", "libMoltenVK.dylib" };
    for (unsigned i = 0; i < sizeof essais / sizeof essais[0] && !V.lib; i++) {
        if (!*essais[i]) continue;
        V.lib = dlopen(essais[i], RTLD_NOW | RTLD_LOCAL);
        if (!V.lib) LOG("[VK] essai %u : %s indisponible (%s)", i + 2, essais[i], dlerror());
    }
    if (!V.lib) { LOG("[VK] backend inactif : ni MoltenVK ni loader Vulkan trouvé (SC2_VK_LIB=<chemin> pour forcer)"); return 0; }
    return 1;
}
#endif

static int init_interne(void)
{
    const char *e = getenv("SC2_VULKAN");
    if (e && *e == '0') { LOG("[VK] SC2_VULKAN=0 : backend désactivé, device D3D9 factice seul"); return 0; }
#ifdef __APPLE__
    if (!charger_vk_apple()) return 0;
#else
    V.lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!V.lib) { LOG("[VK] backend inactif : libvulkan.so.1 introuvable (%s)", dlerror()); return 0; }
#endif
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(V.lib, "vkGetInstanceProcAddr");
    if (!gipa) return echec("vkGetInstanceProcAddr absent", VK_ERROR_INITIALIZATION_FAILED);
#ifdef __APPLE__
    {   /* chemin RÉEL de la bibliothèque qui fournit gipa (un nom court résolu par dyld, un lien Homebrew…) */
        Dl_info di = { 0 };
        const char *reel = dladdr((const void *)gipa, &di) && di.dli_fname ? di.dli_fname : "?";
        V.loader = strstr(reel, "libvulkan") != NULL;
        LOG("[VK] bibliothèque : %s (%s)", reel, V.loader ? "loader Khronos" : "MoltenVK direct, sans loader");
    }
#endif
#define CHG_G(f) f = (PFN_##f)gipa(NULL, #f);
    VK_GLOBAL(CHG_G)
    uint32_t ver = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&ver);
    if (ver < VK_API_VERSION_1_3) return echec("loader Vulkan < 1.3 (rendu dynamique requis)", VK_ERROR_INCOMPATIBLE_DRIVER);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "recomp-sc2",
                              .pEngineName = "sc2-partout d3d9v", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
#ifdef __APPLE__
    /* Passe 7u : par le loader (1.3.216+), MoltenVK n'est énuméré qu'avec VK_KHR_portability_enumeration +
     * ENUMERATE_PORTABILITY ; sans eux, VK_ERROR_INCOMPATIBLE_DRIVER. Inutile en chargement direct. */
    const char *iexts[1] = { VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME };
    if (V.loader) {
        PFN_vkEnumerateInstanceExtensionProperties eie =
            (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
        uint32_t nie = 0; VkExtensionProperties ie[128]; int porte = 0;
        if (eie) { eie(NULL, &nie, NULL); if (nie > 128) nie = 128; eie(NULL, &nie, ie); }
        for (uint32_t i = 0; i < nie; i++) if (!strcmp(ie[i].extensionName, iexts[0])) porte = 1;
        if (porte) {
            ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = iexts;
            ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        LOG("[VK] loader : %s", porte ? "VK_KHR_portability_enumeration activée (ENUMERATE_PORTABILITY)"
                                      : "VK_KHR_portability_enumeration ABSENTE (loader < 1.3.216 ?)");
    }
#endif
    VkResult r = vkCreateInstance(&ici, NULL, &V.inst);
    if (r) return echec("vkCreateInstance", r);
#define CHG_I(f) f = (PFN_##f)gipa(V.inst, #f);
    VK_INSTANCE(CHG_I)

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(V.inst, &n, NULL);
    if (!n) return echec("aucun VkPhysicalDevice (mesa-vulkan-drivers / Lavapipe installé ?)", VK_ERROR_INITIALIZATION_FAILED);
    VkPhysicalDevice pds[8]; if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(V.inst, &n, pds);
    VkPhysicalDeviceProperties pp = { 0 };
    for (uint32_t i = 0; i < n && !V.pd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.apiVersion >= VK_API_VERSION_1_3) V.pd = pds[i];
    }
    if (!V.pd) return echec("aucun device Vulkan 1.3", VK_ERROR_INCOMPATIBLE_DRIVER);
#ifndef __APPLE__
    if (d3dv_ombres() && pp.limits.maxPerStageDescriptorSamplers < 34) {   /* 21/10 : voir d3dv_ombres */
        V.ombres_refus = 1;
        LOG("[OMBRES] désactivées : %u échantillonneurs par étage sur ce GPU (34 requis par le layout des ombres)",
            pp.limits.maxPerStageDescriptorSamplers);
    }
#endif
    snprintf(V.nom, sizeof V.nom, "%s (Vulkan %u.%u.%u, type %d)", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
             VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion), (int)pp.deviceType);
    vkGetPhysicalDeviceMemoryProperties(V.pd, &V.memp);

    uint32_t nq = 0; VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, NULL); if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, qp);
    V.qf = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { V.qf = i; break; }
    if (V.qf == UINT32_MAX) return echec("aucune file graphique", VK_ERROR_INITIALIZATION_FAILED);

    float prio = 1.f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qf,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .dynamicRendering = VK_TRUE };
    /* passe 5d : descripteurs poussés (draws réels) et compression BC (textures DXTn) */
#ifdef __APPLE__
    const char *exts[2];
#else
    const char *exts[1];
#endif
    uint32_t next = 0, ndx = 0;
    VkExtensionProperties dx[256];
    vkEnumerateDeviceExtensionProperties(V.pd, NULL, &ndx, NULL); if (ndx > 256) ndx = 256;
    vkEnumerateDeviceExtensionProperties(V.pd, NULL, &ndx, dx);
    /* passe 7c : SC2_VK_SANS_PUSH_DESCRIPTOR=1 → extension NON demandée (même device qu'un GPU qui ne l'a
     * pas : émulateur Android M4/SwiftShader) ; les draws réels passent par les descripteurs classiques. */
    const char *sp = getenv("SC2_VK_SANS_PUSH_DESCRIPTOR");
    V.sans_push = sp && *sp == '1';
#ifdef __APPLE__
    if (d3dv_ombres()) V.sans_push = 1;     /* 21/10 : ombres sous MoltenVK = argument buffers, pas de descripteurs poussés */
#endif
    for (uint32_t i = 0; i < ndx && !V.sans_push; i++)
        if (!strcmp(dx[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) exts[next++] = dx[i].extensionName;
    VkPhysicalDeviceFeatures fd = { 0 }, fdon = { 0 };
    vkGetPhysicalDeviceFeatures(V.pd, &fd);
    /* passe 7l : SC2_VK_SANS_BC=1 → compression BC NON demandée, même sur un device qui l'a : c'est le cas
     * d'un GPU sans BC (Adreno, émulateur) ; les DXTn sont alors décodés par le CPU (RGBA8). */
    const char *sb = getenv("SC2_VK_SANS_BC");
    V.bc_device = fd.textureCompressionBC; V.sans_bc = sb && *sb == '1';
    V.bc = V.bc_device && !V.sans_bc;
    fdon.textureCompressionBC = (VkBool32)V.bc;
    fdon.samplerAnisotropy = fd.samplerAnisotropy;
    V.aniso_max = fd.samplerAnisotropy ? pp.limits.maxSamplerAnisotropy : 0.f;
    V.biais_max = pp.limits.maxSamplerLodBias;
#ifdef __APPLE__
    /* Passe 7u : un device qui expose VK_KHR_portability_subset (MoltenVK) DOIT l'activer (spec Vulkan) ; ses
     * limites sont journalisées. samplerMipLodBias=0 → biais LOD interdit (mipLodBias doit valoir 0). */
    uint32_t npush = next;
    VkPhysicalDevicePortabilitySubsetFeaturesKHR ps = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR };
    V.fans = 1;
    for (uint32_t i = 0; i < ndx; i++)
        if (!strcmp(dx[i].extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) { exts[next++] = dx[i].extensionName; V.subset = 1; }
    if (V.subset) {
        PFN_vkGetPhysicalDeviceFeatures2 gpf2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(V.inst, "vkGetPhysicalDeviceFeatures2");
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ps };
        if (gpf2) gpf2(V.pd, &f2);
        V.fans = ps.triangleFans;
        if (!ps.samplerMipLodBias) V.biais_max = 0.f;
        LOG("[VK] portability_subset : triangleFans=%d mutableComparisonSamplers=%d samplerMipLodBias=%d "
            "imageViewFormatSwizzle=%d imageViewFormatReinterpretation=%d imageView2DOn3DImage=%d "
            "constantAlphaColorBlendFactors=%d separateStencilMaskRef=%d vertexAttributeAccessBeyondStride=%d pointPolygons=%d events=%d",
            ps.triangleFans, ps.mutableComparisonSamplers, ps.samplerMipLodBias, ps.imageViewFormatSwizzle,
            ps.imageViewFormatReinterpretation, ps.imageView2DOn3DImage, ps.constantAlphaColorBlendFactors,
            ps.separateStencilMaskRef, ps.vertexAttributeAccessBeyondStride, ps.pointPolygons, ps.events);
        ps.pNext = &f13;                     /* fonctionnalités offertes = fonctionnalités activées */
    } else LOG("[VK] portability_subset : absent du device (implémentation conforme complète)");
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = V.subset ? (void *)&ps : (void *)&f13,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = next, .ppEnabledExtensionNames = exts, .pEnabledFeatures = &fdon };
#else
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = next, .ppEnabledExtensionNames = exts, .pEnabledFeatures = &fdon };
#endif
    r = vkCreateDevice(V.pd, &dci, NULL, &V.dev);
    if (r) return echec("vkCreateDevice", r);
#define CHG_D(f) f = (PFN_##f)vkGetDeviceProcAddr(V.dev, #f);
    VK_DEVICE(CHG_D)
    vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
#ifdef __APPLE__
    if (npush) V.push_desc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(V.dev, "vkCmdPushDescriptorSetKHR");
#else
    if (next) V.push_desc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(V.dev, "vkCmdPushDescriptorSetKHR");
#endif

    /* passe 10s (cache de pipelines) : chargé avant toute création de pipeline —
     * pipeline_pour et pipeline5 passeront PC.cache à vkCreateGraphicsPipelines */
    pcache_charger();

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf };
    if ((r = vkCreateCommandPool(V.dev, &cpi, NULL, &V.pool))) return echec("vkCreateCommandPool", r);
    /* passe 10r (v13) : un emplacement par frame en vol — tampon, fence SIGNALÉE (un emplacement
     * jamais soumis n'attend pas), sémaphore « rendu » pour la présentation, moitié d'anneau */
    V.n_empl = mode_synchrone() ? 1 : EN_VOL;
    VkCommandBuffer cbs[EN_VOL];
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = V.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = (uint32_t)V.n_empl };
    if ((r = vkAllocateCommandBuffers(V.dev, &cai, cbs))) return echec("vkAllocateCommandBuffers", r);
    for (int i = 0; i < V.n_empl; i++) {
        V.empl[i].cb = cbs[i];
        V.empl[i].base = (VkDeviceSize)i * ANNEAU_TAILLE;
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        if ((r = vkCreateFence(V.dev, &fi, NULL, &V.empl[i].fence))) return echec("vkCreateFence", r);
        V.empl[i].en_cours = 1;              /* fence signalée d'office : « déjà terminée », l'attente initiale est vide */
        if (mode_synchrone()) continue;
        VkSemaphoreCreateInfo sei = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if ((r = vkCreateSemaphore(V.dev, &sei, NULL, &V.empl[i].sem_rendu))) return echec("vkCreateSemaphore", r);
    }

    /* passe 10s (chrono GPU) : SC2_VK_CHRONO_GPU — un pool de timestamps par emplacement ;
     * inactif sans la variable, ou si la file n'a pas de timestamps fiables */
    CH.period = chrono_gpu_period();
    if (CH.period) {
        CH.bits = qp[V.qf].timestampValidBits;
        CH.tick = (double)pp.limits.timestampPeriod;
        if (!CH.bits || !(CH.tick > 0.0))
            LOG("[VK] passe 10s (chrono GPU) : inactif — timestamps non fiables sur cette file "
                "(timestampValidBits %u, timestampPeriod %g)", CH.bits, pp.limits.timestampPeriod);
        else {
            VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                          .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = CHRONO_MAX };
            int ok = 1;
            for (int i = 0; i < V.n_empl; i++) if (vkCreateQueryPool(V.dev, &qpi, NULL, &CH.pool[i])) ok = 0;
            if (!ok) LOG("[VK] passe 10s (chrono GPU) : inactif — vkCreateQueryPool en échec");
            else {
                CH.actif = 1;
                LOG("[VK] passe 10s (chrono GPU) : ACTIF — %u requêtes par tampon, %u bits valides, "
                    "%.3f ns/tick, compte rendu toutes les %u frames", (unsigned)CHRONO_MAX,
                    CH.bits, CH.tick, CH.period);
            }
        }
    }

    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof spv_lumiere_vert, .pCode = spv_lumiere_vert };
    if ((r = vkCreateShaderModule(V.dev, &smi, NULL, &V.vs))) return echec("vkCreateShaderModule (vertex)", r);
    smi.codeSize = sizeof spv_lumiere_frag; smi.pCode = spv_lumiere_frag;
    if ((r = vkCreateShaderModule(V.dev, &smi, NULL, &V.fs))) return echec("vkCreateShaderModule (fragment)", r);
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    if ((r = vkCreatePipelineLayout(V.dev, &pli, NULL, &V.layout))) return echec("vkCreatePipelineLayout", r);

    LOG("[VK] backend Vulkan headless ACTIF : %s, file graphique %u, rendu dynamique, BC %s", V.nom, V.qf,
        V.bc ? "natif (VK_FORMAT_BC1..3)" : V.sans_bc ? "non (SC2_VK_SANS_BC=1) : DXTn décodés par le CPU"
             : "absent du device : DXTn décodés par le CPU");
    LOG("[VK] samplers : anisotropie %s (max %.0f), biais LOD max %.1f", V.aniso_max > 1.f ? "oui" : "non", V.aniso_max, V.biais_max);
    LOG("[VK] descripteurs : %s", V.push_desc ? "poussés (extension VK_KHR_push_descriptor)"
        : V.sans_push ? "classiques (fallback forcé : SC2_VK_SANS_PUSH_DESCRIPTOR=1)"
        : "classiques (fallback : VK_KHR_push_descriptor absent du device)");
    LOG("[VK] passe 10r (v13) : %s", mode_synchrone()
        ? "mode SYNCHRONE (SC2_VK_SYNCHRONE=1) : une soumission + vkQueueWaitIdle par frame, comme avant"
        : "DEUX FRAMES EN VOL : un emplacement par frame (tampon, fence, moitié d'anneau), CPU et GPU en parallèle");
    return 1;
}

int d3dv_init(void)
{
    pthread_mutex_lock(&g_mu);
    if (!V.essaye) { V.essaye = 1; V.actif = init_interne(); }
    int a = V.actif;
    pthread_mutex_unlock(&g_mu);
    return a;
}
int d3dv_actif(void) { return V.actif; }
const char *d3dv_nom_device(void) { return V.nom; }

/* ---- commandes ---------------------------------------------------------------- */
static void barriere(void)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(CB, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         1, &mb, 0, NULL, 0, NULL);
}

static void cb_ouvrir(void)
{
    if (V.cb_ouvert) return;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(CB, &bi);
    V.cb_ouvert = 1;
    if (CH.actif) {   /* passe 10s (chrono GPU) : pool de l'emplacement remis à zéro en tête de tampon, premier segment */
        vkCmdResetQueryPool(CB, CH.pool[V.cur], 0, CHRONO_MAX);
        CH.nq[V.cur] = 0;
        CH.plein[V.cur] = 0;
        chrono_segment(CH_HORS_PASSE, -1);
    }
    /* passe 10r (v13) : ordre ENTRE soumissions — une file exécute ses soumissions dans l'ordre,
     * mais la mémoire ne suit pas : ce que la frame N (autre emplacement) a écrit doit être
     * visible des lectures et effacements de la N+1, dès la première commande du tampon. */
    barriere();
}

static void rendu_fermer(void)
{
    if (V.rendu < 0) return;
    vkCmdEndRendering(CB);
    V.rendu = -1; V.rendu_ds = -1; V.rendu_st = 0;
    chrono_segment(CH_HORS_PASSE, -1);   /* passe 10s : ce qui suit la fin d'une passe (barrière comprise) */
    barriere();
}

/* Passe 7n : ds ≥ 0 → la surface de profondeur est attachée (aspect profondeur seul) si elle couvre
 * la cible couleur (D3D9 exige DS ≥ RT ; Vulkan, attachement ≥ zone de rendu). */
static int ds_attachable(int id, int ds)
{
    return ds >= 0 && ds < V.nc && V.c[ds].prof && V.c[ds].w >= V.c[id].w && V.c[ds].h >= V.c[id].h;
}
static void rendu_ouvrir_st(int id, int ds, int st);
static void rendu_ouvrir(int id, int ds) { rendu_ouvrir_st(id, ds, 0); }
/* 21/10 : st = attacher aussi le stencil (seulement pour un dessin qui le teste : sur un GPU à tuiles, l'attacher à
 * TOUTES les passes de profondeur coûtait ≈ 25 % d'images au banc). Un rendu déjà ouvert AVEC stencil sert aussi les
 * dessins sans stencil (leurs pipelines déclarent alors le format stencil : clé k.sa). */
static void rendu_ouvrir_st(int id, int ds, int st)
{
    if (!ds_attachable(id, ds)) ds = -1;
    st = st && ds >= 0 && (V.c[ds].aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (V.rendu == id && V.rendu_ds == ds && (V.rendu_st == st || V.rendu_st)) return;
    cb_ouvrir();
    rendu_fermer();
    cible *t = &V.c[id];
    VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t->vue,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    /* 21/10 : stencil D3D9 — avec stencil, Vulkan exige la MÊME vue (deux aspects) pour les attachements de
     * profondeur et de stencil d'un format combiné ; sans, la vue profondeur seule, comme avant.
     * Attachement stencil ⇔ V.rendu_st ⇔ k.sa ⇔ stencilAttachmentFormat (pipeline5). */
    VkRenderingAttachmentInfo prof = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                       .imageView = ds >= 0 ? (st ? V.c[ds].vue : V.c[ds].vue_prof) : VK_NULL_HANDLE,
                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                       .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingAttachmentInfo ste = prof;   /* même vue, mêmes opérations : LOAD/STORE des deux aspects */
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { t->w, t->h } },
                           .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att,
                           .pDepthAttachment = ds >= 0 ? &prof : NULL, .pStencilAttachment = st ? &ste : NULL };
    chrono_segment(CH_PASSE, id);        /* passe 10s : ouvre le segment de la passe qui commence */
    vkCmdBeginRendering(CB, &ri);
    V.rendu = id; V.rendu_ds = ds; V.rendu_st = st;
}

/* ---- passe 10r (v13) : deux frames en vol — soumission, fences, libérations ----------
 * Avant (et sous SC2_VK_SYNCHRONE=1) : une soumission suivie de vkQueueWaitIdle — le CPU
 * n'enregistrait la frame N+1 qu'une fois la N exécutée (série). Ici chaque emplacement
 * porte son tampon de commandes, sa fence, sa moitié d'anneau, ses libérations différées et
 * son pool de descripteurs ; soumettre() signale la fence de l'emplacement courant puis
 * attend celle de l'emplacement SUIVANT — sa soumission d'avant : le GPU exécute la frame N
 * pendant que le CPU enregistre la N+1. Une file exécute ses soumissions dans l'ordre ; la
 * mémoire, elle, n'est ordonnée entre soumissions que par la barrière de cb_ouvrir(). */
static int g_dans_present;                   /* d3dv_present en cours : sa soumission peut signer « rendu » */
static int g_sem_rendu_demande;              /* la prochaine soumission de present signale sem_rendu (une fois) */
static VkSemaphore g_sem_rendu;              /* sémaphore signalé par la dernière présentation demandée */
static VkSemaphore g_sem_attendre;           /* à attendre à la prochaine soumission, puis oublié (une fois) */
static uint32_t g_sem_attendre_etape;        /* VkPipelineStageFlags de cette attente */

/* motif d'un attendre_tout — pour la ligne [BILAN] [VOL] */
#define AT_RELECTURE  0
#define AT_DESTRUCTION 1
#define AT_LISTE       2

static void empl_attendre(int i)   /* attends la fence d'un emplacement en vol (cumul pour le bilan) */
{
    double t0 = maintenant_ms();
    uint64_t ta = &sc2_att_debut ? sc2_att_debut() : 0;   /* SC2_ATTENTES : temps bloqué, cause fence */
    vkWaitForFences(V.dev, 1, &V.empl[i].fence, VK_TRUE, UINT64_MAX);
    if (ta) sc2_att_fin(ATT_VK_FENCE, 0, ta, 0);
    V.ms_attentes += maintenant_ms() - t0;
    V.n_attentes_fence++;
    V.empl[i].en_cours = 0;
    chrono_relever(i);                   /* passe 10s : fence passée → ses timestamps sont lisibles */
}
static void empl_liberer(int i)    /* détruit ses objets différés — SA fence vient d'être attendue */
{
    for (uint32_t k = 0; k < V.empl[i].n_lib; k++) {
        if (V.empl[i].lib[k].img) {        /* V5.1 : image d'une texture oubliée */
            if (V.empl[i].lib[k].vue) vkDestroyImageView(V.dev, V.empl[i].lib[k].vue, NULL);
            if (V.empl[i].lib[k].vue2) vkDestroyImageView(V.dev, V.empl[i].lib[k].vue2, NULL);
            vkDestroyImage(V.dev, V.empl[i].lib[k].img, NULL);
            if (V.empl[i].lib[k].m) vkFreeMemory(V.dev, V.empl[i].lib[k].m, NULL);
        } else if (V.empl[i].lib[k].dalle >= 0)   /* 21/10 : une PLAGE rend sa place, le VkBuffer reste à sa dalle */
            dalle_rendre(V.empl[i].lib[k].dalle, V.empl[i].lib[k].off, V.empl[i].lib[k].taille);
        else { vkDestroyBuffer(V.dev, V.empl[i].lib[k].b, NULL); vkFreeMemory(V.dev, V.empl[i].lib[k].m, NULL); }
    }
    V.empl[i].n_lib = 0;
}
static void empl_pool_raz(int i)   /* remise à zéro du pool de descripteurs classiques de l'emplacement */
{
    if (V.empl[i].pool && V.empl[i].pool_en_cours) {
        vkResetDescriptorPool(V.dev, V.empl[i].pool, 0);
        R.n_sets_liberes += V.empl[i].pool_en_cours; R.n_pool_raz++; V.empl[i].pool_en_cours = 0;
    }
}

static int soumettre(void)
{
    int vide = 0;   /* passe 10r (v13) : frame vide + sémaphore « rendu » demandé → soumission VIDE
                     * (commandBufferCount = 0) qui le signale : un sémaphore signalé doit être attendu */
    if (!V.cb_ouvert) {
        if (mode_synchrone() || !g_dans_present || !g_sem_rendu_demande) return 0;
        vide = 1;
    } else {
        rendu_fermer();
        chrono_segment(CH_HORS_PASSE, -1);   /* passe 10s : dernier timestamp, ferme le dernier segment */
        vkEndCommandBuffer(CB);
        V.cb_ouvert = 0;
    }
    VkSemaphore satt = VK_NULL_HANDLE, ssig = VK_NULL_HANDLE;
    VkPipelineStageFlags etape = 0;
    if (!mode_synchrone()) {
        if (g_sem_attendre) {                /* présentation : CETTE soumission attend son sémaphore */
            satt = g_sem_attendre; etape = g_sem_attendre_etape; g_sem_attendre = VK_NULL_HANDLE;
        }
        if (g_dans_present && g_sem_rendu_demande) {   /* … et signale celui du rendu de SON emplacement */
            ssig = V.empl[V.cur].sem_rendu;
            g_sem_rendu_demande = 0;
        }
    }
    VkCommandBuffer cb = CB;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = vide ? 0 : 1,
                        .pCommandBuffers = &cb };
    if (satt) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &satt; si.pWaitDstStageMask = &etape; }
    if (ssig) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &ssig; }
    if (!mode_synchrone())
        vkResetFences(V.dev, 1, &V.empl[V.cur].fence);   /* jamais signalée en la soumettant :
                                                          * créée signalée, et une fence attendue
                                                          * puis rendue ne doit pas être resoumise */
    VkResult r = vkQueueSubmit(V.q, 1, &si, mode_synchrone() ? VK_NULL_HANDLE : V.empl[V.cur].fence);
    if (!r && !vide && CH.actif) CH.attente[V.cur] = CH.nq[V.cur];   /* passe 10s : à relever une fois sa fence passée */
    if (!r && mode_synchrone()) r = vkQueueWaitIdle(V.q);   /* comportement d'avant : GPU au repos */
    V.n_soumis++;
    if (vide) V.n_soumis_vides++;
    if (r) LOG("[VK] soumission en échec (VkResult %d)", (int)r);
    if (mode_synchrone()) {
        if (!r) chrono_relever(0);       /* passe 10s : GPU au repos — timestamps lisibles */
        R.pos = 0;                           /* GPU au repos : l'anneau de la frame est libre */
        R.epoque++;                          /* passe 10r : les emplacements réservés avant ne valent plus */
        empl_pool_raz(0);                    /* … et les sets de descripteurs classiques aussi */
        empl_liberer(0);                     /* (même en échec, comme avant : rien de parti n'est référencé) */
    } else if (!r) {                         /* en vol : la fence de CET emplacement se signalera */
        V.empl[V.cur].en_cours = 1;
        if (ssig) g_sem_rendu = ssig;        /* d3dv_semaphore_rendu() le rendra, puis l'oubliera */
        V.cur = (V.cur + 1) % V.n_empl;      /* emplacement suivant : au tour de SA soumission précédente */
        if (V.empl[V.cur].en_cours) empl_attendre(V.cur);
        vkResetFences(V.dev, 1, &V.empl[V.cur].fence);
        empl_liberer(V.cur);                 /* sa fence attendue couvre aussi la soumission d'avant */
        empl_pool_raz(V.cur);
        R.pos = V.empl[V.cur].base;          /* sa moitié d'anneau est libre */
        R.epoque++;
    }                                        /* échec en vol : l'emplacement reste courant, rien n'est parti */
    return r ? -1 : 0;
}

/* passe 10r (v13) : soumet le tampon ouvert, puis attend les fences de TOUS les emplacements —
 * en retour le GPU est au repos. À appeler avant toute relecture CPU (relire, d3dv_texture_relire)
 * et avant toute destruction ou réallocation d'image, de vue, de mémoire ou de tampon que le GPU
 * peut encore lire ; seule exception : ce qui passe par les libérations différées, qui attendent
 * leur emplacement. Rend 0, -1 si une soumission ou une attente échoue. */
static int attendre_tout(int motif)
{
    /* passe 10r (v13) : une attente armée par la présentation (son blit, sur SA file, lit encore le back buffer)
     * part avec la soumission — sans tampon ouvert, soumettre() ne soumettrait rien et nos fences ne couvriraient
     * pas le blit : on en ouvre un (barrière seule) pour qu'une destruction qui suit attende aussi le blit */
    if (!mode_synchrone() && g_sem_attendre && !V.cb_ouvert) cb_ouvrir();
    int r = soumettre();
    if (motif == AT_RELECTURE) V.n_at_relecture++;
    else if (motif == AT_DESTRUCTION) V.n_at_destruction++;
    else V.n_at_liste++;
    if (mode_synchrone()) {
        int w = vkQueueWaitIdle(V.q);
        if (!w) chrono_relever(0);           /* passe 10s : GPU au repos */
        return w ? -1 : r;
    }
    VkFence fs[EN_VOL]; uint32_t n = 0;
    for (int i = 0; i < V.n_empl; i++) if (V.empl[i].en_cours) fs[n++] = V.empl[i].fence;
    if (n) {
        double t0 = maintenant_ms();
        uint64_t ta = &sc2_att_debut ? sc2_att_debut() : 0;   /* SC2_ATTENTES : temps bloqué, cause fence */
        VkResult w = vkWaitForFences(V.dev, n, fs, VK_TRUE, UINT64_MAX);
        if (ta) sc2_att_fin(ATT_VK_FENCE, 0, ta, 0);
        V.ms_attentes += maintenant_ms() - t0; V.n_attentes_fence++;
        for (int i = 0; i < V.n_empl; i++) V.empl[i].en_cours = 0;
        if (w) { LOG("[VK] attente de fence en échec (VkResult %d)", (int)w); return -1; }
    }
    for (int i = 0; i < V.n_empl; i++) { chrono_relever(i); empl_liberer(i); empl_pool_raz(i); }   /* passe 10s en tête : fences passées */
    return r;
}

/* passe 10r : libération différée d'un VkBuffer remplacé — des draws d'une soumission EN VOL
 * peuvent encore le lire à l'exécution. Il part dans la liste de l'emplacement EN COURS
 * D'ENREGISTREMENT (ses propres draws, plus tôt dans la frame, peuvent le lier) : il meurt
 * quand cet emplacement revient, après sa fence. Liste pleine : tout attendre, tout vider. */
static void liberer_differer(VkBuffer b, VkDeviceMemory m)   /* sous g_mu */
{
    if (V.empl[V.cur].n_lib >= A_LIBERER) attendre_tout(AT_LISTE);
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].b = b;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].m = m;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].dalle = -1;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].img = VK_NULL_HANDLE;   /* V5.1 */
    V.empl[V.cur].n_lib++;
}

/* 21/10 : libération différée d'une PLAGE de dalle (le tampon sous-alloué disparaît, sa place
 * redevient libre une fois la fence passée — même garde et même emplacement que ci-dessus). */
static void liberer_plage_differer(int dalle, uint32_t off, uint32_t taille)   /* sous g_mu */
{
    if (V.empl[V.cur].n_lib >= A_LIBERER) attendre_tout(AT_LISTE);
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].b = VK_NULL_HANDLE;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].m = VK_NULL_HANDLE;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].dalle = dalle;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].off = off;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].taille = taille;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].img = VK_NULL_HANDLE;   /* V5.1 */
    V.empl[V.cur].n_lib++;
}
/* V5.1 : libération différée d'une IMAGE de texture oubliée (vue, image, mémoire) — même garde, même emplacement */
static void liberer_image_differer(VkImage img, VkImageView vue, VkImageView vue2, VkDeviceMemory m)   /* sous g_mu */
{
    if (V.empl[V.cur].n_lib >= A_LIBERER) attendre_tout(AT_LISTE);
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].b = VK_NULL_HANDLE;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].m = m;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].dalle = -1;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].img = img;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].vue = vue;
    V.empl[V.cur].lib[V.empl[V.cur].n_lib].vue2 = vue2 != vue ? vue2 : VK_NULL_HANDLE;
    V.empl[V.cur].n_lib++;
}

/* ---- V5.1 (24/10) : oubli des objets invités rendus à l'arène ----------------------------------------------------
 * sc2_d3d9.c rend à l'arène une texture (2D, cube, volume) ou un tampon de sommets/index à son dernier Release (hors du
 * device : plus aucune liaison) — son adresse invitée, CLÉ de R.t et de R.tam, sera resservie à un autre objet. Avant,
 * ses entrées sont oubliées ici : image + vue + mémoire, ou VkBuffer / plage de dalle, partent en libération DIFFÉRÉE
 * dans l'emplacement en cours d'enregistrement (détruites quand il revient, après sa fence : une soumission en vol, ou
 * un draw plus tôt dans la frame, peut encore les lire) ; l'entrée prend la clé morte, sa case de hachage une pierre
 * tombale (-2), son indice repart en service. Sans cet oubli, un objet neuf à la même adresse et de même génération
 * (0, puis +2 par Lock/Unlock) recevait l'IMAGE de l'ancien. Les cibles de rendu (V.c) ne sont pas oubliées :
 * d3dv_cible recrée l'image d'une clé reprise dès que taille, format ou profondeur diffèrent (contenu indéfini, comme
 * une cible D3D9 neuve). L'époque avance : aucune plage volatile de l'anneau n'est reprise pour un objet neuf. */
static void hache_refaire(int32_t *ht, uint32_t taille, int tex)   /* sous g_mu : pierres tombales trop nombreuses */
{
    memset(ht, 0xFF, taille * sizeof *ht);
    if (tex == 1) { g_tex_tombes = 0; for (int i = 0; i < R.nt; i++) if (R.t[i].cle != CLE_MORTE) tex_noter(i); }
    else if (tex == 2) { g_cib_tombes = 0; for (int i = 0; i < V.nc; i++) if (V.c[i].cle != CLE_MORTE) cible_noter(i); }
    else { g_tam_tombes = 0; for (int i = 0; i < R.ntam; i++) if (R.tam[i].cle != (uint32_t)CLE_MORTE) tam_noter(i); }
    g_n_hache_refaites++;
}
static void cible_retirer(int id)                    /* sous g_mu : pierre tombale à la case de V.c[id] */
{
    uint32_t h = hache_cle64(V.c[id].cle, HACHE_CIBLES_BITS);
    for (uint32_t n = 0; n < HACHE_CIBLES_TAILLE; n++) {
        if (g_hache_cibles[h] == id) { g_hache_cibles[h] = -2; g_cib_tombes++; return; }
        if (g_hache_cibles[h] == -1) return;
        h = (h + 1) & HACHE_CIBLES_MASQUE;
    }
}
/* V5.1 (revue adverse, MAJEUR 1) : cibles de rendu d'un objet rendu à l'arène — clé = son adresse (surface autonome)
 * ou adresse << 32 | taille (niveaux d'une texture RENDERTARGET / DEPTHSTENCIL, cf. cible_de et dessin_reel). Sans
 * cela, ≈ 73 cibles mortes de plus par mission (446 en fin d'acte 1, ≈ 1 Go d'images) et, à MAX_CIBLES, d3dv_cible
 * rendait −1 : dessins refusés. Une cible en usage (rendu ouvert, RT ou profondeur courantes) est gardée, comptée. */
static int cible_de_lot(uint64_t cle, const uint32_t *objs, int n)
{
    if (cle == CLE_MORTE) return 0;
    for (int i = 0; i < n; i++) if (cle == objs[i] || (cle >> 32) == objs[i]) return 1;
    return 0;
}
static void cibles_oublier(const uint32_t *objs, int n)   /* sous g_mu */
{
    for (int c = 0; c < V.nc; c++) {
        if (!cible_de_lot(V.c[c].cle, objs, n)) continue;
        if (c == V.rt || c == V.ds || c == V.rendu || c == V.rendu_ds) { g_n_cib_gardees++; continue; }
        cible *t = &V.c[c];
        if (t->img) liberer_image_differer(t->img, t->vue, t->vue_prof, t->mem);
        cible_retirer(c);
        memset(t, 0, sizeof *t); t->cle = CLE_MORTE;
        g_cib_libres[g_ncib_libres++] = c;
        g_n_cib_oubliees++;
    }
}
static void tam_retirer(int id)                      /* sous g_mu : pierre tombale à la case de R.tam[id] */
{
    uint32_t h = hache_cle64(R.tam[id].cle, HACHE_TAM_BITS);
    for (uint32_t n = 0; n < HACHE_TAM_TAILLE; n++) {
        if (g_hache_tam[h] == id) { g_hache_tam[h] = -2; g_tam_tombes++; return; }
        if (g_hache_tam[h] == -1) return;
        h = (h + 1) & HACHE_TAM_MASQUE;
    }
}
void d3dv_oublier(const uint32_t *objs, int n)
{
    if (!V.actif || n <= 0) return;
    pthread_mutex_lock(&g_mu);
    int vu = 0;
    for (int i = 0; i < n; i++) {
        int id = tex_trouver(objs[i]);
        if (id >= 0) {
            tex5 *t = &R.t[id];
            if (t->img) liberer_image_differer(t->img, t->vue, VK_NULL_HANDLE, t->mem);
            tex_retirer_(id, 1);                           /* pierre tombale : des chaînes peuvent la traverser */
            memset(t, 0, sizeof *t); t->cle = CLE_MORTE; t->gen = ~0u;
            g_tex_libres[g_ntex_libres++] = id;            /* R.nt ne recule pas : indice repris à la création */
            g_n_tex_oubliees++; vu = 1;
        }
        id = tam_trouver(objs[i]);
        if (id >= 0) {
            tam10 *t = &R.tam[id];
            if (t->buf) {                                  /* volatil : déjà rendu (buf nul, octets décomptés) */
                if (t->dalle >= 0) liberer_plage_differer(t->dalle, t->off, arrondi256(t->taille));
                else liberer_differer(t->buf, t->mem);
                R.o_tam -= t->taille;
            }
            tam_retirer(id);
            memset(t, 0, sizeof *t); t->cle = (uint32_t)CLE_MORTE; t->dalle = -1;
            g_tam_libres[g_ntam_libres++] = id;
            g_n_tam_oublies++; vu = 1;
        }
    }
    cibles_oublier(objs, n);                             /* V5.1 (revue) : cibles de rendu aussi */
    if (g_tex_tombes > (int)(HACHE_TEX_TAILLE / 4)) hache_refaire(g_hache_tex, HACHE_TEX_TAILLE, 1);
    if (g_cib_tombes > (int)(HACHE_CIBLES_TAILLE / 4)) hache_refaire(g_hache_cibles, HACHE_CIBLES_TAILLE, 2);
    if (g_tam_tombes > (int)(HACHE_TAM_TAILLE / 4)) hache_refaire(g_hache_tam, HACHE_TAM_TAILLE, 0);
    if (vu) R.epoque++;
    pthread_mutex_unlock(&g_mu);
}
void d3dv_oublis(uint64_t *tex, uint64_t *tam, int *tex_vivantes, int *tam_vivants, uint64_t *cibles, int *cibles_vivantes)
{   /* verrou ESSAYÉ : appelée par les lignes [MEM] de sc2_stop, peut-être sur une faute sous g_mu */
    int v = 0;
    for (int k = 0; k < 100 && !(v = !pthread_mutex_trylock(&g_mu)); k++) usleep(1000);
    if (tex) *tex = g_n_tex_oubliees;
    if (tam) *tam = g_n_tam_oublies;
    if (tex_vivantes) *tex_vivantes = R.nt - g_ntex_libres;
    if (tam_vivants) *tam_vivants = R.ntam - g_ntam_libres;
    if (cibles) *cibles = g_n_cib_oubliees;
    if (cibles_vivantes) *cibles_vivantes = V.nc - g_ncib_libres;
    if (v) pthread_mutex_unlock(&g_mu);
}

/* ---- cibles -------------------------------------------------------------------- */
static int fmt_profondeur(uint32_t f)
{
    switch (f) {
    case 70: case 71: case 73: case 75: case 77: case 79: case 80: case 82: case 83:   /* D16_LOCKABLE … D24FS8 */
    case 0x5A544E49u: case 0x34324644u: case 0x36314644u: case 0x50574152u:            /* INTZ, DF24, DF16, RAWZ */
        return 1;
    default: return 0;
    }
}
static VkFormat vk_couleur(uint32_t f)
{
    VkFormat v;
    switch (f) {
    case 32: v = VK_FORMAT_R8G8B8A8_UNORM; break;              /* A8B8G8R8 */
    case 34: v = VK_FORMAT_R16G16_UNORM; break;                /* G16R16 */
    case 36: v = VK_FORMAT_R16G16B16A16_UNORM; break;          /* A16B16G16R16 */
    case 111: v = VK_FORMAT_R16_SFLOAT; break;                 /* R16F */
    case 112: v = VK_FORMAT_R16G16_SFLOAT; break;              /* G16R16F */
    case 113: v = VK_FORMAT_R16G16B16A16_SFLOAT; break;        /* A16B16G16R16F */
    case 114: v = VK_FORMAT_R32_SFLOAT; break;                 /* R32F */
    case 115: v = VK_FORMAT_R32G32_SFLOAT; break;              /* G32R32F */
    case 116: v = VK_FORMAT_R32G32B32A32_SFLOAT; break;        /* A32B32G32R32F */
    default: v = VK_FORMAT_B8G8R8A8_UNORM;                     /* A8R8G8B8, X8R8G8B8 et le reste */
    }
    if (!format_ok(v, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) v = VK_FORMAT_B8G8R8A8_UNORM;
    return v;
}

static void cible_liberer(cible *t)
{
    if (t->vue) vkDestroyImageView(V.dev, t->vue, NULL);
    if (t->vue_prof && t->vue_prof != t->vue) vkDestroyImageView(V.dev, t->vue_prof, NULL);
    t->vue_prof = VK_NULL_HANDLE;
    if (t->img) vkDestroyImage(V.dev, t->img, NULL);
    if (t->mem) vkFreeMemory(V.dev, t->mem, NULL);
    t->vue = VK_NULL_HANDLE; t->img = VK_NULL_HANDLE; t->mem = VK_NULL_HANDLE;
}

static int cible_creer(cible *t)
{
    VkImageUsageFlags u;
    if (t->prof) {
        t->vf = format_ok(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
              ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
        t->aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (t->vf == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        u = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        /* passe 7s : profondeur échantillonnable (carte d'ombres du jeu : texture D16 usage DEPTHSTENCIL) si
         * SC2_OMBRES=1 et si le format le permet */
        if (d3dv_ombres() && format_ok(t->vf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    } else {
        t->vf = vk_couleur(t->fmt);
        t->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
          | VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = t->vf, .extent = { t->w, t->h, 1 }, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(V.dev, &ii, NULL, &t->img)) return -1;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V.dev, t->img, &mr);
    int32_t ty = type_memoire(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ty < 0) ty = type_memoire(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = (uint32_t)ty };
    if (ty < 0 || vkAllocateMemory(V.dev, &ai, NULL, &t->mem) || vkBindImageMemory(V.dev, t->img, t->mem, 0)) {
        cible_liberer(t); return -1;
    }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = t->vf,
                                 .subresourceRange = { t->aspect, 0, 1, 0, 1 } };
    if (vkCreateImageView(V.dev, &vi, NULL, &t->vue)) { cible_liberer(t); return -1; }
    t->vue_prof = t->vue;
    if (t->prof && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {      /* attachement de profondeur : aspect profondeur seul */
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (vkCreateImageView(V.dev, &vi, NULL, &t->vue_prof)) { cible_liberer(t); return -1; }
    }
    /* UNDEFINED → GENERAL, une fois pour toutes ; contenu initial : noir / profondeur 1 (passe 7n : écrit
     * explicitement — la transition depuis UNDEFINED ne garantit AUCUN contenu). */
    cb_ouvrir();
    rendu_fermer();
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = t->img, .subresourceRange = { t->aspect, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(CB, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkImageSubresourceRange sr = { t->aspect, 0, 1, 0, 1 };
    if (t->prof) {   /* passe 10s : l'effacement initial compte dans les clears de profondeur de la cible */
        VkClearDepthStencilValue z1 = { 1.f, 0 };
        chrono_segment(CH_CLEAR_PROF, (int)(t - V.c));
        vkCmdClearDepthStencilImage(CB, t->img, VK_IMAGE_LAYOUT_GENERAL, &z1, 1, &sr);
        chrono_segment(CH_HORS_PASSE, -1);
    }
    else { VkClearColorValue noir = { .float32 = { 0, 0, 0, 0 } }; vkCmdClearColorImage(CB, t->img, VK_IMAGE_LAYOUT_GENERAL, &noir, 1, &sr); }
    barriere();
    return 0;
}

/* 21/10 : ombres ACTIVES par défaut (SC2_OMBRES=0 pour les couper) — c'est alors le réglage « Ombres » du jeu qui
 * décide, comme sur PC ; refusées d'office sur un GPU qui n'a pas les 34 échantillonneurs par étage du layout
 * (V.ombres_refus, posé à l'initialisation). Passe 7v2/7v3 : variantes Dref des PS, binding 18 + k. */
int d3dv_ombres(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SC2_OMBRES"); v = !(e && *e == '0'); }
    return v && !V.ombres_refus;
}

int d3dv_cible(uint64_t cle, uint32_t w, uint32_t h, uint32_t fmt, int profondeur)
{
    if (!V.actif || !w || !h || w > 16384 || h > 16384) return -1;
    pthread_mutex_lock(&g_mu);
    int prof = profondeur || fmt_profondeur(fmt);
    int id = cible_trouver(cle);   /* passe 10r */
    if (id >= 0) {
        cible *t = &V.c[id];
        if (t->w != w || t->h != h || t->prof != prof || t->fmt != fmt) {   /* Reset, réallocation */
            attendre_tout(AT_DESTRUCTION);   /* passe 10r (v13) : une soumission en vol peut encore lier cette cible */
            cible_liberer(t);
            t->w = w; t->h = h; t->fmt = fmt; t->prof = prof;
            if (cible_creer(t)) id = -1;
        }
    } else if (g_ncib_libres || V.nc < MAX_CIBLES) {
        int k = g_ncib_libres ? g_cib_libres[--g_ncib_libres] : V.nc;   /* V5.1 : indice d'une cible oubliée */
        cible *t = &V.c[k];
        memset(t, 0, sizeof *t);
        t->cle = cle; t->w = w; t->h = h; t->fmt = fmt; t->prof = prof;
        if (cible_creer(t)) {                        /* échec : l'indice repris retourne à la liste */
            if (k < V.nc) { memset(t, 0, sizeof *t); t->cle = CLE_MORTE; g_cib_libres[g_ncib_libres++] = k; }
        } else {
            id = k; if (k == V.nc) V.nc++;
            cible_noter(id);
            if (V.nc <= 12)
                LOG("[VK] cible n°%d : %u×%u D3DFMT %u → VkFormat %d%s", id, w, h, fmt, (int)t->vf, prof ? " (profondeur)" : "");
        }
    }
    pthread_mutex_unlock(&g_mu);
    return id;
}

/* Passe 7w : StretchRect D3D9 entre deux cibles (surfaces de rendu, niveaux de textures RENDERTARGET, backbuffer) :
 * vkCmdBlitImage avec mise à l'échelle. rs/rd : RECT D3D9 (gauche, haut, droite, bas) ou NULL = surface entière. */
int d3dv_copier(int src, const int32_t *rs, int dst, const int32_t *rd, int lineaire)
{
    if (!V.actif || src < 0 || dst < 0 || src >= V.nc || dst >= V.nc || src == dst) return -1;
    pthread_mutex_lock(&g_mu);
    cible *a = &V.c[src], *b = &V.c[dst];
    int r = -1;
    if (a->prof == b->prof) {
        int32_t sr[4] = { 0, 0, (int32_t)a->w, (int32_t)a->h }, dr[4] = { 0, 0, (int32_t)b->w, (int32_t)b->h };
        if (rs) memcpy(sr, rs, sizeof sr);
        if (rd) memcpy(dr, rd, sizeof dr);
        if (sr[2] > (int32_t)a->w) sr[2] = (int32_t)a->w; if (sr[3] > (int32_t)a->h) sr[3] = (int32_t)a->h;
        if (dr[2] > (int32_t)b->w) dr[2] = (int32_t)b->w; if (dr[3] > (int32_t)b->h) dr[3] = (int32_t)b->h;
        if (sr[0] < sr[2] && sr[1] < sr[3] && dr[0] < dr[2] && dr[1] < dr[3]) {
            cb_ouvrir(); rendu_fermer();
            VkImageBlit bl = { .srcSubresource = { a->aspect, 0, 0, 1 }, .dstSubresource = { b->aspect, 0, 0, 1 } };
            bl.srcOffsets[0] = (VkOffset3D){ sr[0], sr[1], 0 }; bl.srcOffsets[1] = (VkOffset3D){ sr[2], sr[3], 1 };
            bl.dstOffsets[0] = (VkOffset3D){ dr[0], dr[1], 0 }; bl.dstOffsets[1] = (VkOffset3D){ dr[2], dr[3], 1 };
            chrono_segment(CH_COPIE, dst);   /* passe 10s : StretchRect, étiqueté de la cible destination */
            vkCmdBlitImage(CB, a->img, VK_IMAGE_LAYOUT_GENERAL, b->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                           lineaire && !a->prof ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            chrono_segment(CH_HORS_PASSE, -1);
            barriere();
            a->n_copie_src++; b->n_copie_dst++;
            r = 0;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}
/* Passe 7v2 : slots vus liés à une profondeur pour un module PS (bit k) — pour le traducteur / la recapture. */
uint32_t d3dv_profondeur_module(int ps) { return ps >= 0 && ps < MAX_MODULES ? R.prof_module[ps] : 0; }
int d3dv_rt_courante(void) { return V.actif ? V.rt : -1; }

void d3dv_rt(int id) { if (V.actif) V.rt = id >= 0 && id < V.nc && !V.c[id].prof ? id : -1; }
void d3dv_ds(int id) { if (V.actif) V.ds = id >= 0 && id < V.nc && V.c[id].prof ? id : -1; }
void d3dv_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float zmin, float zmax)
{
    V.vp[0] = x; V.vp[1] = y; V.vp[2] = w; V.vp[3] = h; V.zmin = zmin; V.zmax = zmax;
}

/* ---- Clear ------------------------------------------------------------------- */
static void noter_couleur(uint32_t col)
{
    for (int i = 0; i < V.ncouleurs; i++) if (V.couleurs[i].col == col) { V.couleurs[i].n++; return; }
    if (V.ncouleurs < 16) { V.couleurs[V.ncouleurs].col = col; V.couleurs[V.ncouleurs++].n = 1; }
}

void d3dv_clear(uint32_t n, const int32_t *rects, uint32_t flags, uint32_t couleur, float z, uint32_t stencil)
{
    if (!V.actif) return;
    pthread_mutex_lock(&g_mu);
    if ((flags & D3DV_CLEAR_TARGET) && V.rt >= 0) {
        cible *t = &V.c[V.rt];
        /* D3D9 : le Clear est borné au viewport, puis aux rectangles éventuels. */
        int64_t vx1 = V.vp[0], vy1 = V.vp[1], vx2 = (int64_t)V.vp[0] + V.vp[2], vy2 = (int64_t)V.vp[1] + V.vp[3];
        if (vx2 > t->w) vx2 = t->w;
        if (vy2 > t->h) vy2 = t->h;
        VkClearRect cr[64]; uint32_t ncr = 0;
        uint32_t total = rects && n ? n : 1;
        for (uint32_t i = 0; i < total && ncr < 64; i++) {
            int64_t x1 = vx1, y1 = vy1, x2 = vx2, y2 = vy2;
            if (rects && n) {
                const int32_t *r = rects + 4 * i;
                if (r[0] > x1) x1 = r[0];
                if (r[1] > y1) y1 = r[1];
                if (r[2] < x2) x2 = r[2];
                if (r[3] < y2) y2 = r[3];
            }
            if (x2 <= x1 || y2 <= y1) continue;
            cr[ncr++] = (VkClearRect){ { { (int32_t)x1, (int32_t)y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } }, 0, 1 };
        }
        if (ncr) {
            rendu_ouvrir(V.rt, -1);
            VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0 };
            ca.clearValue.color.float32[0] = (float)((couleur >> 16) & 0xFF) / 255.f;
            ca.clearValue.color.float32[1] = (float)((couleur >> 8) & 0xFF) / 255.f;
            ca.clearValue.color.float32[2] = (float)(couleur & 0xFF) / 255.f;
            ca.clearValue.color.float32[3] = (float)(couleur >> 24) / 255.f;
            vkCmdClearAttachments(CB, 1, &ca, ncr, cr);
            V.n_rects += ncr;
            t->n_clear++;
        }
        V.n_clear_c++;
        noter_couleur(couleur);
    }
    if ((flags & (D3DV_CLEAR_ZBUFFER | D3DV_CLEAR_STENCIL)) && V.ds >= 0) {
        cible *t = &V.c[V.ds];
        VkImageAspectFlags asp = 0;
        if (flags & D3DV_CLEAR_ZBUFFER) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & D3DV_CLEAR_STENCIL) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        asp &= t->aspect;
        if (asp) {
            cb_ouvrir();
            rendu_fermer();
            VkClearDepthStencilValue v = { z < 0.f ? 0.f : z > 1.f ? 1.f : z, stencil & 0xFF };
            VkImageSubresourceRange sr = { asp, 0, 1, 0, 1 };
            chrono_segment(CH_CLEAR_PROF, V.ds);   /* passe 10s */
            vkCmdClearDepthStencilImage(CB, t->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &sr);
            chrono_segment(CH_HORS_PASSE, -1);
            barriere();
        }
        V.n_clear_p++;
    }
    pthread_mutex_unlock(&g_mu);
}

void d3dv_scene(int debut) { (void)debut; if (V.actif) { pthread_mutex_lock(&g_mu); cb_ouvrir(); pthread_mutex_unlock(&g_mu); } }

/* ---- Draw : « mode lumière » ---------------------------------------------------- */
static VkPipeline pipeline_pour(VkFormat f)
{
    for (int i = 0; i < V.npipe; i++) if (V.pipe[i].f == f) return V.pipe[i].p;
    if (V.npipe >= 16) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = V.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = V.fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                              .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                  .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                          .colorAttachmentCount = 1, .pColorAttachmentFormats = &f };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci,
                                         .stageCount = 2, .pStages = st, .pVertexInputState = &vin,
                                         .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
                                         .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                         .layout = V.layout };
    VkPipeline p;
    double t0 = maintenant_ms();   /* passe 10s (cache de pipelines) : compté pour [BILAN] [PIPECACHE] */
    VkResult r = vkCreateGraphicsPipelines(V.dev, PC.cache, 1, &gpi, NULL, &p);
    pcache_compter(!r, maintenant_ms() - t0);
    if (r) { LOG("[VK] pipeline « lumière » pour VkFormat %d : échec (VkResult %d)", (int)f, (int)r); return VK_NULL_HANDLE; }
    LOG("[VK] pipeline « lumière » créé pour VkFormat %d (triangle dégénéré, shaders SM2/3 non traduits)", (int)f);
    V.pipe[V.npipe].f = f; V.pipe[V.npipe++].p = p;
    return p;
}

void d3dv_draw(uint32_t prim, uint32_t nprim)
{
    (void)prim;
    if (!V.actif) return;
    pthread_mutex_lock(&g_mu);
    VkPipeline p = V.rt >= 0 && nprim ? pipeline_pour(V.c[V.rt].vf) : VK_NULL_HANDLE;
    if (!p) { V.n_draw_ignores++; pthread_mutex_unlock(&g_mu); return; }
    cible *t = &V.c[V.rt];
    rendu_ouvrir(V.rt, -1);
    vkCmdBindPipeline(CB, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    float vw = V.vp[2] ? (float)V.vp[2] : (float)t->w, vh = V.vp[3] ? (float)V.vp[3] : (float)t->h;
    VkViewport vp = { (float)V.vp[0], (float)V.vp[1], vw, vh, V.zmin, V.zmax > V.zmin ? V.zmax : 1.f };
    VkRect2D sc = { { 0, 0 }, { t->w, t->h } };
    vkCmdSetViewport(CB, 0, 1, &vp);
    vkCmdSetScissor(CB, 0, 1, &sc);
    vkCmdDraw(CB, 3, 1, 0, 0);
    V.n_draw++;
    t->n_draw++;
    pthread_mutex_unlock(&g_mu);
}

/* ---- relecture ------------------------------------------------------------------- */
static int rb_assurer(VkDeviceSize taille)   /* sous g_mu : tampon de relecture hôte ≥ taille */
{
    if (taille > V.rbtaille) {
        attendre_tout(AT_DESTRUCTION);       /* passe 10r (v13) : une soumission en vol peut encore écrire dans V.rb */
        if (V.rbmap) vkUnmapMemory(V.dev, V.rbmem);
        if (V.rb) vkDestroyBuffer(V.dev, V.rb, NULL);
        if (V.rbmem) vkFreeMemory(V.dev, V.rbmem, NULL);
        V.rb = VK_NULL_HANDLE; V.rbmem = VK_NULL_HANDLE; V.rbmap = NULL; V.rbtaille = 0;
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = taille,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        if (vkCreateBuffer(V.dev, &bi, NULL, &V.rb)) return -1;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, V.rb, &mr);
        int32_t ty = type_memoire(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                    .memoryTypeIndex = (uint32_t)ty };
        if (ty < 0 || vkAllocateMemory(V.dev, &ai, NULL, &V.rbmem) || vkBindBufferMemory(V.dev, V.rb, V.rbmem, 0)
            || vkMapMemory(V.dev, V.rbmem, 0, VK_WHOLE_SIZE, 0, &V.rbmap)) return -1;
        V.rbtaille = taille;
    }
    return 0;
}
static int relire(int id, uint8_t *rgba)   /* sous g_mu ; rgba = w*h*4 */
{
    cible *t = &V.c[id];
    if (t->prof || (t->vf != VK_FORMAT_B8G8R8A8_UNORM && t->vf != VK_FORMAT_R8G8B8A8_UNORM)) return -1;
    if (rb_assurer((VkDeviceSize)t->w * t->h * 4)) return -1;
    cb_ouvrir();
    rendu_fermer();
    barriere();
    VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { t->w, t->h, 1 } };
    chrono_segment(CH_RELECTURE, -1);     /* passe 10s */
    vkCmdCopyImageToBuffer(CB, t->img, VK_IMAGE_LAYOUT_GENERAL, V.rb, 1, &bc);
    chrono_segment(CH_HORS_PASSE, -1);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(CB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (attendre_tout(AT_RELECTURE)) return -1;   /* passe 10r (v13) : soumet la copie, puis attend TOUTES les fences */
    const uint8_t *s = (const uint8_t *)V.rbmap;
    int bgra = t->vf == VK_FORMAT_B8G8R8A8_UNORM;
    for (size_t i = 0; i < (size_t)t->w * t->h; i++) {
        rgba[4 * i + 0] = s[4 * i + (bgra ? 2 : 0)];
        rgba[4 * i + 1] = s[4 * i + 1];
        rgba[4 * i + 2] = s[4 * i + (bgra ? 0 : 2)];
        rgba[4 * i + 3] = s[4 * i + 3];
    }
    V.n_lectures++;
    return 0;
}

/* Passe 7m (diagnostic) : toutes les cibles couleur 8 bits en PPM <dir>/cible_<frame>_<n>.ppm */
void d3dv_vider_cibles(const char *dir, uint32_t frame)
{
    if (!V.actif) return;
    mkdir(dir, 0755);
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < V.nc; i++) {
        cible *t = &V.c[i];
        if (!t->img || t->prof || (t->vf != VK_FORMAT_B8G8R8A8_UNORM && t->vf != VK_FORMAT_R8G8B8A8_UNORM)) continue;   /* V5.1 : oubliée */
        uint8_t *px = malloc((size_t)t->w * t->h * 4);
        if (px && !relire(i, px)) {
            char c[600]; snprintf(c, sizeof c, "%s/cible_%05u_%02d_%ux%u.ppm", dir, frame, i, t->w, t->h);
            FILE *o = fopen(c, "wb");
            if (o) {
                fprintf(o, "P6\n%u %u\n255\n", t->w, t->h);
                for (size_t k = 0; k < (size_t)t->w * t->h; k++) fwrite(px + 4 * k, 1, 3, o);
                fclose(o);
            }
        }
        free(px);
    }
    pthread_mutex_unlock(&g_mu);
}
int d3dv_lire(int id, uint8_t *rgba)
{
    if (!V.actif || id < 0 || id >= V.nc) return -1;
    pthread_mutex_lock(&g_mu);
    int r = relire(id, rgba);
    pthread_mutex_unlock(&g_mu);
    return r;
}

int d3dv_present(int id, const char *chemin, double moy[3])
{
    if (!V.actif) return -1;
    if (&sc2_att_present) sc2_att_present();   /* SC2_ATTENTES : marque CE fil « jeu » (faible : absent de test_d3d9v) */
    pthread_mutex_lock(&g_mu);
    g_dans_present = 1;   /* passe 10r (v13) : les soumissions de CETTE présentation (relire comprise) peuvent signaler le sémaphore « rendu » */
    V.n_present++;
    if (id >= 0 && id < V.nc) V.c[id].n_present++;
    int r = 0;
    if (chemin && id >= 0 && id < V.nc) {
        cible *t = &V.c[id];
        size_t np = (size_t)t->w * t->h;
        uint8_t *px = malloc(np * 4);
        r = px ? relire(id, px) : -1;
        if (!r) {
            uint64_t s[3] = { 0, 0, 0 };
            for (size_t i = 0; i < np; i++) { s[0] += px[4 * i]; s[1] += px[4 * i + 1]; s[2] += px[4 * i + 2]; }
            if (moy) for (int k = 0; k < 3; k++) moy[k] = (double)s[k] / (double)np;
            FILE *f = fopen(chemin, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", t->w, t->h);
                for (size_t i = 0; i < np; i++) fwrite(px + 4 * i, 1, 3, f);
                fclose(f);
            } else r = -1;
        }
        free(px);
    } else r = soumettre();
    if (CH.actif) {   /* passe 10s (chrono GPU) : compte rendu toutes les N frames, puis fenêtre remise à zéro */
        ch_win.frames++; ch_tot.frames++;
        if (ch_win.frames >= CH.period) {
            static char l[2304];
            chrono_ligne(l, sizeof l, &ch_win, 8);
            LOG("[GPU] %s", l);
            memset(&ch_win, 0, sizeof ch_win);
        }
    }
    if (!(V.n_present % 600)) pcache_sauver();   /* passe 10s (cache de pipelines) : n'écrit que si de NOUVEAUX pipelines */
    g_dans_present = 0;
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* ---- passe 10r (v13) : API de synchronisation pour la présentation (autre file Vulkan) ----
 * Un sémaphore binaire signalé doit être attendu exactement une fois : la présentation
 * consomme « rendu » (même quand elle n'affiche rien) avant que son emplacement revienne —
 * un sémaphore par emplacement suffit ; le backend attend le sémaphore de la présentation à
 * SA prochaine soumission, quelle qu'elle soit, une seule fois. En mode synchrone : rien. */
void d3dv_semaphore_rendu_demander(void)
{
    if (mode_synchrone()) return;
    pthread_mutex_lock(&g_mu);
    g_sem_rendu_demande = 1;
    pthread_mutex_unlock(&g_mu);
}
uint64_t d3dv_semaphore_rendu(void)
{
    pthread_mutex_lock(&g_mu);
    uint64_t s = (uint64_t)(uintptr_t)g_sem_rendu;   /* 0 si aucun : non demandé, synchrone ou échec */
    g_sem_rendu = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
    return s;
}
void d3dv_attendre_semaphore(uint64_t s, uint32_t etape)
{
    if (mode_synchrone() || !s) return;
    pthread_mutex_lock(&g_mu);
    g_sem_attendre = (VkSemaphore)(uintptr_t)s;
    g_sem_attendre_etape = etape;
    pthread_mutex_unlock(&g_mu);
}
int d3dv_en_vol(void) { return V.actif && !mode_synchrone(); }

static void bilan5(void);
void d3dv_bilan(void)
{
    if (!V.actif) return;
    bilan5();
    if (CH.actif) {   /* passe 10s (chrono GPU) : dernière fenêtre (incomplète), puis total du run */
        static char l[2304];
        if (ch_win.frames) {
            chrono_ligne(l, sizeof l, &ch_win, 8);
            LOG("[GPU] %s", l);
            memset(&ch_win, 0, sizeof ch_win);
        }
        chrono_ligne(l, sizeof l, &ch_tot, 12);
        printf("[BILAN] [GPU] passe 10s (chrono GPU) : %s\n", l);
    } else printf("[BILAN] [GPU] passe 10s (chrono GPU) : chronométrage GPU inactif\n");
    if (PC.depuis_sauvegarde) pcache_sauver();   /* passe 10s (cache de pipelines) : dernière écriture, sauf si rien de neuf */
    printf("[BILAN] [PIPECACHE] passe 10s (cache de pipelines) : %s, %s ; %llu octets chargés au démarrage ; "
           "%llu pipelines créés en %.1f ms de vkCreateGraphicsPipelines (max %.1f ms par appel) ; "
           "%u sauvegarde%s, dernière %llu octets, %u sauvegarde%s sautée%s : taille inchangée\n",
           PC.cache ? "actif" : "désactivé", PC.desactive ? PC.raison : PC.chemin,
           (unsigned long long)PC.octets_charges, (unsigned long long)PC.n_pipelines, PC.ms, PC.ms_max,
           PC.n_sauvegardes, PC.n_sauvegardes > 1 ? "s" : "", (unsigned long long)PC.derniere_taille,
           PC.n_inchangees, PC.n_inchangees > 1 ? "s" : "", PC.n_inchangees > 1 ? "s" : "");
    printf("[BILAN] Vulkan : %s — %d cibles (V5.1 : %d vivantes, %llu oubliées, %llu gardées en usage), %llu Clear couleur (%llu rectangles), %llu Clear profondeur, "
           "%llu draws « lumière » (%llu ignorés), %d pipelines, %llu Present, %llu soumissions, %llu relectures, "
           "%llu draws stencil\n",
           V.nom, V.nc, V.nc - g_ncib_libres, (unsigned long long)g_n_cib_oubliees, (unsigned long long)g_n_cib_gardees,
           (unsigned long long)V.n_clear_c, (unsigned long long)V.n_rects, (unsigned long long)V.n_clear_p,
           (unsigned long long)V.n_draw, (unsigned long long)V.n_draw_ignores, V.npipe, (unsigned long long)V.n_present,
           (unsigned long long)V.n_soumis, (unsigned long long)V.n_lectures, (unsigned long long)R.n_stencil);
    printf("[BILAN] [VOL] passe 10r (v13) : %s ; %llu soumissions (%llu vides, pour le sémaphore de présentation), "
           "%llu attentes de fence, %.1f ms cumulées à les attendre ; attendre_tout : %llu relectures, %llu destructions/réallocations, %llu listes pleines\n",
           d3dv_en_vol() ? "DEUX FRAMES EN VOL" : "SYNCHRONE (SC2_VK_SYNCHRONE=1)",
           (unsigned long long)V.n_soumis, (unsigned long long)V.n_soumis_vides,
           (unsigned long long)V.n_attentes_fence, V.ms_attentes,
           (unsigned long long)V.n_at_relecture, (unsigned long long)V.n_at_destruction,
           (unsigned long long)V.n_at_liste);
    printf("[BILAN] [VOL] passe 10s : %.2f soumissions par Present ; forcées par une moitié d'anneau pleine (%u Mio) : "
           "%llu en dessin, %llu en téléversement ; pool de descripteurs plein : %llu\n",
           V.n_present ? (double)V.n_soumis / (double)V.n_present : 0.0, (unsigned)(ANNEAU_TAILLE >> 20),
           (unsigned long long)V.n_soum_anneau_dessin, (unsigned long long)V.n_soum_anneau_tex, (unsigned long long)R.n_pool_plein);
    for (int i = 0; i < V.nc; i++) if (V.c[i].cle != CLE_MORTE)   /* V5.1 : oubliée sautée */
        printf("[BILAN]   cible n°%d %u×%u D3DFMT %u%s : %llu Clear couleur, %llu draws, %llu Present, %llu fois échantillonnée, "
               "StretchRect %llu src / %llu dst\n", i, V.c[i].w, V.c[i].h,
               V.c[i].fmt, V.c[i].prof ? " (profondeur)" : "", (unsigned long long)V.c[i].n_clear,
               (unsigned long long)V.c[i].n_draw, (unsigned long long)V.c[i].n_present, (unsigned long long)V.c[i].n_echant,
               (unsigned long long)V.c[i].n_copie_src, (unsigned long long)V.c[i].n_copie_dst);
    printf("[BILAN] couleurs de Clear (ARGB) :");
    for (int i = 0; i < V.ncouleurs; i++) printf(" 0x%08X×%llu", V.couleurs[i].col, (unsigned long long)V.couleurs[i].n);
    printf("\n");
}

/* =============================================================================
 * Passe 5d : draws RÉELS — shaders du jeu traduits hors-ligne (scripts/d3d9-vers-spirv.py),
 * textures du jeu téléversées, sommets/index/constantes du jeu copiés dans un anneau.
 *
 * Interface des shaders traduits (cf. l'en-tête du script) :
 *   set 0 : binding 0 UBO constantes VS, binding 1 UBO constantes PS, 2+n sampler n du PS
 *   push constants : VS [0,16) correction demi-pixel ; PS [16,32) test alpha
 * Simplifications v1 (documentées dans TRACE-PASSE5D) : niveau 0 des textures seulement,
 * pas de profondeur (ni test ni écriture), pas de culling, pas de scissor, pas d'instancing.
 * ============================================================================= */
static int image_creer(VkImageType ty, VkFormat f, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t couches,
                       VkImageCreateFlags fl, VkImage *img, VkDeviceMemory *mem)
{
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = fl, .imageType = ty, .format = f,
                             .extent = { w, h, d }, .mipLevels = mips, .arrayLayers = couches, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(V.dev, &ii, NULL, img)) return -1;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V.dev, *img, &mr);
    int32_t t = type_memoire(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) t = type_memoire(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || vkAllocateMemory(V.dev, &ai, NULL, mem) || vkBindImageMemory(V.dev, *img, *mem, 0)) {
        vkDestroyImage(V.dev, *img, NULL); *img = VK_NULL_HANDLE; return -1;
    }
    cb_ouvrir(); rendu_fermer();
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = *img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, couches } };
    vkCmdPipelineBarrier(CB, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    return 0;
}

static int prets5(void)
{
    if (R.pret) return R.pret > 0;
    R.pret = -1;
    /* passe 7v2 : SC2_OMBRES=1 → 16 bindings de plus (18 + k) : la MÊME vue de profondeur que 2 + k, avec un sampler à
     * comparaison, pour un échantillonnage Dref (sampler2DShadow) du traducteur ; sans la variable : 18 bindings, inchangé */
    uint32_t nb = d3dv_ombres() ? 34 : 18;
    VkDescriptorSetLayoutBinding b[34];
    for (uint32_t k = 0; k < nb; k++)
        b[k] = (VkDescriptorSetLayoutBinding){ k, k < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               k == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = V.push_desc ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                           .bindingCount = nb, .pBindings = b };
    if (vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &R.dsl)) return 0;
    if (!V.push_desc) {                      /* passe 7c : descripteurs classiques — un pool PAR EMPLACEMENT (10r v13) */
        VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * POOL_SETS },
                                       { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (nb - 2) * POOL_SETS } };
        VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = POOL_SETS,
                                          .poolSizeCount = 2, .pPoolSizes = ps };
        for (int i = 0; i < V.n_empl; i++)
            if (vkCreateDescriptorPool(V.dev, &pi, NULL, &V.empl[i].pool)) { LOG("[VK] vkCreateDescriptorPool en échec : pas de draws réels"); return 0; }
    }
    VkPushConstantRange pr[2] = { { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 }, { VK_SHADER_STAGE_FRAGMENT_BIT, 16, 16 } };
    VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &R.dsl, .pushConstantRangeCount = 2, .pPushConstantRanges = pr };
    if (vkCreatePipelineLayout(V.dev, &pl, NULL, &R.layout)) return 0;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)V.n_empl * ANNEAU_TAILLE,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                     | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    if (vkCreateBuffer(V.dev, &bi, NULL, &R.anneau)) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, R.anneau, &mr);
    int32_t t = type_memoire(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || vkAllocateMemory(V.dev, &ai, NULL, &R.anneau_mem) || vkBindBufferMemory(V.dev, R.anneau, R.anneau_mem, 0)
        || vkMapMemory(V.dev, R.anneau_mem, 0, VK_WHOLE_SIZE, 0, (void **)&R.anneau_ptr)) return 0;
    /* factices : blanc opaque, pour un sampler sans texture exploitable */
    static const VkImageType ty[3] = { VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D };
    static const VkImageViewType vt[3] = { VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_3D };
    for (int k = 0; k < 3; k++) {
        uint32_t couches = k == 1 ? 6 : 1;
        if (image_creer(ty[k], VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, couches, k == 1 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                        &R.fimg[k], &R.fmem[k])) return 0;
        VkClearColorValue blanc = { .float32 = { 1, 1, 1, 1 } };
        VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, couches };
        vkCmdClearColorImage(CB, R.fimg[k], VK_IMAGE_LAYOUT_GENERAL, &blanc, 1, &sr);
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = R.fimg[k], .viewType = vt[k],
                                     .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = sr };
        if (vkCreateImageView(V.dev, &vi, NULL, &R.fvue[k])) return 0;
    }
    barriere();
    R.pret = 1;
    R.epoque = 1;                             /* passe 10r : 0 = « jamais envoyé » pour cblk[] */
    LOG("[VK] draws réels prêts : descripteurs %s, anneau %u Mo (%d emplacement%s × %u Mo), textures factices blanches 2D/cube/3D",
        V.push_desc ? "poussés" : "classiques (un set par draw, pool par emplacement remis à zéro quand il revient)",
        (unsigned)((uint64_t)V.n_empl * ANNEAU_TAILLE >> 20), V.n_empl, V.n_empl > 1 ? "s" : "", (unsigned)(ANNEAU_TAILLE >> 20));
    return 1;
}

int d3dv_module(const uint32_t *spv, uint32_t nmots, int ps)
{
    if (!V.actif) return -1;
    pthread_mutex_lock(&g_mu);
    int id = -1;
    if (R.nmod >= MAX_MODULES) {
        static int dit;
        if (!dit++) LOG("[VK] plafond de %d modules SPIR-V atteint : shaders suivants non traduits", MAX_MODULES);
    }
    if (R.nmod < MAX_MODULES) {
        VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = 4u * nmots, .pCode = spv };
        if (!vkCreateShaderModule(V.dev, &smi, NULL, &R.mod[R.nmod])) { R.ps[R.nmod] = ps; id = R.nmod++; }
    }
    pthread_mutex_unlock(&g_mu);
    return id;
}

/* ---- anneau (une frame par emplacement — passe 10r v13 : moitié de l'emplacement courant) ---- */
static VkDeviceSize anneau(const void *src, VkDeviceSize n, VkDeviceSize align)   /* sous g_mu ; ~0 si plein */
{
    VkDeviceSize fin = ANNEAU_FIN;
    VkDeviceSize p = (R.pos + align - 1) & ~(align - 1);
    if (p + n > fin) return ~(VkDeviceSize)0;
    if (src) memcpy(R.anneau_ptr + p, src, n);
    R.pos = p + n;
    return p;
}
/* Passe 10t : réserve `reserve` octets mais n'en recopie que les `copie` premiers (bloc de constantes : cf. consts_a_copier). */
static VkDeviceSize anneau_partiel(const void *src, VkDeviceSize copie, VkDeviceSize reserve, VkDeviceSize align)
{
    VkDeviceSize p = anneau(NULL, reserve, align);
    if (p != ~(VkDeviceSize)0 && src) memcpy(R.anneau_ptr + p, src, copie < reserve ? copie : reserve);
    return p;
}

/* ---- passe 10r : tampons de sommets/index STATIQUES ----------------------------
 * Un tampon D3D9 sans D3DUSAGE_DYNAMIC, dont le contenu (génération O_GEN du device factice,
 * avancée à chaque Lock) n'a pas changé, est copié UNE FOIS dans un VkBuffer dédié de sa taille
 * totale — mémoire HOST_VISIBLE|HOST_COHERENT mappée en permanence, comme l'anneau — puis lié
 * directement à chaque dessin au décalage demandé : plus aucune copie de la plage utilisée dans
 * l'anneau. Toute régénération (Lock) crée un VkBuffer NEUF : l'ancien peut être lié au tampon
 * de commandes en cours et lu à l'exécution par des draws déjà enregistrés — il part en
 * libération différée. Au-delà de TAMPONS_PLAFOND octets en cache, on retombe sur l'anneau. */
/* Mesure Fold du 19/10 (v12, plateau 180-330 s) : avec le cache de tampons 11,4 FPS / jeu 60 ms, sans 12,9 FPS / jeu
 * 49 ms → désactivé par défaut. Passe 10s (19/10, v15a, jeu actif, 60 s profilées) : AVEC la garde de volatilité, le
 * cache gagne — CPU du fil du jeu 43,8 → 41,4 ms/frame (memmove 3,7 → 1,8 ms, ioctl 0,5 → 1,2 ms), 18,4 → 19,2 FPS —
 * donc ACTIF par défaut ; SC2_VK_CACHE_TAMPONS=0 le coupe. */
static int sans_cache_tampons(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SC2_VK_CACHE_TAMPONS"); v = e && *e == '0'; }
    return v;
}
static int sans_cache_consts(void)   /* passe 10r : SC2_VK_SANS_CACHE_CONSTS=1 → recopie à chaque dessin */
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SC2_VK_SANS_CACHE_CONSTS"); v = e && *e == '1'; }
    return v;
}
static int consts_completes(void)   /* passe 10t : SC2_VK_CONSTS_COMPLETES=1 → blocs de constantes entiers (A/B) */
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SC2_VK_CONSTS_COMPLETES"); v = e && *e == '1'; }
    return v;
}
static int cblk_reutilisable(int k, uint32_t gen, VkDeviceSize besoin)   /* passe 10t : cf. bloc_consts_reutilisable */
{
    return bloc_consts_reutilisable(R.cblk[k].epoque, R.cblk[k].gen, R.cblk[k].etendue, R.epoque, gen, besoin);
}
static int sans_cache_volatils(void)   /* passe 10t : SC2_VK_CACHE_VOLATILS=0 → recopie à chaque dessin (actif par défaut) */
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SC2_VK_CACHE_VOLATILS"); v = e && *e == '0'; }
    return v;
}
/* passe 10t : sert du repli anneau pour une plage identifiée — déjà recopiée à CETTE époque
 * pour la même clé (tampon invité, génération, décalage, taille, genre) → l'emplacement est
 * réutilisé sans recopie ; sinon recopie (anneau) puis entrée en table. Sans identité (obj = 0 :
 * user pointer, attribut constant), taille nulle (attribut constant de 16 octets) ou cache
 * coupé : chemin d'avant, anneau pur. n = octets à copier (16 pour un flux de taille 0).
 * Rend ~0 si l'anneau déborde (théorique : besoin sur-réserve la place). Sous g_mu. */
static VkDeviceSize volatils_puiser(uint32_t obj, uint32_t gen, uint32_t decalage, uint32_t taille, uint32_t genre,
                                    const void *src, VkDeviceSize n, VkDeviceSize align)
{
    if (sans_cache_volatils() || !obj || !taille) return anneau(src, n, align);
    uint64_t off;
    if (volatils_trouver(obj, gen, decalage, taille, genre, R.epoque, &off)) {
        if (genre == VOLATILS_INDEX) R.n_volatils_index++; else R.n_volatils_flux++;
        R.o_volatils += taille;
        R.o_evites += taille;
        return (VkDeviceSize)off;
    }
    VkDeviceSize o = anneau(src, n, align);
    if (o != ~(VkDeviceSize)0) volatils_noter(obj, gen, decalage, taille, genre, R.epoque, (uint64_t)o);
    return o;
}
static int tam_creer(tam10 *t, uint32_t cle, uint32_t gen, uint32_t taille, const void *donnees)   /* sous g_mu */
{
    memset(t, 0, sizeof *t);
    t->dalle = -1; t->off = 0;                 /* dédié, sauf plage trouvée ci-dessous */
    /* 21/10 : petit tampon → plage dans une grande dalle : le VkBuffer et la mémoire sont ceux
     * de la dalle (mem nulle ici, map = map de la dalle + off), un vkCreateBuffer, un
     * vkAllocateMemory et un vkMapMemory de moins par tampon. Pas de place : chemin dédié. */
    if (dalles_actives() && taille <= DALLE_TAILLE / 4 && !dalle_prendre(taille, &t->dalle, &t->off)) {
        t->buf = R.dalle[t->dalle].buf; t->mem = VK_NULL_HANDLE; t->map = R.dalle[t->dalle].map + t->off;
        memcpy(t->map, donnees, taille);
        t->cle = cle; t->gen = gen; t->taille = taille;
        return 0;
    }
    t->dalle = -1;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = taille,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT };
    if (!taille || vkCreateBuffer(V.dev, &bi, NULL, &t->buf)) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, t->buf, &mr);
    int32_t ty = type_memoire(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = (uint32_t)ty };
    if (ty < 0 || vkAllocateMemory(V.dev, &ai, NULL, &t->mem) || vkBindBufferMemory(V.dev, t->buf, t->mem, 0)
        || vkMapMemory(V.dev, t->mem, 0, VK_WHOLE_SIZE, 0, (void **)&t->map)) {
        if (t->buf) vkDestroyBuffer(V.dev, t->buf, NULL);
        if (t->mem) vkFreeMemory(V.dev, t->mem, NULL);
        memset(t, 0, sizeof *t);
        t->dalle = -1;
        return -1;
    }
    memcpy(t->map, donnees, taille);
    t->cle = cle; t->gen = gen; t->taille = taille;
    return 0;
}
/* Sert un flux ou l'index depuis le cache : 1 si buf/off reçoivent le VkBuffer en cache et son
 * décalage SANS AUCUNE copie, 2 s'ils les reçoivent après (re)création (tout le tampon invité
 * vient d'être copié), 0 s'il faut copier dans l'anneau (UP, dynamique, vide, hors bornes —
 * la plage utilisée peut dépasser le tampon : l'anneau, lui, tolérait la lecture invitée —,
 * plafond atteint, création refusée). sous g_mu. */
static int tampon_puiser(uint32_t obj, uint32_t gen, uint32_t usage, uint32_t taille_tot, const void *donnees,
                         uint32_t decalage, uint32_t taille, VkBuffer *buf, VkDeviceSize *off)
{
    if (sans_cache_tampons() || !obj || (usage & 0x200u /* D3DUSAGE_DYNAMIC */)
        || !taille || !taille_tot || (uint64_t)decalage + taille > taille_tot) return 0;
    int id = tam_trouver(obj);
    tam10 *t = id >= 0 ? &R.tam[id] : NULL;
    if (t && t->volatil) { R.n_tam_volatils_servis++; return 0; }
    if (t && t->gen == gen && t->taille == taille_tot) {
        *buf = t->buf;
        *off = t->dalle >= 0 ? (VkDeviceSize)(t->off + decalage) : decalage;   /* 21/10 : plage de dalle */
        return 1;
    }
    /* Garde de volatilité (mesure Fold v12 : backend +20 ms/frame) : un tampon « statique » verrouillé à répétition
     * coûtait à chaque nouvelle génération un vkAllocateMemory, une copie complète et un vkFreeMemory (ce dernier dans
     * soumettre(), donc dans la tranche backend). Au-delà de TAMPON_REMPL_MAX remplacements, il sort du cache. */
    if (t && t->remplacements + 1 > TAMPON_REMPL_MAX) {
        if (t->dalle >= 0) liberer_plage_differer(t->dalle, t->off, arrondi256(t->taille));
        else liberer_differer(t->buf, t->mem);
        R.o_tam -= t->taille;
        t->buf = VK_NULL_HANDLE; t->mem = VK_NULL_HANDLE; t->map = NULL; t->dalle = -1; t->volatil = 1;
        R.n_tam_volatils++;
        return 0;
    }
    if (R.o_tam + taille_tot > TAMPONS_PLAFOND) { R.n_tam_plafond++; return 0; }
    if (!t && R.ntam >= MAX_TAMPONS && !g_ntam_libres) return 0;   /* V5.1 : indices oubliés remis en service */
    tam10 n;
    if (tam_creer(&n, obj, gen, taille_tot, donnees)) return 0;
    if (t) {                                   /* remplacée (génération ou taille) : l'ancienne part ensuite */
        if (t->dalle >= 0) liberer_plage_differer(t->dalle, t->off, arrondi256(t->taille));
        else liberer_differer(t->buf, t->mem);
        R.o_tam -= t->taille;
        R.n_tam_remplaces++;
        n.remplacements = t->remplacements + 1;
        *t = n;
    } else {
        int idn = g_ntam_libres ? g_tam_libres[--g_ntam_libres] : R.ntam++;   /* V5.1 */
        R.tam[idn] = n; tam_noter(idn);
    }
    R.o_tam += taille_tot;
    *buf = n.buf;
    *off = n.dalle >= 0 ? (VkDeviceSize)(n.off + decalage) : decalage;   /* 21/10 : plages de dalles */
    return 2;
}

/* ---- textures ----
 * Passe 7l : chemin de texture complet.
 *   - natif : staging (anneau) → VkImage du format D3D équivalent, TOUS les niveaux de mip (le
 *     device factice les range serrés, dans l'ordre d'un DDS : cf. niveau_offset de sc2_d3d9.c) ;
 *     DXT1..5 en VK_FORMAT_BC1..3 quand le device a textureCompressionBC ;
 *   - LOGICIEL : sans BC (Adreno, émulateur ; SC2_VK_SANS_BC=1 pour le forcer), DXTn décodés par
 *     le CPU vers RGBA8 ; même repli pour les formats D3D sans équivalent Vulkan obligatoire
 *     (R8G8B8, A4R4G4B4, X4R4G4B4, R3G3B2, X8B8G8R8) ;
 *   - SC2_TEX_DUMP=<dossier> : chaque téléversement (niveau 0, 400 au plus) vidé en PPM (RGB) + PGM (alpha).
 */
enum { CONV_AUCUNE, CONV_BC1, CONV_BC2, CONV_BC3, CONV_PIXELS };
typedef struct { VkFormat vf; VkComponentMapping cm; uint32_t bloc, bpp; int conv; } fmt_tex;

static int fmt_bloc_d3d(uint32_t f)
{
    switch (f) { case 0x31545844u: return 8; case 0x32545844u: case 0x33545844u: case 0x34545844u: case 0x35545844u: return 16; }
    return 0;
}
static uint32_t bpp_d3d(uint32_t f)
{
    switch (f) {
    case 28: case 50: case 27: return 1;
    case 23: case 24: case 25: case 26: case 30: case 51: case 111: return 2;
    case 20: return 3;
    case 113: case 115: return 8;
    case 116: return 16;
    default: return 4;
    }
}
static int vk_tex(uint32_t f, fmt_tex *o)
{
    const VkComponentMapping id = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                    VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    o->cm = id; o->bloc = (uint32_t)fmt_bloc_d3d(f); o->bpp = bpp_d3d(f); o->conv = CONV_AUCUNE;
    switch (f) {
    case 21: o->vf = VK_FORMAT_B8G8R8A8_UNORM; return 0;
    case 22: o->cm.a = VK_COMPONENT_SWIZZLE_ONE; o->vf = VK_FORMAT_B8G8R8A8_UNORM; return 0;
    case 32: o->vf = VK_FORMAT_R8G8B8A8_UNORM; return 0;
    case 23: o->vf = VK_FORMAT_R5G6B5_UNORM_PACK16; return 0;
    case 24: o->cm.a = VK_COMPONENT_SWIZZLE_ONE; o->vf = VK_FORMAT_A1R5G5B5_UNORM_PACK16; return 0;   /* X1R5G5B5 */
    case 25: o->vf = VK_FORMAT_A1R5G5B5_UNORM_PACK16; return 0;
    case 28: o->cm = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
             o->vf = VK_FORMAT_R8_UNORM; return 0;                                   /* A8 */
    case 50: o->cm = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
             o->vf = VK_FORMAT_R8_UNORM; return 0;                                   /* L8 */
    case 51: o->cm = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G };
             o->vf = VK_FORMAT_R8G8_UNORM; return 0;                                 /* A8L8 */
    case 35: o->vf = VK_FORMAT_A2R10G10B10_UNORM_PACK32; return 0;                  /* A2R10G10B10 */
    case 31: o->vf = VK_FORMAT_A2B10G10R10_UNORM_PACK32; return 0;                  /* A2B10G10R10 */
    case 34: o->vf = VK_FORMAT_R16G16_UNORM; return 0;                              /* G16R16 */
    case 36: o->bpp = 8; o->vf = VK_FORMAT_R16G16B16A16_UNORM; return 0;            /* A16B16G16R16 */
    case 111: o->vf = VK_FORMAT_R16_SFLOAT; return 0;
    case 112: o->vf = VK_FORMAT_R16G16_SFLOAT; return 0;
    case 113: o->vf = VK_FORMAT_R16G16B16A16_SFLOAT; return 0;
    case 114: o->vf = VK_FORMAT_R32_SFLOAT; return 0;
    case 115: o->vf = VK_FORMAT_R32G32_SFLOAT; return 0;
    case 116: o->vf = VK_FORMAT_R32G32B32A32_SFLOAT; return 0;
    case 0x31545844u: o->vf = V.bc ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM; o->conv = V.bc ? 0 : CONV_BC1; return 0;
    case 0x32545844u: case 0x33545844u:
        o->vf = V.bc ? VK_FORMAT_BC2_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM; o->conv = V.bc ? 0 : CONV_BC2; return 0;
    case 0x34545844u: case 0x35545844u:
        o->vf = V.bc ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM; o->conv = V.bc ? 0 : CONV_BC3; return 0;
    case 20: case 26: case 27: case 30: case 33:                                    /* R8G8B8, A4R4G4B4, R3G3B2, X4R4G4B4, X8B8G8R8 */
        o->vf = VK_FORMAT_R8G8B8A8_UNORM; o->conv = CONV_PIXELS; return 0;
    default: o->vf = VK_FORMAT_UNDEFINED; return -1;
    }
}

/* taille d'un niveau tel que le range le device factice (= sc2_d3d9.c pitch_of × rows_of × profondeur) */
static uint32_t lvl5(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }
static uint32_t taille_niveau(uint32_t f, uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t b = (uint32_t)fmt_bloc_d3d(f);
    if (b) return ((w + 3) / 4) * b * ((h + 3) / 4) * d;
    return w * bpp_d3d(f) * h * d;
}

/* ---- décodage logiciel BC1/BC2/BC3 (DXT1..5) → RGBA8 ----
 * Règles de la Khronos Data Format Specification (§ S3TC) : couleurs 5:6:5 étendues par réplication
 * des bits de poids fort ; BC1 : c0 > c1 → 4 couleurs, sinon 3 + noir transparent ; BC2/BC3 : bloc
 * couleur TOUJOURS en mode 4 couleurs ; alpha BC2 explicite 4 bits ; alpha BC3 interpolé 8/6 niveaux. */
static void rgb565(uint16_t c, int o[3])
{
    int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    o[0] = r << 3 | r >> 2; o[1] = g << 2 | g >> 4; o[2] = b << 3 | b >> 2;
}
static void bloc_couleur(const uint8_t *s, int bc1, uint8_t px[16][4])
{
    uint16_t c0 = (uint16_t)(s[0] | s[1] << 8), c1 = (uint16_t)(s[2] | s[3] << 8);
    int p[4][4], a[3], b[3];
    rgb565(c0, a); rgb565(c1, b);
    for (int k = 0; k < 3; k++) { p[0][k] = a[k]; p[1][k] = b[k]; }
    p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
    if (!bc1 || c0 > c1)
        for (int k = 0; k < 3; k++) { p[2][k] = (2 * a[k] + b[k]) / 3; p[3][k] = (a[k] + 2 * b[k]) / 3; }
    else {
        for (int k = 0; k < 3; k++) { p[2][k] = (a[k] + b[k]) / 2; p[3][k] = 0; }
        p[3][3] = 0;
    }
    uint32_t idx = (uint32_t)s[4] | (uint32_t)s[5] << 8 | (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24;
    for (int i = 0; i < 16; i++) { const int *q = p[idx >> (2 * i) & 3]; px[i][0] = (uint8_t)q[0]; px[i][1] = (uint8_t)q[1];
                                   px[i][2] = (uint8_t)q[2]; px[i][3] = (uint8_t)q[3]; }
}
static void bloc_alpha_bc3(const uint8_t *s, uint8_t px[16][4])
{
    int a[8]; a[0] = s[0]; a[1] = s[1];
    if (a[0] > a[1]) for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
    else { for (int k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5; a[6] = 0; a[7] = 255; }
    uint64_t idx = 0; for (int k = 0; k < 6; k++) idx |= (uint64_t)s[2 + k] << (8 * k);
    for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)a[idx >> (3 * i) & 7];
}
/* une image (w×h×d) DXTn → RGBA8 serré */
static void decoder_bc(int conv, const uint8_t *src, uint32_t w, uint32_t h, uint32_t d, uint8_t *dst)
{
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, tb = conv == CONV_BC1 ? 8 : 16;
    uint8_t px[16][4];
    for (uint32_t z = 0; z < d; z++)
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t *s = src + ((size_t)(z * bh + by) * bw + bx) * tb;
                if (conv == CONV_BC1) bloc_couleur(s, 1, px);
                else {
                    bloc_couleur(s + 8, 0, px);
                    if (conv == CONV_BC2) for (int i = 0; i < 16; i++) { int v = s[i / 2] >> (4 * (i & 1)) & 15; px[i][3] = (uint8_t)(v * 17); }
                    else bloc_alpha_bc3(s, px);
                }
                for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                        memcpy(dst + 4 * (((size_t)z * h + by * 4 + y) * w + bx * 4 + x), px[y * 4 + x], 4);
            }
}
/* formats non compressés → RGBA8 (repli et vidages) ; 0 si fait */
static int pixels_rgba(uint32_t f, const uint8_t *s, size_t n, uint8_t *o)
{
    for (size_t i = 0; i < n; i++, o += 4) {
        uint32_t v;
        switch (f) {
        case 20: o[0] = s[3 * i + 2]; o[1] = s[3 * i + 1]; o[2] = s[3 * i]; o[3] = 255; break;
        case 21: case 22: o[0] = s[4 * i + 2]; o[1] = s[4 * i + 1]; o[2] = s[4 * i]; o[3] = f == 21 ? s[4 * i + 3] : 255; break;
        case 32: case 33: memcpy(o, s + 4 * i, 4); if (f == 33) o[3] = 255; break;
        case 23: { int c[3]; rgb565((uint16_t)(s[2 * i] | s[2 * i + 1] << 8), c); o[0] = (uint8_t)c[0]; o[1] = (uint8_t)c[1]; o[2] = (uint8_t)c[2]; o[3] = 255; break; }
        case 24: case 25:
            v = (uint32_t)(s[2 * i] | s[2 * i + 1] << 8);
            o[0] = (uint8_t)((v >> 10 & 31) * 255 / 31); o[1] = (uint8_t)((v >> 5 & 31) * 255 / 31); o[2] = (uint8_t)((v & 31) * 255 / 31);
            o[3] = f == 25 ? (v >> 15 ? 255 : 0) : 255; break;
        case 26: case 30:
            v = (uint32_t)(s[2 * i] | s[2 * i + 1] << 8);
            o[0] = (uint8_t)((v >> 8 & 15) * 17); o[1] = (uint8_t)((v >> 4 & 15) * 17); o[2] = (uint8_t)((v & 15) * 17);
            o[3] = f == 26 ? (uint8_t)((v >> 12) * 17) : 255; break;
        case 27: v = s[i]; o[0] = (uint8_t)((v >> 5) * 255 / 7); o[1] = (uint8_t)((v >> 2 & 7) * 255 / 7); o[2] = (uint8_t)((v & 3) * 85); o[3] = 255; break;
        case 28: o[0] = o[1] = o[2] = 0; o[3] = s[i]; break;
        case 50: o[0] = o[1] = o[2] = s[i]; o[3] = 255; break;
        case 51: o[0] = o[1] = o[2] = s[2 * i]; o[3] = s[2 * i + 1]; break;
        default: return -1;
        }
    }
    return 0;
}
/* un niveau quelconque → RGBA8 (repli, vidages, tests) ; 0 si fait */
static int vers_rgba(uint32_t f, const uint8_t *src, uint32_t w, uint32_t h, uint32_t d, uint8_t *dst)
{
    switch (f) {
    case 0x31545844u: decoder_bc(CONV_BC1, src, w, h, d, dst); return 0;
    case 0x32545844u: case 0x33545844u: decoder_bc(CONV_BC2, src, w, h, d, dst); return 0;
    case 0x34545844u: case 0x35545844u: decoder_bc(CONV_BC3, src, w, h, d, dst); return 0;
    default: return pixels_rgba(f, src, (size_t)w * h * d, dst);
    }
}
int d3dv_decoder_rgba(uint32_t fmt, const void *src, uint32_t w, uint32_t h, uint8_t *rgba)
{ return vers_rgba(fmt, src, w, h, 1, rgba); }

/* SC2_TEX_DUMP=<dossier> */
static void vider_texture(uint64_t cle, uint32_t gen, uint32_t f, const uint8_t *src, uint32_t w, uint32_t h)
{
    static const char *dir; static int essaye, n;
    if (!essaye) { essaye = 1; dir = getenv("SC2_TEX_DUMP"); if (dir && *dir) mkdir(dir, 0755); else dir = NULL; }
    if (!dir || n >= 400 || (size_t)w * h > 4096u * 4096u) return;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return;
    if (!vers_rgba(f, src, w, h, 1, rgba)) {
        char c[600];
        snprintf(c, sizeof c, "%s/tex_%04d_%010llx_g%u_%ux%u_f%X", dir, n++, (unsigned long long)cle, gen, w, h, f);
        size_t l = strlen(c);
        strcpy(c + l, ".ppm"); FILE *o = fopen(c, "wb");
        if (o) { fprintf(o, "P6\n%u %u\n255\n", w, h); for (size_t i = 0; i < (size_t)w * h; i++) fwrite(rgba + 4 * i, 1, 3, o); fclose(o); }
        strcpy(c + l, "_a.pgm"); o = fopen(c, "wb");
        if (o) { fprintf(o, "P5\n%u %u\n255\n", w, h); for (size_t i = 0; i < (size_t)w * h; i++) fputc(rgba[4 * i + 3], o); fclose(o); }
    }
    free(rgba);
}

/* bilan par format : textures natives / logicielles */
static struct { uint32_t fmt; int conv; uint64_t n_tex, n_tel, octets; uint32_t nivmax; } g_tfmt[32];
static int g_ntfmt;
static void noter_fmt(uint32_t f, int conv, int nouvelle, uint64_t octets, uint32_t niv)
{
    int i = 0; while (i < g_ntfmt && g_tfmt[i].fmt != f) i++;
    if (i == g_ntfmt) { if (g_ntfmt == 32) return; g_ntfmt++; memset(&g_tfmt[i], 0, sizeof g_tfmt[i]); g_tfmt[i].fmt = f; }
    g_tfmt[i].conv = conv; g_tfmt[i].n_tex += (uint64_t)nouvelle;
    if (octets) { g_tfmt[i].n_tel++; g_tfmt[i].octets += octets; }
    if (niv > g_tfmt[i].nivmax) g_tfmt[i].nivmax = niv;
}

int d3dv_texture_niveaux(uint64_t cle, int genre, uint32_t w, uint32_t h, uint32_t d, uint32_t fmt, uint32_t niveaux,
                         const void *donnees, uint32_t taille, uint32_t gen)
{
    if (!V.actif || !w || !h) return -1;
    uint32_t faces = genre == 3 ? 6 : 1;                       /* cube (passe 7l) : 6 faces × chaîne, ordre DDS */
    pthread_mutex_lock(&g_mu);
    int id = -1;
    if (!prets5()) goto fin;
    fmt_tex ft;
    if (vk_tex(fmt, &ft)) { R.n_tex_ko++; goto fin; }
    if (genre != 4) d = 1;
    if (genre == 3) h = w;
    if (!niveaux) niveaux = 1;
    if (niveaux > 16) niveaux = 16;
    /* la chaîne ne descend pas sous 1×1×1 ; et seuls les niveaux présents dans les données comptent */
    uint32_t nmax = 1; while (nmax < 16 && (lvl5(w, nmax - 1) > 1 || lvl5(h, nmax - 1) > 1 || lvl5(d, nmax - 1) > 1)) nmax++;
    if (niveaux > nmax) niveaux = nmax;
    uint32_t pas = taille / faces;                             /* octets par face (cube : chaîne complète du jeu) */
    if (donnees) {
        uint64_t cum = 0; uint32_t k = 0;
        for (; k < niveaux; k++) { cum += taille_niveau(fmt, lvl5(w, k), lvl5(h, k), lvl5(d, k)); if (cum > pas) break; }
        if (!k) { R.n_tex_ko++; goto fin; }
        niveaux = k;
    }
    id = tex_trouver(cle);   /* passe 10r */
    tex5 *t = id >= 0 ? &R.t[id] : NULL;
    if (t && (t->w != w || t->h != h || t->d != d || t->fmt != fmt || t->niv != niveaux)) {
        attendre_tout(AT_DESTRUCTION);   /* passe 10r (v13) : des soumissions en vol échantillonnent encore cette texture */
        vkDestroyImageView(V.dev, t->vue, NULL); vkDestroyImage(V.dev, t->img, NULL); vkFreeMemory(V.dev, t->mem, NULL);
        t->img = VK_NULL_HANDLE; t->gen = ~0u;
    }
    int nouvelle = 0;
    if (!t) {
        if (g_ntex_libres) id = g_tex_libres[--g_ntex_libres];   /* V5.1 : indice d'une texture oubliée */
        else if (R.nt >= MAX_TEX) goto fin;
        else id = R.nt++;
        t = &R.t[id]; memset(t, 0, sizeof *t); t->cle = cle; t->gen = ~0u;
        tex_noter(id);
    }
    if (!t->img) {
        t->genre = genre; t->w = w; t->h = h; t->d = d; t->fmt = fmt; t->vf = ft.vf; t->niv = niveaux; t->conv = ft.conv;
        if (image_creer(genre == 4 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, ft.vf, w, h, d, niveaux, faces,
                        genre == 3 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, &t->img, &t->mem)) {
            tex_retirer(id);
            if (id == R.nt - 1) R.nt--; else { t->cle = CLE_MORTE; g_tex_libres[g_ntex_libres++] = id; }   /* V5.1 */
            id = -1; goto fin;
        }
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
                                     .viewType = genre == 4 ? VK_IMAGE_VIEW_TYPE_3D : genre == 3 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
                                     .format = ft.vf, .components = ft.cm,
                                     .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, niveaux, 0, faces } };
        vkCreateImageView(V.dev, &vi, NULL, &t->vue);
        nouvelle = 1;
    }
    if (t->gen != gen && donnees) {
        /* place dans l'anneau : niveaux convertis (RGBA8) ou tels quels */
        VkDeviceSize n = 0;
        for (uint32_t k = 0; k < niveaux; k++) {
            uint32_t lw = lvl5(w, k), lh = lvl5(h, k), ld = lvl5(d, k);
            n += (ft.conv ? (VkDeviceSize)lw * lh * ld * 4 : taille_niveau(fmt, lw, lh, ld)) + 16;
        }
        n *= faces;
        if (n > ANNEAU_TAILLE / 2) { R.n_tex_ko++; goto fin; }
        if (R.pos + n + 256 > ANNEAU_FIN) { V.n_soum_anneau_tex++; soumettre(); }   /* passe 10r (v13) : sa moitié est pleine */
        VkBufferImageCopy rg[6 * 16];
        uint32_t nrg = 0;
        int ok = 1;
        for (uint32_t f = 0; f < faces && ok; f++) {
            const uint8_t *src = (const uint8_t *)donnees + (size_t)f * pas;
            for (uint32_t k = 0; k < niveaux && ok; k++) {
                uint32_t lw = lvl5(w, k), lh = lvl5(h, k), ld = lvl5(d, k), tn = taille_niveau(fmt, lw, lh, ld);
                VkDeviceSize o;
                if (ft.conv) {
                    o = anneau(NULL, (VkDeviceSize)lw * lh * ld * 4, 16);
                    if (o != ~(VkDeviceSize)0 && vers_rgba(fmt, src, lw, lh, ld, R.anneau_ptr + o)) ok = 0;
                } else o = anneau(src, tn, 16);
                if (o == ~(VkDeviceSize)0) ok = 0;
                rg[nrg++] = (VkBufferImageCopy){ .bufferOffset = o, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, k, f, 1 },
                                                 .imageExtent = { lw, lh, ld } };
                src += tn;
            }
        }
        if (ok) {
            cb_ouvrir(); rendu_fermer(); barriere();
            chrono_segment(CH_TELEVERSEMENT, -1);   /* passe 10s */
            vkCmdCopyBufferToImage(CB, R.anneau, t->img, VK_IMAGE_LAYOUT_GENERAL, nrg, rg);
            chrono_segment(CH_HORS_PASSE, -1);
            barriere();
            t->gen = gen; R.n_tex_tel++; R.o_tel += n; R.n_niv_tel += niveaux * faces; R.n_cubes_tel += genre == 3;
            if (ft.conv) { R.n_tel_logiciel++; R.o_logiciel += n; }
            noter_fmt(fmt, ft.conv, nouvelle, n, niveaux);
            vider_texture(cle, gen, fmt, donnees, w, h);
        } else R.n_tex_ko++;
    } else if (nouvelle) noter_fmt(fmt, ft.conv, 1, 0, niveaux);
fin:
    pthread_mutex_unlock(&g_mu);
    return id;
}
int d3dv_texture(uint64_t cle, int genre, uint32_t w, uint32_t h, uint32_t d, uint32_t fmt,
                 const void *donnees, uint32_t taille, uint32_t gen)
{ return d3dv_texture_niveaux(cle, genre, w, h, d, fmt, 1, donnees, taille, gen); }

/* Relecture d'un niveau de texture 2D (tests) : blit (NEAREST, même taille) vers une image RGBA8 —
 * le décodage est celui du GPU (BC natif : décodeur du pilote) ; canaux bruts, sans le swizzle de vue. */
int d3dv_texture_relire(int id, uint32_t couche, uint32_t niveau, uint8_t *rgba)
{
    if (!V.actif) return -1;
    pthread_mutex_lock(&g_mu);
    int r = -1;
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    if (id < 0 || id >= R.nt || !R.t[id].img || niveau >= R.t[id].niv || R.t[id].genre == 4 || R.t[id].gen == ~0u
        || couche >= (R.t[id].genre == 3 ? 6u : 1u)) goto fin;
    tex5 *t = &R.t[id];
    uint32_t w = lvl5(t->w, niveau), h = lvl5(t->h, niveau);
    if (rb_assurer((VkDeviceSize)w * h * 4) || image_creer(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1, 0, &img, &mem)) goto fin;
    barriere();
    VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, niveau, couche, 1 }, .srcOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } } };
    vkCmdBlitImage(CB, t->img, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    barriere();
    VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
    chrono_segment(CH_RELECTURE, -1);     /* passe 10s */
    vkCmdCopyImageToBuffer(CB, img, VK_IMAGE_LAYOUT_GENERAL, V.rb, 1, &bc);
    chrono_segment(CH_HORS_PASSE, -1);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(CB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (attendre_tout(AT_RELECTURE)) goto fin;   /* passe 10r (v13) : soumet le blit + copie, attend TOUTES les fences */
    memcpy(rgba, V.rbmap, (size_t)w * h * 4);
    r = 0;
fin:
    if (img) { vkDestroyImage(V.dev, img, NULL); vkFreeMemory(V.dev, mem, NULL); }
    pthread_mutex_unlock(&g_mu);
    return r;
}
int d3dv_bc_natif(void) { return V.bc; }

/* ---- samplers ----
 * Passe 7l : états D3D9 fidèles — adressage U/V/W (WRAP 1, MIRROR 2, CLAMP 3, BORDER 4, MIRRORONCE 5 →
 * clamp), couleur de bordure (la plus proche des 3 couleurs Vulkan de base), filtres mag/min (POINT 1,
 * LINEAR 2, ANISOTROPIC 3 → linéaire + anisotropie MAXANISOTROPY si le device l'a), MIPFILTER (NONE 0 :
 * le seul niveau MAXMIPLEVEL ; POINT 1 / LINEAR 2), MIPMAPLODBIAS (flottant), MAXMIPLEVEL (= minLod). */
static int bordure_vk(uint32_t argb)
{
    uint32_t a = argb >> 24, l = ((argb >> 16 & 255) + (argb >> 8 & 255) + (argb & 255)) / 3;
    return a < 128 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : l >= 128 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                   : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}
static VkSampler sampler_cmp(const uint32_t adr[3], uint32_t mag, uint32_t min, uint32_t mip, uint32_t biais,
                             uint32_t maxmip, uint32_t aniso, uint32_t bordure, int cmp);
static VkSampler sampler_pour(const uint32_t adr[3], uint32_t mag, uint32_t min, uint32_t mip, uint32_t biais,
                              uint32_t maxmip, uint32_t aniso, uint32_t bordure)
{ return sampler_cmp(adr, mag, min, mip, biais, maxmip, aniso, bordure, 0); }
/* Passe 7v2 : cmp = 1 → sampler à COMPARAISON (carte d'ombres D3D9, chemin « PCF matériel » des pilotes NVIDIA) :
 * compareEnable, compareOp LESS_OR_EQUAL — Vulkan rend (Dref <= profondeur lue), le D3D9 rend (z_réf <= z_carte) ;
 * heuristique : pas de CTAB dans les bytecodes de SC2, le sens est celui de toutes les cartes d'ombres D3D9 « D16 +
 * PCF » (lumière à z croissant). Filtre linéaire = PCF 2×2 comme le matériel. */
static VkSampler sampler_cmp(const uint32_t adr[3], uint32_t mag, uint32_t min, uint32_t mip, uint32_t biais,
                             uint32_t maxmip, uint32_t aniso, uint32_t bordure, int cmp)
{
    float b; memcpy(&b, &biais, 4);
    if (!(b == b)) b = 0.f;                                   /* NaN */
    if (b > V.biais_max) b = V.biais_max;
    if (b < -V.biais_max) b = -V.biais_max;
    int an = (mag == 3 || min == 3) && V.aniso_max > 1.f ? (int)(aniso < 1 ? 1 : aniso > 16 ? 16 : aniso) : 1;
    if (maxmip > 15) maxmip = 15;
    int bq = (int)(b * 16.f);
    uint64_t k = (uint64_t)(adr[0] & 7) | (uint64_t)(adr[1] & 7) << 3 | (uint64_t)(adr[2] & 7) << 6 | (uint64_t)(mag > 1) << 9
               | (uint64_t)(min > 1) << 10 | (uint64_t)(mip > 2 ? 2 : mip) << 11 | (uint64_t)maxmip << 13 | (uint64_t)an << 17
               | (uint64_t)bordure_vk(bordure) << 22 | (uint64_t)(uint8_t)(int8_t)bq << 25 | (uint64_t)(cmp != 0) << 33;
    for (int i = 0; i < R.nsmp; i++) if (R.smp[i].k == k) return R.smp[i].s;
    if (R.nsmp >= MAX_SAMPLERS) return R.smp[0].s;
    static const VkSamplerAddressMode m[8] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT };
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = mag > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                               .minFilter = min > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                               .mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = m[adr[0] & 7], .addressModeV = m[adr[1] & 7], .addressModeW = m[adr[2] & 7],
                               .mipLodBias = (float)bq / 16.f, .anisotropyEnable = an > 1, .maxAnisotropy = (float)an,
                               .minLod = (float)maxmip, .maxLod = mip ? VK_LOD_CLAMP_NONE : (float)maxmip,
                               .borderColor = (VkBorderColor)bordure_vk(bordure),
                               .compareEnable = cmp ? VK_TRUE : VK_FALSE,
                               .compareOp = cmp ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_NEVER };
    VkSampler s;
    if (vkCreateSampler(V.dev, &si, NULL, &s)) return R.nsmp ? R.smp[0].s : VK_NULL_HANDLE;
    R.smp[R.nsmp].k = k; R.smp[R.nsmp++].s = s;
    return s;
}

/* ---- pipelines ---- */
static VkBlendFactor facteur(uint32_t b)
{
    static const VkBlendFactor t[16] = { VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR };
    return b < 16 ? t[b] : VK_BLEND_FACTOR_ONE;
}
static VkBlendOp operation(uint32_t o)
{
    switch (o) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
                 case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}
/* SC2_VK_PROFONDEUR=0 : ni profondeur ni culling (comportement des passes 5d-7m, comparaison) */
static int profondeur_coupee(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SC2_VK_PROFONDEUR"); v = e && *e == '0'; }
    return v;
}
static VkPipeline pipeline5(const cle_pipe *k)
{
    int trouve = pipe_trouver(k);   /* passe 10r */
    if (trouve >= 0) return R.pipe[trouve].p;
    if (R.npipe >= MAX_PIPES) {
        static int dit;
        if (!dit++) LOG("[VK] plafond de %d pipelines atteint : dessins refusés au-delà", MAX_PIPES);
        return VK_NULL_HANDLE;
    }
    /* passe 7v2 : constantes de spécialisation profondeur_k (ID 100 + k, entier 0/1) du PS — sans effet sur un SPIR-V qui
     * ne les déclare pas (règle Vulkan) ; posées seulement sous SC2_OMBRES=1 */
    static VkSpecializationMapEntry me[16]; uint32_t sv[16];
    for (uint32_t i = 0; i < 16; i++) { me[i] = (VkSpecializationMapEntry){ 100 + i, 4 * i, 4 }; sv[i] = (k->prof >> i) & 1u; }
    VkSpecializationInfo spi = { 16, me, sizeof sv, sv };
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = R.mod[k->vs], .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = k->diag ? V.fs : R.mod[k->ps], .pName = "main", .pSpecializationInfo = d3dv_ombres() ? &spi : NULL } };
    VkVertexInputBindingDescription vb[16]; VkVertexInputAttributeDescription va[16];
    for (uint32_t i = 0; i < k->nflux; i++)   /* V2.2 : taux par instance de diviseur 1 = cœur de Vulkan 1.0 (aucune extension) */
        vb[i] = (VkVertexInputBindingDescription){ i, k->stride[i],
                    (k->inst >> i) & 1u ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX };
    for (uint32_t i = 0; i < k->nattr; i++) va[i] = (VkVertexInputAttributeDescription){ k->attr[i].loc, k->attr[i].flux, (VkFormat)k->attr[i].vkfmt, k->attr[i].offset };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = k->nflux, .pVertexBindingDescriptions = vb,
        .vertexAttributeDescriptionCount = k->nattr, .pVertexAttributeDescriptions = va };
    static const VkPrimitiveTopology topo[7] = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = topo[k->topo < 7 ? k->topo : 4] };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    /* Passe 7n : culling D3D9 (CULLMODE 1 NONE, 2 CW, 3 CCW = sens des triangles À L'ÉCRAN, rejetés). Le VS
     * traduit inverse y et le viewport Vulkan descend : le sens à l'écran est celui de D3D9. SC2_VK_SENS=1
     * inverse la convention (diagnostic). */
    static int sens = -1;
    if (sens < 0) { const char *e = getenv("SC2_VK_SENS"); sens = e && *e == '1'; }
    VkFrontFace ff = (k->cull == 2) != sens ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = k->cull == 2 || k->cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
        .frontFace = ff, .lineWidth = 1.f };
    /* Passe 7v3 : biais de profondeur D3D9 (passe d'ombre du jeu), seulement sous SC2_OMBRES=1 (clé nulle sinon).
     * DEPTHBIAS est en unités de profondeur [0, 1] ; Vulkan multiplie depthBiasConstantFactor par r, la plus petite
     * différence représentable : pour D32_SFLOAT, r = 2^(e - 23) — on prend z ∈ [0,5 ; 1[, soit r = 2^-24
     * (approximation ; exacte pour une profondeur 24 bits fixe). SLOPESCALEDEPTHBIAS = facteur de pente, tel quel. */
    if (k->zbiais || k->zpente) {
        float cb, pe; memcpy(&cb, &k->zbiais, 4); memcpy(&pe, &k->zpente, 4);
        rs.depthBiasEnable = VK_TRUE; rs.depthBiasConstantFactor = cb * 16777216.f; rs.depthBiasSlopeFactor = pe;
    }
    /* profondeur D3D9 : ZENABLE (≠ 0), ZWRITEENABLE (écrit seulement si le test est actif), ZFUNC D3DCMP 1..8 → VkCompareOp 0..7 */
    VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = k->df && k->zt, .depthWriteEnable = k->df && k->zt && k->zw,
        .depthCompareOp = (VkCompareOp)(k->zf >= 1 && k->zf <= 8 ? k->zf - 1 : VK_COMPARE_OP_LESS_OR_EQUAL) };
    /* 21/10 : stencil D3D9. Le code choisit ff pour que VK_CULL_MODE_BACK élimine ce que D3D9 élimine :
     * cull == 2 (CULL_CW) → les faces AVANT Vulkan sont les CCW de D3D9 → front = ops CCW_*, back = ops
     * normales ; sinon l'inverse. Sans TWOSIDEDSTENCILMODE, front = back = ops normales. D3DSTENCILOP
     * 1..8 → VkStencilOp = valeur − 1 ; compareMask/writeMask D3D9 sont sur 8 bits. */
    if (k->st_func) {                        /* clé non nulle = stencil actif (d3dv_dessiner ne la remplit que là) */
        VkStencilOpState ops = { .failOp = (VkStencilOp)(k->st_fail ? k->st_fail - 1 : VK_STENCIL_OP_KEEP),
                                 .passOp = (VkStencilOp)(k->st_pass ? k->st_pass - 1 : VK_STENCIL_OP_KEEP),
                                 .depthFailOp = (VkStencilOp)(k->st_zfail ? k->st_zfail - 1 : VK_STENCIL_OP_KEEP),
                                 .compareOp = (VkCompareOp)(k->st_func >= 1 && k->st_func <= 8 ? k->st_func - 1
                                                                                               : VK_COMPARE_OP_ALWAYS),
                                 .compareMask = k->st_masque & 0xFF, .writeMask = k->st_ecriture & 0xFF };
        VkStencilOpState ccw = { .failOp = (VkStencilOp)(k->st_ccw_fail ? k->st_ccw_fail - 1 : VK_STENCIL_OP_KEEP),
                                 .passOp = (VkStencilOp)(k->st_ccw_pass ? k->st_ccw_pass - 1 : VK_STENCIL_OP_KEEP),
                                 .depthFailOp = (VkStencilOp)(k->st_ccw_zfail ? k->st_ccw_zfail - 1
                                                                            : VK_STENCIL_OP_KEEP),
                                 .compareOp = (VkCompareOp)(k->st_ccw_func >= 1 && k->st_ccw_func <= 8
                                                            ? k->st_ccw_func - 1 : VK_COMPARE_OP_ALWAYS),
                                 .compareMask = ops.compareMask, .writeMask = ops.writeMask };
        dss.stencilTestEnable = VK_TRUE;
        if (k->st_ccw_func) {                /* deux faces : TWOSIDEDSTENCILMODE (la clé recopie ops normales sinon) */
            dss.front = k->cull == 2 ? ccw : ops;
            dss.back  = k->cull == 2 ? ops : ccw;
        } else dss.front = dss.back = ops;
    }
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .blendEnable = k->blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = facteur(k->src), .dstColorBlendFactor = facteur(k->dst), .colorBlendOp = operation(k->op),
        .srcAlphaBlendFactor = facteur(k->src_a), .dstAlphaBlendFactor = facteur(k->dst_a), .alphaBlendOp = operation(k->op_a),
        .colorWriteMask = k->masque & 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 3, .pDynamicStates = dyn };
    /* 21/10 : stencil D3D9 — l'attachement stencil existe ⇔ rendu_ouvrir en pose un (D32S8, même vue) ;
     * la cohérence stencilAttachmentFormat ⇔ pStencilAttachment est portée par k->sa (= V.rendu_st). */
    VkFormat sf = k->sa ? k->df : VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                          .pColorAttachmentFormats = &k->cf, .depthAttachmentFormat = k->df,
                                          .stencilAttachmentFormat = sf };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = R.layout };
    VkPipeline p;
    double t0 = maintenant_ms();   /* passe 10s (cache de pipelines) : compté pour [BILAN] [PIPECACHE] */
    VkResult r = vkCreateGraphicsPipelines(V.dev, PC.cache, 1, &gpi, NULL, &p);
    pcache_compter(!r, maintenant_ms() - t0);
    if (r) { LOG("[VK] pipeline réel refusé (VkResult %d)", (int)r); return VK_NULL_HANDLE; }
    R.pipe[R.npipe].k = *k; R.pipe[R.npipe++].p = p;
    pipe_noter(R.npipe - 1);
    if (R.npipe <= 8) LOG("[VK] pipeline réel n°%d : VS %d PS %d, %u attributs, %u flux, topologie %u, blend %u, inst 0x%X", R.npipe - 1,
                          k->vs, k->ps, k->nattr, k->nflux, k->topo, k->blend, k->inst);
    if (k->inst && ++R.n_pipes_inst <= 4)
        LOG("[VK] V2.2 : pipeline n°%d à flux d'instance 0x%X (VS %d, %u flux, %u attributs)", R.npipe - 1, k->inst,
            k->vs, k->nflux, k->nattr);
    return p;
}

int d3dv_dessiner(const d3dv_dessin *d)
{
    if (!V.actif) return -1;
    pthread_mutex_lock(&g_mu);
    int r = -1;
    if (!prets5()) goto fin;
    if (V.rt < 0 || d->vs < 0 || d->ps < 0 || d->vs >= R.nmod || d->ps >= R.nmod) { R.n_refus_cible++; goto fin; }
    cible *t = &V.c[V.rt];
    /* V2.2 : d->instances 0/1 et d->flux_inst 0 → clé, liaisons et appel d'avant, à l'octet */
    const uint32_t ninst = d->instances > 1 ? d->instances : 1;
    const uint32_t finst = d->nflux ? d->flux_inst & (0xFFFFFFFFu >> (32 - d->nflux)) : 0;
    /* passe 10r : tampons statiques d'abord — liaisons résolues AVANT tout soumettre()
     * éventuel (anneau ou pool de sets plein) : un VkBuffer en cache survit à une remise à
     * zéro de l'anneau, son contenu est immuable jusqu'à la prochaine régénération. */
    VkBuffer bflux[16], bindex = R.anneau; VkDeviceSize of[16] = { 0 }, oindex = 0;
    /* place dans l'anneau : tout ou rien (sinon on soumet d'abord) — passe 10r : n'y comptent
     * que les constantes et les flux/l'index réellement servis par l'anneau */
    VkDeviceSize besoin = 2 * (D3DV_CONST_TAILLE + 256) + 64;
    for (uint32_t i = 0; i < d->nflux; i++) {
        bflux[i] = R.anneau;
        int tr = tampon_puiser(d->flux[i].obj, d->flux[i].gen, d->flux[i].usage, d->flux[i].taille_tot,
                               d->flux[i].donnees_tot, d->flux[i].decalage, d->flux[i].taille, &bflux[i], &of[i]);
        if (tr == 1) { R.n_tam_flux++; R.o_evites += d->flux[i].taille; }   /* lié sans copie */
        else if (!tr) besoin += d->flux[i].taille + 64;
    }
    if (d->index) {
        int tr = tampon_puiser(d->index_obj, d->index_gen, d->index_usage, d->index_taille_tot, d->index_donnees_tot,
                               d->index_decalage, d->index_taille, &bindex, &oindex);
        if (tr == 1) { R.n_tam_index++; R.o_evites += d->index_taille; }
        else if (!tr) besoin += d->index_taille;
    }
    if (besoin > ANNEAU_TAILLE / 2) { R.n_refus_place++; goto fin; }
    if (R.pos + besoin > ANNEAU_FIN) { V.n_soum_anneau_dessin++; soumettre(); }   /* passe 10s : compté */
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!V.push_desc) {                      /* passe 7c : set classique ; pool de L'EMPLACEMENT plein → soumettre (10r v13) */
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                           .descriptorSetCount = 1, .pSetLayouts = &R.dsl };
        if (V.empl[V.cur].pool_en_cours >= POOL_SETS) { R.n_pool_plein++; soumettre(); }
        ai.descriptorPool = V.empl[V.cur].pool;   /* après un soumettre, l'emplacement (et son pool) ont changé */
        VkResult ra = vkAllocateDescriptorSets(V.dev, &ai, &set);
        if (ra && V.empl[V.cur].pool_en_cours) {
            R.n_pool_plein++; soumettre(); ai.descriptorPool = V.empl[V.cur].pool;
            ra = vkAllocateDescriptorSets(V.dev, &ai, &set);
        }
        if (ra) { R.n_refus_place++; goto fin; }
        V.empl[V.cur].pool_en_cours++; R.n_sets_alloues++;
        if (V.empl[V.cur].pool_en_cours > R.pool_max) R.pool_max = V.empl[V.cur].pool_en_cours;
    }
    cb_ouvrir();
    /* passe 10r : bloc inchangé depuis le dessin précédent (même génération assemblée) et
     * anneau intact (même époque) → emplacement réutilisé sans recopie ; la génération et
     * l'époque sont relues ici, APRÈS les soumettre() ci-dessus qui changent l'époque. */
    VkDeviceSize ovs, ops;
    /* passe 10t : seuls les registres lus par le shader sont recopiés (nconst) ; un bloc déjà dans l'anneau ne sert
     * que s'il en couvre assez (un dessin suivant de même génération peut lire plus loin : il recopie alors). */
    VkDeviceSize nvs = consts_a_copier(d->nconst_vs, consts_completes()), nps = consts_a_copier(d->nconst_ps, consts_completes());
    if (!sans_cache_consts() && cblk_reutilisable(0, d->gen_vs, nvs)) {
        ovs = R.cblk[0].off; R.n_cache_cvs++; R.o_evites += nvs;
    } else {
        ovs = anneau_partiel(d->cvs, nvs, D3DV_CONST_TAILLE, 256);
        R.cblk[0].gen = d->gen_vs; R.cblk[0].off = ovs; R.cblk[0].epoque = R.epoque; R.cblk[0].etendue = nvs;
        R.n_blocs++; R.n_blocs_entiers += nvs == D3DV_CONST_TAILLE; R.o_consts += nvs; R.o_plage += D3DV_CONST_TAILLE - nvs;
    }
    if (!sans_cache_consts() && cblk_reutilisable(1, d->gen_ps, nps)) {
        ops = R.cblk[1].off; R.n_cache_cps++; R.o_evites += nps;
    } else {
        ops = anneau_partiel(d->cps, nps, D3DV_CONST_TAILLE, 256);
        R.cblk[1].gen = d->gen_ps; R.cblk[1].off = ops; R.cblk[1].epoque = R.epoque; R.cblk[1].etendue = nps;
        R.n_blocs++; R.n_blocs_entiers += nps == D3DV_CONST_TAILLE; R.o_consts += nps; R.o_plage += D3DV_CONST_TAILLE - nps;
    }
    /* passe 10r : replis vers l'anneau — flux UP/constant (obj = 0), dynamiques, hors bornes,
     * plafond atteint (le marqueur bflux[i]/bindex est resté sur l'anneau). Passe 10t : les
     * replis IDENTIFIÉS (tampon invité, plage) ne sont recopiés qu'une fois par époque —
     * l'époque est relue ici, après les soumettre() ci-dessus qui l'avancent. */
    for (uint32_t i = 0; i < d->nflux; i++)
        if (bflux[i] == R.anneau)
            of[i] = volatils_puiser(d->flux[i].obj, d->flux[i].gen, d->flux[i].decalage, d->flux[i].taille,
                                    VOLATILS_FLUX, d->flux[i].donnees, d->flux[i].taille ? d->flux[i].taille : 16, 16);
    if (d->index && bindex == R.anneau)
        oindex = volatils_puiser(d->index_obj, d->index_gen, d->index_decalage, d->index_taille,
                                 VOLATILS_INDEX, d->index, d->index_taille, 4);
    /* textures : descripteurs */
    VkDescriptorImageInfo ii[16], iio[16]; VkWriteDescriptorSet w[34]; uint32_t nw = 0, prof = 0;
    VkDescriptorBufferInfo bvs = { R.anneau, ovs, D3DV_CONST_TAILLE }, bps = { R.anneau, ops, D3DV_CONST_TAILLE };
    w[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bvs };
    w[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bps };
    for (uint32_t k = 0; k < 16; k++) {
        int g = d->tex[k].genre_shader;
        if (!g) continue;
        int fi = g == 3 ? 1 : g == 4 ? 2 : 0;
        VkImageView vue = R.fvue[fi];
        if (d->tex[k].source == D3DV_TEX_TEXTURE && d->tex[k].id >= 0 && d->tex[k].id < R.nt && R.t[d->tex[k].id].genre == g
            && R.t[d->tex[k].id].gen != ~0u)
            vue = R.t[d->tex[k].id].vue;
        else if (d->tex[k].source == D3DV_TEX_CIBLE && g == 2 && d->tex[k].id >= 0 && d->tex[k].id < V.nc
                 && d->tex[k].id != V.rt && !V.c[d->tex[k].id].prof) {
            vue = V.c[d->tex[k].id].vue; V.c[d->tex[k].id].n_echant++;
        } else if (d->tex[k].source == D3DV_TEX_CIBLE && g == 2 && d->tex[k].id >= 0 && d->tex[k].id < V.nc
                   && V.c[d->tex[k].id].prof && d3dv_ombres() && d->tex[k].id != V.ds && V.c[d->tex[k].id].vue_prof) {
            /* passe 7s : carte d'ombres = profondeur BRUTE (aspect profondeur, sampler sans comparaison) ; le chemin
             * « PCF matériel » de D3D9 (comparaison dans le sampler) demanderait un échantillonnage Dref au shader */
            vue = V.c[d->tex[k].id].vue_prof; V.c[d->tex[k].id].n_echant++;
            prof |= 1u << k; R.n_ech_prof++; R.n_slots_prof++;   /* passe 7v2 : drapeau du sampler k */
        }
        ii[k] = (VkDescriptorImageInfo){ sampler_pour(d->tex[k].adresse, d->tex[k].mag, d->tex[k].min, d->tex[k].mip,
                                                      d->tex[k].biais, d->tex[k].maxmip, d->tex[k].aniso, d->tex[k].bordure),
                                         vue, VK_IMAGE_LAYOUT_GENERAL };
        w[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2 + k, .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[k] };
    }
    /* passe 7v2 : pour chaque sampler de profondeur, binding 18 + k = même vue + sampler à COMPARAISON */
    for (uint32_t k = 0; k < 16; k++) if (prof & (1u << k)) {
        iio[k] = (VkDescriptorImageInfo){ sampler_cmp(d->tex[k].adresse, d->tex[k].mag, d->tex[k].min, d->tex[k].mip,
                                                      d->tex[k].biais, d->tex[k].maxmip, d->tex[k].aniso, d->tex[k].bordure, 1),
                                          ii[k].imageView, VK_IMAGE_LAYOUT_GENERAL };
        w[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 18 + k, .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &iio[k] };
    }
    if (prof) {
        R.n_dessins_prof++;
        if (d->ps >= 0 && d->ps < MAX_MODULES && (R.prof_module[d->ps] | prof) != R.prof_module[d->ps]) {
            R.prof_module[d->ps] |= (uint16_t)prof;
            LOG("[OMBRES] module PS %d : samplers de PROFONDEUR 0x%04X (drapeaux profondeur_k = constantes de spécialisation 100 + k)",
                d->ps, R.prof_module[d->ps]);
        }
    }
    cle_pipe k; memset(&k, 0, sizeof k);
    k.prof = prof;
    k.vs = d->vs; k.ps = d->ps; k.topo = d->topo; k.cf = t->vf; k.nattr = d->nattr; k.nflux = d->nflux;
    if (d->topo == 6) R.n_fan++;             /* passe 7u : D3DPT_TRIANGLEFAN → VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN */
    for (uint32_t i = 0; i < d->nattr; i++) { k.attr[i].loc = d->attr[i].loc; k.attr[i].flux = d->attr[i].flux;
                                              k.attr[i].vkfmt = d->attr[i].vkfmt; k.attr[i].offset = d->attr[i].offset; }
    for (uint32_t i = 0; i < d->nflux; i++) k.stride[i] = d->flux[i].stride;
    k.inst = finst;                          /* V2.2 */
    k.blend = d->blend; k.src = d->src; k.dst = d->dst; k.op = d->op; k.src_a = d->src_a; k.dst_a = d->dst_a; k.op_a = d->op_a;
    k.masque = d->masque_couleur;
    /* passe 7n (diagnostic) : SC2_VK_PS_DIAG=<n° de cible> (ou « tout ») → PS magenta sur les draws de cette
     * cible : la géométrie seule, indépendamment de l'ombrage du jeu */
    static int diag_lu, diag_cible = -2;
    if (!diag_lu) { const char *e = getenv("SC2_VK_PS_DIAG"); diag_lu = 1;
        if (e && *e) { diag_cible = !strcmp(e, "tout") ? -1 : atoi(e); LOG("[VK] DIAGNOSTIC SC2_VK_PS_DIAG=%s : PS magenta", e); } }
    k.diag = diag_cible == -1 || diag_cible == V.rt;
    /* V2.2 (diagnostic) : SC2_INSTANCES_DIAG=1 → PS magenta OPAQUE (sans mélange) sur les seuls dessins instanciés : la
     * zone qu'ils couvrent, pour chiffrer la CAPTURE A/B SC2_INSTANCES=0/1 */
    static int diag_inst = -1;
    if (diag_inst < 0) { const char *e = getenv("SC2_INSTANCES_DIAG"); diag_inst = e && *e == '1';
                         if (diag_inst) LOG("[VK] DIAGNOSTIC SC2_INSTANCES_DIAG=1 : PS magenta opaque sur les dessins instanciés"); }
    if (diag_inst && (finst || ninst > 1)) { k.diag = 1; k.blend = 0; }
    if (k.diag) k.masque = 0xF;
    /* passe 7n : profondeur — attachée si une surface de profondeur couvre la cible et que le draw s'en sert ;
     * 21/10 : stencil — attachée aussi si le test stencil est actif ET que la cible porte un aspect stencil. */
    int st_actif = d->st_en && V.ds >= 0 && ds_attachable(V.rt, V.ds)
                   && (V.c[V.ds].aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    int ds = (((d->zt || d->zw) && !profondeur_coupee()) || st_actif) && ds_attachable(V.rt, V.ds) ? V.ds : -1;
    k.df = ds >= 0 ? V.c[ds].vf : VK_FORMAT_UNDEFINED;
    k.zt = d->zt; k.zw = d->zw; k.zf = d->zf; k.cull = profondeur_coupee() ? 1 : d->cull;
    if (d3dv_ombres() && ds >= 0 && (d->zbiais || d->zpente)) {      /* passe 7v3 : biais de profondeur (passe d'ombre) */
        k.zbiais = d->zbiais; k.zpente = d->zpente;
        static int n_b = 0;
        if (n_b < 16) {
            int deja = 0; for (int i = 0; i < R.npipe; i++) if (R.pipe[i].k.zbiais == k.zbiais && R.pipe[i].k.zpente == k.zpente) deja = 1;
            if (!deja) { float cb, pe; memcpy(&cb, &k.zbiais, 4); memcpy(&pe, &k.zpente, 4); n_b++;
                LOG("[OMBRES] biais de profondeur D3D9 : DEPTHBIAS %g, SLOPESCALEDEPTHBIAS %g (cible %d, profondeur n°%d) → depthBias Vulkan",
                    cb, pe, V.rt, ds); }
        }
    }
    /* 21/10 : stencil D3D9 — la clé ne vit que si le test est actif ET l'attachement stencil existe
     * (D32S8) ; la référence reste hors clé (dynamique). Sans deux faces, ccw_* recopient les ops
     * normales : front = back quel que soit le culling. */
    if (st_actif && ds >= 0) {
        k.st_func = d->st_func; k.st_fail = d->st_fail; k.st_zfail = d->st_zfail; k.st_pass = d->st_pass;
        k.st_masque = d->st_masque & 0xFF; k.st_ecriture = d->st_ecriture & 0xFF;
        if (d->st_2faces) { k.st_ccw_func = d->st_ccw_func; k.st_ccw_fail = d->st_ccw_fail;
                            k.st_ccw_zfail = d->st_ccw_zfail; k.st_ccw_pass = d->st_ccw_pass; }
        else { k.st_ccw_func = k.st_func; k.st_ccw_fail = k.st_fail;
               k.st_ccw_zfail = k.st_zfail; k.st_ccw_pass = k.st_pass; }
        R.n_stencil++;
        if (profondeur_coupee()) k.zt = k.zw = 0;   /* SC2_VK_PROFONDEUR=0 : le diagnostic ne réactive pas le test Z */
        static int dit = 0;
        if (!dit++) LOG("[VK] stencil : test actif — func %u, fail %u, zfail %u, pass %u, ref %u, masque 0x%X, "
                        "écriture 0x%X, deux faces %u (cible %d, profondeur n°%d)", d->st_func, d->st_fail,
                        d->st_zfail, d->st_pass, d->st_ref & 0xFF, d->st_masque & 0xFF, d->st_ecriture & 0xFF,
                        d->st_2faces, V.rt, ds);
    }
    /* 21/10 : stencil attaché si CE dessin le teste, ou si le rendu ouvert pour la même cible l'attache déjà */
    k.sa = ds >= 0 && (st_actif || (V.rendu == V.rt && V.rendu_ds == ds && V.rendu_st));
    VkPipeline p = pipeline5(&k);
    if (!p) { R.n_refus_pipe++; goto fin; }
    rendu_ouvrir_st(V.rt, ds, (int)k.sa);
    vkCmdBindPipeline(CB, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    if (st_actif && ds >= 0)                 /* 21/10 : référence stencil = état dynamique (hors clé) */
        vkCmdSetStencilReference(CB, VK_STENCIL_FACE_FRONT_AND_BACK, d->st_ref & 0xFF);
    if (V.push_desc) V.push_desc(CB, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, nw, w);
    else {
        /* Set classique : TOUS ses bindings doivent être valides (le pilote peut les lire au bind :
         * Lavapipe déréférence une vue nulle → SIGSEGV). Samplers non utilisés par les shaders :
         * texture factice blanche 2D. Les descripteurs poussés n'ont pas cette contrainte. */
        int ecrit[32] = { 0 };
        for (uint32_t i = 2; i < nw; i++) ecrit[w[i].dstBinding - 2] = 1;
        if (d3dv_ombres())                       /* passe 7v2 : bindings d'ombre 18 + k non utilisés → factice (jamais lu) */
            for (uint32_t k = 16; k < 32; k++) if (!ecrit[k]) {
                static const uint32_t wrap2[3] = { 1, 1, 1 };
                iio[k - 16] = (VkDescriptorImageInfo){ sampler_pour(wrap2, 1, 1, 0, 0, 0, 1, 0), R.fvue[0], VK_IMAGE_LAYOUT_GENERAL };
                w[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2 + k,
                                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                  .pImageInfo = &iio[k - 16] };
            }
        for (uint32_t k = 0; k < 16; k++) if (!ecrit[k]) {
            static const uint32_t wrap[3] = { 1, 1, 1 };
            ii[k] = (VkDescriptorImageInfo){ sampler_pour(wrap, 1, 1, 0, 0, 0, 1, 0), R.fvue[0], VK_IMAGE_LAYOUT_GENERAL };
            w[nw++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2 + k,
                                              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              .pImageInfo = &ii[k] };
        }
        for (uint32_t i = 0; i < nw; i++) w[i].dstSet = set;
        vkUpdateDescriptorSets(V.dev, nw, w, 0, NULL);
        vkCmdBindDescriptorSets(CB, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 1, &set, 0, NULL);
    }
    vkCmdPushConstants(CB, R.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, d->hp);
    vkCmdPushConstants(CB, R.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 16, 16, d->at);
    for (uint32_t i = 0; i < d->nflux; i++) vkCmdBindVertexBuffers(CB, i, 1, &bflux[i], &of[i]);   /* passe 10r : cache ou anneau */
    float vw = V.vp[2] ? (float)V.vp[2] : (float)t->w, vh = V.vp[3] ? (float)V.vp[3] : (float)t->h;
    VkViewport vp = { (float)V.vp[0], (float)V.vp[1], vw, vh, V.zmin, V.zmax > V.zmin ? V.zmax : 1.f };
    VkRect2D sc = { { 0, 0 }, { t->w, t->h } };
    vkCmdSetViewport(CB, 0, 1, &vp);
    vkCmdSetScissor(CB, 0, 1, &sc);
    if (d->index) {
        vkCmdBindIndexBuffer(CB, bindex, oindex, d->index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);   /* passe 10r */
        vkCmdDrawIndexed(CB, d->nombre, ninst, 0, d->decalage_sommet, 0);   /* V2.2 : instanceCount = N, firstInstance 0 */
    } else
        vkCmdDraw(CB, d->nombre, ninst, 0, 0);       /* ninst = 1 ici : dessin_reel n'instancie que les dessins indexés */
    R.n_reels++; t->n_draw++;
    if (finst || ninst > 1) { R.n_inst_dessins++; R.n_inst += ninst; if (ninst > R.inst_max) R.inst_max = ninst; }
    r = 0;
fin:
    pthread_mutex_unlock(&g_mu);
    return r;
}

void d3dv_compteurs_inst(uint64_t *dessins, uint64_t *instances, uint32_t *max, int *pipes, int *pipes_inst)
{
    pthread_mutex_lock(&g_mu);
    if (dessins) *dessins = R.n_inst_dessins;
    if (instances) *instances = R.n_inst;
    if (max) *max = R.inst_max;
    if (pipes) *pipes = R.npipe;
    if (pipes_inst) *pipes_inst = R.n_pipes_inst;
    pthread_mutex_unlock(&g_mu);
}
static void bilan5(void)
{
    /* passe 10r : caches du chemin de dessin (constantes inchangées, tampons statiques) ;
     * passe 10t : plages volatiles réutilisées dans l'anneau (une recopie par époque) ;
     * 21/10 : dédiés vivants = entrées en cache sans dalle (buf nul = tampon volatil sorti) */
    int n_dedies = 0;
    for (int i = 0; i < R.ntam; i++) if (R.tam[i].buf && R.tam[i].dalle < 0) n_dedies++;
    printf("[BILAN] [CACHE] passe 10r : blocs de constantes réutilisés sans recopie par %llu dessins VS + %llu PS ; "
           "tampons statiques : %llu flux + %llu index liés depuis le cache, %llu remplacements, %llu volatils (%llu dessins renvoyés à l'anneau), %u refus plafond, "
           "%llu octets de copie évités au total ; %d entrées en cache (%.1f Mio / %u Mio) ; "
           "volatils réutilisés : %llu flux + %llu index (%.1f Mio évités) ; "
           "21/10 : %d dalle(s) de %u Mio, %.1f Mio occupés, %d tampons dédiés%s%s%s%s\n",
           (unsigned long long)R.n_cache_cvs, (unsigned long long)R.n_cache_cps, (unsigned long long)R.n_tam_flux,
           (unsigned long long)R.n_tam_index, (unsigned long long)R.n_tam_remplaces, (unsigned long long)R.n_tam_volatils,
           (unsigned long long)R.n_tam_volatils_servis, R.n_tam_plafond,
           (unsigned long long)R.o_evites, R.ntam, R.o_tam / 1048576.0, TAMPONS_PLAFOND >> 20,
           (unsigned long long)R.n_volatils_flux, (unsigned long long)R.n_volatils_index, R.o_volatils / 1048576.0,
           R.ndalles, DALLE_TAILLE >> 20, R.o_dalles / 1048576.0, n_dedies,
           !dalles_actives() ? " ; dalles DÉSACTIVÉES (SC2_VK_DALLES=0)" : "",
           sans_cache_consts() ? " ; cache constantes DÉSACTIVÉ (SC2_VK_SANS_CACHE_CONSTS=1)" : "",
           sans_cache_tampons() ? " ; cache tampons désactivé (SC2_VK_CACHE_TAMPONS=0)" : "",
           sans_cache_volatils() ? " ; cache volatils désactivé (SC2_VK_CACHE_VOLATILS=0)" : "");
    printf("[BILAN] [VK] constantes (passe 10t) : %llu blocs recopiés dont %llu entiers, %.1f Mo recopiés, "
           "%.1f Mo épargnés par la plage lue (nconst)%s\n",
           (unsigned long long)R.n_blocs, (unsigned long long)R.n_blocs_entiers, R.o_consts / 1e6, R.o_plage / 1e6,
           consts_completes() ? " ; blocs entiers forcés (SC2_VK_CONSTS_COMPLETES=1)" : "");
    if (d3dv_ombres()) {
        int nm = 0; for (int i = 0; i < MAX_MODULES; i++) if (R.prof_module[i]) nm++;
        uint64_t ech_d32 = 0; for (int i = 0; i < V.nc; i++) if (V.c[i].prof) ech_d32 += V.c[i].n_echant;
        printf("[BILAN] [OMBRES] passe 7v2 : %llu draws avec ≥ 1 sampler de PROFONDEUR (drapeau posé, binding 18+k à comparaison "
               "LESS_OR_EQUAL), %llu liaisons de slot, %d module(s) PS marqué(s) ; échantillonnages des cibles de profondeur "
               "(Vulkan D32) : %llu\n", (unsigned long long)R.n_dessins_prof, (unsigned long long)R.n_slots_prof, nm,
               (unsigned long long)ech_d32);
        for (int i = 0; i < V.nc; i++) if (V.c[i].prof)
            printf("[BILAN] [OMBRES]   cible de profondeur n°%d %u×%u D3DFMT %u (VkFormat %d) : %llu échantillonnages\n", i,
                   V.c[i].w, V.c[i].h, V.c[i].fmt, (int)V.c[i].vf, (unsigned long long)V.c[i].n_echant);
    }
    printf("[BILAN] draws RÉELS (shaders du jeu en SPIR-V) : %llu ; refusés : %llu sans cible/module, %llu pipeline, %llu place ; "
           "%d modules, %d pipelines, %d samplers ; textures : %d, %llu téléversements (%llu Mo), %llu formats/tailles non gérés ; "
           "V5.1 : %llu textures et %llu tampons oubliés (objets rendus à l'arène), %d + %d indices libres, %llu tables refaites\n",
           (unsigned long long)R.n_reels, (unsigned long long)R.n_refus_cible, (unsigned long long)R.n_refus_pipe,
           (unsigned long long)R.n_refus_place, R.nmod, R.npipe, R.nsmp, R.nt, (unsigned long long)R.n_tex_tel,
           (unsigned long long)(R.o_tel >> 20), (unsigned long long)R.n_tex_ko, (unsigned long long)g_n_tex_oubliees,
           (unsigned long long)g_n_tam_oublies, g_ntex_libres, g_ntam_libres, (unsigned long long)g_n_hache_refaites);
    printf("[BILAN] [INST] backend V2.2 : %llu dessins à flux d'instance, Σ %llu instances (max %u) ; %d pipelines à flux d'instance\n",
           (unsigned long long)R.n_inst_dessins, (unsigned long long)R.n_inst, R.inst_max, R.n_pipes_inst);
    printf("[BILAN] [TEX] backend : BC %s ; %llu niveaux téléversés (%llu cubes) ; %llu téléversements DÉCODÉS PAR LE CPU (%llu Mo)\n",
           V.bc ? "natif" : V.sans_bc ? "logiciel (SC2_VK_SANS_BC=1)" : "logiciel (absent du device)",
           (unsigned long long)R.n_niv_tel, (unsigned long long)R.n_cubes_tel, (unsigned long long)R.n_tel_logiciel, (unsigned long long)(R.o_logiciel >> 20));
    for (int i = 0; i < g_ntfmt; i++) {
        uint32_t f = g_tfmt[i].fmt; char nf[24];
        if (f > 0x20202020u) { memcpy(nf, &f, 4); nf[4] = 0; } else snprintf(nf, sizeof nf, "D3DFMT %u", f);
        printf("[BILAN] [TEX]   %-10s → %s : %llu textures, %llu téléversements, %.1f Mo, jusqu'à %u niveaux\n", nf,
               g_tfmt[i].conv ? "RGBA8 (décodage CPU)" : "natif", (unsigned long long)g_tfmt[i].n_tex,
               (unsigned long long)g_tfmt[i].n_tel, g_tfmt[i].octets / 1048576.0, g_tfmt[i].nivmax);
    }
    /* passe 7u : MoltenVK (portability_subset triangleFans=0) refuse TRIANGLE_FAN ; une conversion fan → liste ne
     * se justifie que si ce compteur est > 0 (journal seul, aucun effet sur le rendu) */
#ifdef __APPLE__
    printf("[BILAN] draws D3DPT_TRIANGLEFAN (topologie 6) : %llu%s\n", (unsigned long long)R.n_fan,
           V.fans ? "" : R.n_fan ? " — NON supportés par le device (triangleFans=0) : conversion fan → liste REQUISE"
                               : " (triangleFans=0 : aucun draw concerné)");
#else
    printf("[BILAN] draws D3DPT_TRIANGLEFAN (topologie 6) : %llu\n", (unsigned long long)R.n_fan);
#endif
    if (R.pret > 0)
        printf("[BILAN] descripteurs %s", V.push_desc ? "poussés (VK_KHR_push_descriptor)\n" : "CLASSIQUES : ");
    if (R.pret > 0 && !V.push_desc)
        printf("%llu sets alloués, %llu libérés + %u en cours (= %llu), %llu remises à zéro du pool, %llu pool plein, "
               "max %u sets par soumission (pool %d, un par emplacement — passe 10r v13)\n", (unsigned long long)R.n_sets_alloues,
               (unsigned long long)R.n_sets_liberes, V.empl[V.cur].pool_en_cours,
               (unsigned long long)(R.n_sets_liberes + V.empl[V.cur].pool_en_cours), (unsigned long long)R.n_pool_raz,
               (unsigned long long)R.n_pool_plein, R.pool_max, POOL_SETS);
}
#endif   /* SC2_D3D9V_TABLE_VOLATILS : fin du backend complet (début sous l'en-tête) */
