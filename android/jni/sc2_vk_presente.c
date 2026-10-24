/* sc2_vk_presente.c — présentation GPU du back buffer par swapchain Vulkan (passe 7b).
 *
 * Remplace la copie CPU de la passe 7a (d3dv_lire → ANativeWindow_lock) quand elle est compilée
 * (SC2_SWAPCHAIN, option CMake) et active (SC2_SWAPCHAIN=1 dans l'environnement). Le backend
 * D3D9→Vulkan (recomp-sc2/runtime/sc2_d3d9v.c, hors de la zone de cette passe) n'est PAS modifié :
 * on s'insère entre lui et le chargeur Vulkan.
 *
 *   --wrap=dlsym       dlsym(lib, "vkGetInstanceProcAddr") rend notre gipa, qui interpose :
 *     vkCreateInstance   + VK_KHR_surface + VK_KHR_android_surface (téléphone) / VK_KHR_xcb_surface (banc, Xvfb)
 *     vkCreateDevice     + VK_KHR_swapchain ; une 2e file de la même famille si elle existe (file
 *                        PROPRE au présentateur : la file du backend est protégée par son verrou interne)
 *     vkGetDeviceProcAddr → vkCreateImage / vkDestroyImage observés
 *   --wrap=d3dv_cible  l'image créée pendant l'appel est celle de la cible rendue : id → VkImage
 *
 * Present : le backend a soumis la frame et attendu la file (vkQueueWaitIdle) ; toutes ses images
 * restent en VK_IMAGE_LAYOUT_GENERAL. On acquiert une image de swapchain, on l'efface (bandes
 * noires), on y copie le back buffer par vkCmdBlitImage (mise à l'échelle GPU, rapport conservé),
 * on présente (FIFO) et on attend notre barrière avant de rendre la main : le backend peut
 * réécrire le back buffer dès la frame suivante.
 */
#define _GNU_SOURCE
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#elif defined(__APPLE__)
/* V3, lot M5 (22/10 ; v3-critique.md §3.4, v3-conception-A.md §2.3) — branche macOS (MoltenVK). Chaque ajout est sous
 * __APPLE__ : « unifdef -U__APPLE__ » redonne le fichier d'Android et du banc Linux. Sur Mac, ce fichier est compilé
 * dans recomp-sc2 par mac/mac.cmake, avec des renommages au lieu des --wrap (ld64 n'en a pas).
 *   surface   CAMetalLayer (sc2v_fenetre(layer, w, h), app) par VK_EXT_metal_surface ; sans écran (sc2v_fenetre(NULL,
 *             w, h), banc SC2_SWAPCHAIN=1) par VK_EXT_headless_surface. Jamais la surface NSView de MoltenVK ni le
 *             minutage d'affichage : ses seuls chemins vers une attente synchrone du fil principal (v3-critique n° 1).
 *   défauts   SC2_PRESENT absente → mailbox (repli FIFO) ; SC2_BARRE absente → jamais (=1 la force).
 *   ajouts    sc2v_dladdr (journal [VK] bibliothèque juste malgré l'interposition), sc2v_visible (fenêtre occultée :
 *             présentation sautée), sc2v_presentees, sc2v_liberer_essai (sortie sans attente du verrou). Déclarés dans
 *             mac/sc2_mac.h. Pas de copie CPU sur Mac : une bascule fige l'image jusqu'au réarmement par la fenêtre. */
#define VK_USE_PLATFORM_METAL_EXT
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "sc2_vk_presente.h"
#include "sc2_geo.h"
#include "sc2_barre.h"         /* 21/10 : clavier de raccourcis dans la bande libre */
#include "sc2_trace_present.h"
#include "sc2_android.h"

void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static PFN_vkGetInstanceProcAddr r_gipa;
static PFN_vkGetDeviceProcAddr r_gdpa;
static PFN_vkCreateInstance r_CreateInstance;
static PFN_vkCreateDevice r_CreateDevice;
static PFN_vkCreateImage r_CreateImage;
static PFN_vkDestroyImage r_DestroyImage;

#define VKI(X) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VKD(X) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) \
    X(vkCmdBlitImage) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkResetCommandBuffer) X(vkCreateBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkDestroyBuffer) X(vkFreeMemory)   /* 21/10 : tampon de la barre */
#define DECL(f) static PFN_##f p_##f;
VKI(DECL) VKD(DECL)

static _Atomic uint64_t g_rect;           /* passe 7o : sw, sh, dw, dh du dernier blit (cf. sc2v_geometrie) */

static struct {
    int voulu, inst_ok, dev_ok;           /* SC2_SWAPCHAIN=1 ; extensions obtenues */
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev;
    uint32_t qf; VkQueue q; int file_propre;
    VkCommandPool pool; VkCommandBuffer cb; VkSemaphore acq, fin; VkFence fence;
    VkSemaphore blit_fin;                 /* passe 10r (v13) : blit fini → attendu par la prochaine soumission du backend */
    /* cibles du backend : id → image */
    struct { VkImage img; uint32_t w, h; VkFormat fmt; } c[256];
    VkImage derniere; VkFormat derniere_fmt; uint32_t derniere_w, derniere_h;
    /* fenêtre / surface / swapchain */
    void *fenetre; uint32_t fw, fh;       /* fw/fh : banc (surface sans taille propre) */
    int fenetre_changee;
    VkSurfaceKHR surf; VkSwapchainKHR sc; VkImage img[8]; uint32_t nimg; VkExtent2D ext; VkFormat sfmt;
    int a_refaire; const char *motif;      /* recréation demandée, et pourquoi */
    uint64_t presentes, echecs;
    uint64_t n_fence_ko;                  /* passe 9d : vkWaitForFences ≠ VK_SUCCESS */
    /* passe 7f : une swapchain PERSISTANTE — créations comptées par motif */
    uint64_t n_crees, n_ood, n_redim, n_subopt_ignores;
    atomic_int verifier_taille;            /* la plate-forme signale un changement de taille possible */
    VkSurfaceTransformFlagBitsKHR tr_cree;
    int controle;                         /* banc : relecture de l'image de swapchain (SC2_SWAPCHAIN_CONTROLE) */
    VkBuffer rb; VkDeviceMemory rbm; void *rbp; VkDeviceSize rbt;
    /* passe 7p3 : SC2_PRESENT_BANDES=1 — noir écrit dans les bandes seulement (tampon de pixels noirs opaques) */
    int bandes; VkBuffer zb; VkDeviceMemory zbm; VkDeviceSize zbt; uint64_t n_bandes;
    /* passe 8d : SC2_PRESENT_BANDES_TEMOIN=1 (banc, avec SC2_SWAPCHAIN_CONTROLE=1 seulement) — le tampon des bandes
     * est rempli de magenta au lieu de noir : témoin positif, le contrôle doit voir des bandes non noires */
    int temoin;
    /* passe 10a : bascule DÉFINITIVE sur la copie CPU (cf. basculer) ; fence soumise et non encore attendue ;
     * échecs consécutifs sans code négatif (OUT_OF_DATE, VK_TIMEOUT / VK_NOT_READY à l'acquisition) */
    int bascule; int bascule_code; const char *bascule_ou;
    int fence_soumise; unsigned echecs_suite; uint64_t n_acq_ko;
    /* passe 10o : la bascule n'est plus définitive — une NOUVELLE fenêtre (INIT_WINDOW après pliage, écran éteint…)
     * réarme la swapchain, au plus REARMEMENTS_MAX fois ; OUT_OF_DATE répétés tolérés OOD_TOLERANCE_S secondes */
    unsigned rearmements; double ood_t0;
} P;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* 21/10 : barre de raccourcis (sc2_barre.h). État partagé entre le fil de l'activité (touchers, choix tactile/physique)
 * et le fil du jeu (disposition et dessin à chaque Present), sous g_barre_mu. SC2_BARRE=0 : jamais ; =1 : toujours,
 * même avec souris ou clavier ; absente : affichée au toucher, masquée dès qu'une souris ou un clavier physique sert. */
static pthread_mutex_t g_barre_mu = PTHREAD_MUTEX_INITIALIZER;
static barre_etat g_be;
static barre_geo g_bgeo;                 /* disposition du dernier Present : image du jeu ET barre */
static int g_bvoulue = -1, g_bforce;     /* -1 : SC2_BARRE pas encore lue ; g_bforce -1 jamais, 1 toujours, 0 auto */
static uint64_t g_n_barre_appuis;
static struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize t; unsigned version; int32_t w, h; int bgr, pret; } B;
static int barre_voulue_lire(void)       /* sous g_barre_mu */
{
    if (g_bvoulue < 0) {
        const char *e = getenv("SC2_BARRE");
#ifdef __APPLE__
        g_bforce = e && *e == '1' ? 1 : -1;  /* M5 : pas de tactile sur Mac — absente (ou 0) = jamais, 1 = toujours */
#else
        g_bforce = e && *e == '0' ? -1 : e && *e == '1' ? 1 : 0;
#endif
        g_bvoulue = g_bforce >= 0;
    }
    return g_bvoulue;
}
#define ECHECS_SUITE_MAX 5                /* passe 10a : au-delà, bascule copie CPU (jamais de boucle sans fin) */
#define OOD_TOLERANCE_S 10.0              /* passe 10o : … et seulement si la série dure depuis au moins 10 s */
#define REARMEMENTS_MAX 8                 /* passe 10o : réarmements de la swapchain sur nouvelle fenêtre */

static double secondes(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Passe 10a : l'activité (sc2_android_main.c) repose la géométrie de la copie CPU après une bascule. Référence
 * faible : le banc android/sim ne la fournit pas. */
void sc2a_geo_invalider(void);
#pragma weak sc2a_geo_invalider

/* ---- interposition du chargeur ------------------------------------------------------------ */

static int ext_presente(const VkExtensionProperties *e, uint32_t n, const char *nom)
{
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, nom)) return 1;
    return 0;
}

#ifdef __ANDROID__
#define EXT_SURFACE_PLATEFORME VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
#elif defined(__APPLE__)
/* M5 : CAMetalLayer de la fenêtre (app) ; VK_EXT_headless_surface en plus si MoltenVK l'annonce (banc, sans écran). */
#define EXT_SURFACE_PLATEFORME VK_EXT_METAL_SURFACE_EXTENSION_NAME
#else
/* Banc Linux : le Lavapipe de Debian 12 (Mesa 22.3) n'a pas VK_EXT_headless_surface ; on présente
 * dans une vraie fenêtre X (Xvfb lancé par android/valider-7b-vk.sh) par VK_KHR_xcb_surface. libxcb
 * est ouverte par dlopen ; le sysroot arm64 n'a pas ses en-têtes : déclarations minimales. */
#define EXT_SURFACE_PLATEFORME "VK_KHR_xcb_surface"
typedef struct xcb_connection_t xcb_connection_t;
typedef struct { uint32_t root, default_colormap, white_pixel, black_pixel, current_input_masks;
                 uint16_t width_in_pixels, height_in_pixels, width_in_millimeters, height_in_millimeters,
                          min_installed_maps, max_installed_maps;
                 uint32_t root_visual; uint8_t backing_stores, save_unders, root_depth, allowed_depths_len; } xcb_screen_t;
typedef struct { xcb_screen_t *data; int rem, index; } xcb_screen_iterator_t;
typedef struct { unsigned int sequence; } xcb_void_cookie_t;
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; xcb_connection_t *connection; uint32_t window; }
        sc2_VkXcbSurfaceCreateInfoKHR;
typedef VkResult (VKAPI_PTR *sc2_PFN_vkCreateXcbSurfaceKHR)(VkInstance, const sc2_VkXcbSurfaceCreateInfoKHR *,
                                                              const VkAllocationCallbacks *, VkSurfaceKHR *);
#endif

#ifdef __ANDROID__
static PFN_vkCreateAndroidSurfaceKHR p_vkCreateAndroidSurfaceKHR;
#elif defined(__APPLE__)
static PFN_vkCreateMetalSurfaceEXT p_vkCreateMetalSurfaceEXT;          /* M5 : NULL si l'extension n'est pas annoncée */
static PFN_vkCreateHeadlessSurfaceEXT p_vkCreateHeadlessSurfaceEXT;
#else
static sc2_PFN_vkCreateXcbSurfaceKHR p_vkCreateXcbSurfaceKHR;
static xcb_connection_t *g_xcb; static uint32_t g_xwin;
#endif

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *al,
                                                       VkInstance *out)
{
    PFN_vkEnumerateInstanceExtensionProperties enumi =
        (PFN_vkEnumerateInstanceExtensionProperties)r_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    VkExtensionProperties e[128]; uint32_t n = 128;
    if (!enumi || enumi(NULL, &n, e) < 0) n = 0;
    const char *ext[64]; uint32_t k = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && k < 60; i++) ext[k++] = ci->ppEnabledExtensionNames[i];
#ifdef __APPLE__
    /* M5 : chacune ajoutée si elle est annoncée ; il en faut au moins une (Metal : la fenêtre ; sans écran : le banc) */
    int metal = ext_presente(e, n, VK_EXT_METAL_SURFACE_EXTENSION_NAME);
    int sans_ecran = ext_presente(e, n, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    int ok = ext_presente(e, n, VK_KHR_SURFACE_EXTENSION_NAME) && (metal || sans_ecran);
    if (!ok) {
        char l[1024] = ""; size_t o = 0;
        for (uint32_t i = 0; i < n && o < sizeof l - 80; i++) o += (size_t)snprintf(l + o, sizeof l - o, " %s", e[i].extensionName);
        sc2_log("[VKP] instance : %s, ou %s et %s, absentes — swapchain indisponible (annoncées :%s)",
                VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, l);
    }
    if (ok) {
        ext[k++] = VK_KHR_SURFACE_EXTENSION_NAME;
        if (metal) ext[k++] = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
        if (sans_ecran) ext[k++] = VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME;
    }
#else
    int ok = ext_presente(e, n, VK_KHR_SURFACE_EXTENSION_NAME) && ext_presente(e, n, EXT_SURFACE_PLATEFORME);
    if (!ok) {
        char l[1024] = ""; size_t o = 0;
        for (uint32_t i = 0; i < n && o < sizeof l - 80; i++) o += (size_t)snprintf(l + o, sizeof l - o, " %s", e[i].extensionName);
        sc2_log("[VKP] instance : %s et/ou %s absentes du chargeur — swapchain indisponible (annoncées :%s)",
                VK_KHR_SURFACE_EXTENSION_NAME, EXT_SURFACE_PLATEFORME, l);
    }
    if (ok) { ext[k++] = VK_KHR_SURFACE_EXTENSION_NAME; ext[k++] = EXT_SURFACE_PLATEFORME; }
#endif
    VkInstanceCreateInfo c2 = *ci; c2.enabledExtensionCount = k; c2.ppEnabledExtensionNames = ext;
    VkResult r = r_CreateInstance(&c2, al, out);
    if (r && ok) { sc2_log("[VKP] instance refusée avec les extensions de surface (%d) : sans swapchain", (int)r); ok = 0;
                   r = r_CreateInstance(ci, al, out); }
    if (!r && ok) {
        P.inst = *out; P.inst_ok = 1;
#define CHG_I(f) p_##f = (PFN_##f)r_gipa(P.inst, #f);
        VKI(CHG_I)
#ifdef __ANDROID__
        p_vkCreateAndroidSurfaceKHR = (PFN_vkCreateAndroidSurfaceKHR)r_gipa(P.inst, "vkCreateAndroidSurfaceKHR");
#elif defined(__APPLE__)
        if (metal) p_vkCreateMetalSurfaceEXT = (PFN_vkCreateMetalSurfaceEXT)r_gipa(P.inst, "vkCreateMetalSurfaceEXT");
        if (sans_ecran)
            p_vkCreateHeadlessSurfaceEXT = (PFN_vkCreateHeadlessSurfaceEXT)r_gipa(P.inst, "vkCreateHeadlessSurfaceEXT");
#else
        p_vkCreateXcbSurfaceKHR = (sc2_PFN_vkCreateXcbSurfaceKHR)r_gipa(P.inst, "vkCreateXcbSurfaceKHR");
#endif
#ifdef __APPLE__
        sc2_log("[VKP] instance : %s%s%s ajoutées", VK_KHR_SURFACE_EXTENSION_NAME,
                metal ? " + " VK_EXT_METAL_SURFACE_EXTENSION_NAME : "",
                sans_ecran ? " + " VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME : "");
#else
        sc2_log("[VKP] instance : %s + %s ajoutées", VK_KHR_SURFACE_EXTENSION_NAME, EXT_SURFACE_PLATEFORME);
#endif
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                     const VkAllocationCallbacks *al, VkDevice *out)
{
    if (!P.inst_ok || ci->queueCreateInfoCount != 1) return r_CreateDevice(pd, ci, al, out);
    VkExtensionProperties e[512]; uint32_t n = 512;
    if (p_vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) < 0) n = 0;
    if (!ext_presente(e, n, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        sc2_log("[VKP] device sans %s : présentation par copie CPU", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        return r_CreateDevice(pd, ci, al, out);
    }
    const char *ext[64]; uint32_t k = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && k < 63; i++) ext[k++] = ci->ppEnabledExtensionNames[i];
    ext[k++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceQueueCreateInfo q = ci->pQueueCreateInfos[0];
    VkQueueFamilyProperties qp[16]; uint32_t nq = 16;
    p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    static const float prio[2] = { 1.f, 1.f };
    int deux = q.queueCount == 1 && q.queueFamilyIndex < nq && qp[q.queueFamilyIndex].queueCount >= 2;
    if (deux) { q.queueCount = 2; q.pQueuePriorities = prio; }
    VkDeviceCreateInfo c2 = *ci; c2.enabledExtensionCount = k; c2.ppEnabledExtensionNames = ext; c2.pQueueCreateInfos = &q;
    VkResult r = r_CreateDevice(pd, &c2, al, out);
    if (r) { sc2_log("[VKP] device refusé avec %s (%d) : copie CPU", VK_KHR_SWAPCHAIN_EXTENSION_NAME, (int)r);
             return r_CreateDevice(pd, ci, al, out); }
    P.pd = pd; P.dev = *out; P.qf = q.queueFamilyIndex; P.file_propre = deux;
#define CHG_D(f) p_##f = (PFN_##f)r_gdpa(P.dev, #f);
    VKD(CHG_D)
    p_vkGetDeviceQueue(P.dev, P.qf, deux ? 1 : 0, &P.q);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = P.qf };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (p_vkCreateCommandPool(P.dev, &pci, NULL, &P.pool) || p_vkCreateSemaphore(P.dev, &sci, NULL, &P.acq) ||
        p_vkCreateSemaphore(P.dev, &sci, NULL, &P.fin) || p_vkCreateFence(P.dev, &fci, NULL, &P.fence) ||
        p_vkCreateSemaphore(P.dev, &sci, NULL, &P.blit_fin)) return r;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = P.pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (p_vkAllocateCommandBuffers(P.dev, &ai, &P.cb)) return r;
    P.dev_ok = 1;
    sc2_log("[VKP] device : %s ajoutée ; file de présentation %s (famille %u)", VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            deux ? "PROPRE (n°1)" : "PARTAGÉE avec le backend (famille à une seule file)", P.qf);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateImage(VkDevice d, const VkImageCreateInfo *ci,
                                                    const VkAllocationCallbacks *al, VkImage *out)
{
    VkResult r = r_CreateImage(d, ci, al, out);
    if (!r) { P.derniere = *out; P.derniere_fmt = ci->format; P.derniere_w = ci->extent.width; P.derniere_h = ci->extent.height; }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL m_DestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks *al)
{
    for (int i = 0; i < 256; i++) if (P.c[i].img == img) P.c[i].img = VK_NULL_HANDLE;
    r_DestroyImage(d, img, al);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL m_gdpa(VkDevice d, const char *nom)
{
    PFN_vkVoidFunction f = r_gdpa(d, nom);
    if (!f || d != P.dev || !P.dev_ok) return f;
    if (!strcmp(nom, "vkCreateImage")) { r_CreateImage = (PFN_vkCreateImage)f; return (PFN_vkVoidFunction)m_CreateImage; }
    if (!strcmp(nom, "vkDestroyImage")) { r_DestroyImage = (PFN_vkDestroyImage)f; return (PFN_vkVoidFunction)m_DestroyImage; }
    return f;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL m_gipa(VkInstance inst, const char *nom)
{
    PFN_vkVoidFunction f = r_gipa(inst, nom);
    if (!f) return f;
    if (!strcmp(nom, "vkCreateInstance")) { r_CreateInstance = (PFN_vkCreateInstance)f; return (PFN_vkVoidFunction)m_CreateInstance; }
    if (!strcmp(nom, "vkCreateDevice")) { r_CreateDevice = (PFN_vkCreateDevice)f; return (PFN_vkVoidFunction)m_CreateDevice; }
    if (!strcmp(nom, "vkGetDeviceProcAddr")) { r_gdpa = (PFN_vkGetDeviceProcAddr)f; return (PFN_vkVoidFunction)m_gdpa; }
    return f;
}
#ifdef __APPLE__

/* M5 (point 10) : le backend appelle dladdr(gipa) pour journaliser la bibliothèque qui le fournit (-Ddladdr=sc2v_dladdr,
 * mac/mac.cmake). Interposé, gipa vaut m_gipa, qui est dans l'exécutable : on rend la bibliothèque du vrai gipa. */
int sc2v_dladdr(const void *a, Dl_info *i)
{
    if (r_gipa && a == (const void *)m_gipa) a = (const void *)r_gipa;
    return dladdr(a, i);
}
#endif

void *__real_dlsym(void *h, const char *nom);
void *__wrap_dlsym(void *h, const char *nom)
{
    void *f = __real_dlsym(h, nom);
    if (!f || !nom || strcmp(nom, "vkGetInstanceProcAddr")) return f;
    const char *e = getenv("SC2_SWAPCHAIN");
    P.voulu = e && *e == '1';
    const char *k = getenv("SC2_SWAPCHAIN_CONTROLE");
    P.controle = k && *k == '1';
    const char *b = getenv("SC2_PRESENT_BANDES");
    P.bandes = b && *b == '1';
    const char *t = getenv("SC2_PRESENT_BANDES_TEMOIN");
    P.temoin = P.controle && P.bandes && t && *t == '1';
    if (!P.voulu) return f;
    r_gipa = (PFN_vkGetInstanceProcAddr)f;
    sc2_log("[VKP] SC2_SWAPCHAIN=1 : présentation GPU par swapchain (le chargeur Vulkan est interposé)");
    return (void *)m_gipa;
}

int __real_d3dv_cible(uint64_t cle, uint32_t w, uint32_t h, uint32_t fmt, int profondeur);
int __wrap_d3dv_cible(uint64_t cle, uint32_t w, uint32_t h, uint32_t fmt, int profondeur)
{
    P.derniere = VK_NULL_HANDLE;
    int id = __real_d3dv_cible(cle, w, h, fmt, profondeur);
    /* une texture créée par un autre fil entre-temps n'aurait pas les dimensions de la cible */
    if (id >= 0 && id < 256 && !profondeur && P.derniere && P.derniere_w == w && P.derniere_h == h) {
        P.c[id].img = P.derniere; P.c[id].w = w; P.c[id].h = h; P.c[id].fmt = P.derniere_fmt;
    }
    sc2a_cible_vue(id, w, h, profondeur);              /* passe 9d : relecture CPU bornée (sc2_android_hooks.c) */
    return id;
}

/* ---- fenêtre, surface, swapchain ----------------------------------------------------------- */

int sc2v_actif(void) { return P.voulu && P.inst_ok && P.dev_ok && !P.bascule; }

/* Nos soumissions terminées avant de détruire swapchain ou surface. Passe 10a : la file PARTAGÉE avec le backend
 * n'est plus attendue par vkQueueWaitIdle — appelé depuis le fil de l'activité (TERM_WINDOW), ce serait un accès
 * concurrent à la file du backend (synchronisation externe exigée par Vulkan) ; on attend notre fence. */
static void attendre_nos_soumissions(void)   /* sous g_mu */
{
    if (P.file_propre) { if (P.q) p_vkQueueWaitIdle(P.q); }
    else if (P.fence_soumise) p_vkWaitForFences(P.dev, 1, &P.fence, VK_TRUE, 2000000000ull);
}
static void swapchain_liberer(void)      /* sous g_mu */
{
    attendre_nos_soumissions();
    if (P.sc) { p_vkDestroySwapchainKHR(P.dev, P.sc, NULL); P.sc = VK_NULL_HANDLE; }
    P.nimg = 0;
}
static void surface_liberer(void)        /* sous g_mu */
{
    swapchain_liberer();
    if (P.surf) { p_vkDestroySurfaceKHR(P.inst, P.surf, NULL); P.surf = VK_NULL_HANDLE; }
}

/* Passe 10a — bascule définitive sur la copie CPU (sous g_mu, fil du jeu). Un échec de présentation ne doit jamais
 * arrêter le run ni le laisser sans image : swapchain et surface sont détruites (la surface Vulkan tient la fenêtre :
 * tant qu'elle existe, ANativeWindow_lock est refusé), l'activité repose la géométrie du tampon CPU, et sc2v_actif()
 * devient faux pour le reste du run — l'appelant (__wrap_d3dv_present) copie par CPU dès cette frame. */
static void basculer(const char *ou, int code)
{
    if (P.bascule) return;
    P.bascule = 1; P.bascule_code = code; P.bascule_ou = ou;
    sc2_log("[VKP] bascule copie CPU (code %d) : %s — swapchain et surface libérées, la copie CPU présente le reste du run "
            "(%llu frames présentées par la swapchain avant)", code, ou, (unsigned long long)P.presentes);
    surface_liberer();
    if (sc2a_geo_invalider) sc2a_geo_invalider();
}
/* Échec sans code négatif (OUT_OF_DATE, acquisition en VK_TIMEOUT / VK_NOT_READY) : toléré, mais borné. Passe 10o :
 * borné en nombre ET en durée — à 4 images/s, 5 frames d'OUT_OF_DATE ne font que 1,25 s d'animation de pliage. */
static void echec_tolere(const char *ou, int code)
{
    double t = secondes();
    if (!P.echecs_suite++) P.ood_t0 = t;
    if (P.echecs_suite >= ECHECS_SUITE_MAX && t - P.ood_t0 >= OOD_TOLERANCE_S) basculer(ou, code);
}

/* Fil de l'activité (APP_CMD_WINDOW_RESIZED / APP_CMD_CONFIG_CHANGED) : sans verrou ; le présentateur
 * compare la taille de la surface à celle de la swapchain au prochain Present (certains pilotes ne
 * rendent ni OUT_OF_DATE ni SUBOPTIMAL sur un redimensionnement — Lavapipe/X11 du banc, vérifié). */
void sc2v_taille_changee(void) { atomic_store(&P.verifier_taille, 1); }

void sc2v_fenetre(void *fenetre, uint32_t w, uint32_t h)
{
    pthread_mutex_lock(&g_mu);
    if (P.inst_ok && P.dev_ok) surface_liberer();       /* l'ancienne fenêtre n'est plus utilisable */
    /* passe 10o : une nouvelle fenêtre n'a jamais été verrouillée par la copie CPU — elle peut reservir à Vulkan */
    if (fenetre && P.bascule && P.rearmements < REARMEMENTS_MAX) {
        P.rearmements++;
        sc2_log("[VKP] nouvelle fenêtre : swapchain RÉARMÉE après la bascule « %s » (code %d) — réarmement n°%u/%d",
                P.bascule_ou ? P.bascule_ou : "?", P.bascule_code, P.rearmements, REARMEMENTS_MAX);
        P.bascule = 0; P.echecs_suite = 0; P.a_refaire = 1; P.motif = "réarmement après bascule";
    }
    P.fenetre = fenetre; P.fw = w; P.fh = h; P.fenetre_changee = 1;
    pthread_mutex_unlock(&g_mu);
}
#ifdef __APPLE__

/* M5 — sortie (gestionnaire atexit de l'hôte, mac/sc2_mac_hote.c) : comme sc2v_fenetre(NULL, 0, 0), mais SANS attendre
 * le verrou. Une faute dans la présentation laisse g_mu tenu par le fil fautif, qui exécute lui-même les gestionnaires
 * de sortie (sc2_stop → exit) : l'attendre bloquerait la sortie. 0 = libérées ; -1 = verrou tenu, rien n'est touché. */
int sc2v_liberer_essai(void)
{
    if (pthread_mutex_trylock(&g_mu)) return -1;
    if (P.inst_ok && P.dev_ok) surface_liberer();
    P.fenetre = NULL; P.fw = P.fh = 0; P.fenetre_changee = 1;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

/* M5 — fenêtre occultée (fil principal, M7) : 0 → la présentation est sautée au Present suivant (sémaphore « rendu »
 * consommé, ni échec ni bascule ; nextDrawable d'un calque invisible peut attendre). Atomique, sans verrou. */
static atomic_int g_visible = 1;
static unsigned g_n_sautees, g_serie_sautees;           /* fil du jeu, sous g_mu */
void sc2v_visible(int visible) { atomic_store(&g_visible, !!visible); }

/* M5 — frames réellement présentées par la swapchain (fil du jeu) : l'hôte n'arme sa sortie qu'après la 1re. */
uint64_t sc2v_presentees(void) { return P.presentes; }
#endif

static int surface_creer(void)           /* sous g_mu */
{
    VkResult r;
#ifdef __ANDROID__
    VkAndroidSurfaceCreateInfoKHR si = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                         .window = (ANativeWindow *)P.fenetre };
    r = p_vkCreateAndroidSurfaceKHR ? p_vkCreateAndroidSurfaceKHR(P.inst, &si, NULL, &P.surf) : VK_ERROR_EXTENSION_NOT_PRESENT;
#elif defined(__APPLE__)
    /* M5 (point 5) : CAMetalLayer (fil du jeu : MoltenVK n'appelle le fil principal que pour une NSView) ; sinon surface
     * sans écran à P.fw × P.fh (currentExtent indéfini : la taille vient de sc2v_fenetre). */
    const char *genre;
    if (P.fenetre) {
        VkMetalSurfaceCreateInfoEXT si = { .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                                           .pLayer = (const CAMetalLayer *)P.fenetre };
        r = p_vkCreateMetalSurfaceEXT ? p_vkCreateMetalSurfaceEXT(P.inst, &si, NULL, &P.surf) : VK_ERROR_EXTENSION_NOT_PRESENT;
        genre = "Metal (CAMetalLayer)";
    } else {
        VkHeadlessSurfaceCreateInfoEXT si = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
        r = p_vkCreateHeadlessSurfaceEXT ? p_vkCreateHeadlessSurfaceEXT(P.inst, &si, NULL, &P.surf)
                                         : VK_ERROR_EXTENSION_NOT_PRESENT;
        genre = "sans écran (VK_EXT_headless_surface)";
    }
    if (!r) sc2_log("[VKP] surface %s créée (%ux%u selon sc2v_fenetre)", genre, P.fw, P.fh);
#else
    if (!g_xcb) {                                         /* fenêtre X de P.fw × P.fh sur $DISPLAY */
        void *x = dlopen("libxcb.so.1", RTLD_NOW);
        xcb_connection_t *(*conn)(const char *, int *) = x ? dlsym(x, "xcb_connect") : NULL;
        if (!conn) { sc2_log("[VKP] libxcb.so.1 introuvable"); return -1; }
        const void *(*setup)(xcb_connection_t *) = dlsym(x, "xcb_get_setup");
        xcb_screen_iterator_t (*racines)(const void *) = dlsym(x, "xcb_setup_roots_iterator");
        uint32_t (*nid)(xcb_connection_t *) = dlsym(x, "xcb_generate_id");
        xcb_void_cookie_t (*creer)(xcb_connection_t *, uint8_t, uint32_t, uint32_t, int16_t, int16_t, uint16_t, uint16_t,
                                   uint16_t, uint16_t, uint32_t, uint32_t, const void *) = dlsym(x, "xcb_create_window");
        xcb_void_cookie_t (*montrer)(xcb_connection_t *, uint32_t) = dlsym(x, "xcb_map_window");
        int (*vider)(xcb_connection_t *) = dlsym(x, "xcb_flush");
        int (*erreur)(xcb_connection_t *) = dlsym(x, "xcb_connection_has_error");
        int ecran = 0;
        xcb_connection_t *c = conn(NULL, &ecran);
        if (!c || erreur(c)) { sc2_log("[VKP] xcb_connect(DISPLAY=%s) : échec", getenv("DISPLAY") ? getenv("DISPLAY") : "(vide)"); return -1; }
        xcb_screen_t *sc = racines(setup(c)).data;
        g_xwin = nid(c);
        creer(c, 0, g_xwin, sc->root, 0, 0, (uint16_t)P.fw, (uint16_t)P.fh, 0, 1, sc->root_visual, 0, NULL);
        montrer(c, g_xwin); vider(c);
        g_xcb = c;
        sc2_log("[VKP] banc : fenêtre X 0x%X de %ux%u sur DISPLAY=%s (écran %ux%u)", g_xwin, P.fw, P.fh,
                getenv("DISPLAY"), sc->width_in_pixels, sc->height_in_pixels);
    }
    sc2_VkXcbSurfaceCreateInfoKHR si = { .sType = (VkStructureType)1000005000 /* XCB_SURFACE_CREATE_INFO_KHR */,
                                         .connection = g_xcb, .window = g_xwin };
    r = p_vkCreateXcbSurfaceKHR ? p_vkCreateXcbSurfaceKHR(P.inst, &si, NULL, &P.surf) : VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
    if (r) {
        sc2_log("[VKP] création de la surface : échec %d", (int)r); P.surf = VK_NULL_HANDLE;
        if (r < 0) basculer("création de la surface", (int)r);              /* passe 10a */
        return -1;
    }
    VkBool32 ok = 0;
    VkResult rs = p_vkGetPhysicalDeviceSurfaceSupportKHR(P.pd, P.qf, P.surf, &ok);
    if (!ok) {
        sc2_log("[VKP] la famille %u ne peut pas présenter sur cette surface", P.qf); surface_liberer();
        basculer("famille de file sans présentation sur la surface", (int)rs);   /* passe 10a : permanent */
        return -1;
    }
    return 0;
}

/* Passe 7o (perf Fold) : SC2_PRESENT=fifo (défaut : vsync, seul mode garanti par Vulkan, cadence de
 * l'écran) | mailbox (latence basse, pas de déchirure, si supporté) | immediate | fifo_relaxed. Un mode non
 * supporté par la surface retombe sur FIFO ; le choix est journalisé une fois. */
#ifdef __APPLE__
static const char *mode_nom(VkPresentModeKHR m)          /* M5 (point 9) : le journal donne le mode réel */
{
    return m == VK_PRESENT_MODE_FIFO_KHR ? "fifo" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
         : m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "fifo_relaxed" : "?";
}
#endif
static VkPresentModeKHR mode_presentation(void)
{
    static const struct { const char *nom; VkPresentModeKHR m; } N[] = {
        { "fifo", VK_PRESENT_MODE_FIFO_KHR }, { "mailbox", VK_PRESENT_MODE_MAILBOX_KHR },
        { "immediate", VK_PRESENT_MODE_IMMEDIATE_KHR }, { "fifo_relaxed", VK_PRESENT_MODE_FIFO_RELAXED_KHR } };
    const char *e = getenv("SC2_PRESENT");
    VkPresentModeKHR voulu = VK_PRESENT_MODE_FIFO_KHR, m[16]; uint32_t nm = 16;
#ifdef __APPLE__
    if (!e) voulu = VK_PRESENT_MODE_MAILBOX_KHR;           /* M5 (point 7) : défaut mailbox sur Mac, repli FIFO plus bas */
#endif
    for (unsigned i = 0; e && i < sizeof N / sizeof N[0]; i++) if (!strcmp(e, N[i].nom)) voulu = N[i].m;
    if (!p_vkGetPhysicalDeviceSurfacePresentModesKHR || p_vkGetPhysicalDeviceSurfacePresentModesKHR(P.pd, P.surf, &nm, m)) nm = 0;
    int ok = voulu == VK_PRESENT_MODE_FIFO_KHR;
    char liste[96] = ""; size_t n = 0;
    for (uint32_t i = 0; i < nm; i++) {
        ok |= m[i] == voulu;
        const char *nom = "?";
        for (unsigned k = 0; k < sizeof N / sizeof N[0]; k++) if (N[k].m == m[i]) nom = N[k].nom;
        n += (size_t)snprintf(liste + n, n < sizeof liste ? sizeof liste - n : 0, "%s%s", i ? "," : "", nom);
        if (n >= sizeof liste) n = sizeof liste - 1;
    }
    VkPresentModeKHR r = ok ? voulu : VK_PRESENT_MODE_FIFO_KHR;
    static int dit;
#ifdef __APPLE__
    static char dite[96];                           /* M5 : journalisé à chaque nouvelle liste (surface Metal ≠ sans écran) */
    if (!dit++ || strcmp(dite, liste)) {
        snprintf(dite, sizeof dite, "%s", liste);
        sc2_log("[VKP] mode de présentation %s (SC2_PRESENT=%s%s ; supportés : %s)", mode_nom(r), e ? e : "(défaut mailbox)",
                r != voulu ? " : non supporté, repli fifo" : "", nm ? liste : "?");
    }
#else
    if (!dit++)
        sc2_log("[VKP] mode de présentation %s (SC2_PRESENT=%s ; supportés : %s)",
                r == VK_PRESENT_MODE_FIFO_KHR ? "fifo" : r == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                : r == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo_relaxed", e ? e : "(défaut fifo)", nm ? liste : "?");
#endif
    return r;
}

static int swapchain_creer(void)         /* sous g_mu */
{
    VkSurfaceCapabilitiesKHR cap;
    VkResult rc = p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(P.pd, P.surf, &cap);
    if (rc) { if (rc < 0) basculer("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", (int)rc); return -1; }   /* passe 10a */
    VkExtent2D e = cap.currentExtent;
    if (e.width == 0xFFFFFFFFu) { e.width = P.fw ? P.fw : 1280; e.height = P.fh ? P.fh : 720; }
    if (!e.width || !e.height) return -1;                 /* fenêtre réduite : passager, pas de bascule */
    VkSurfaceFormatKHR f[32]; uint32_t nf = 32;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(P.pd, P.surf, &nf, f);
    VkSurfaceFormatKHR choix = nf ? f[0] : (VkSurfaceFormatKHR){ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    for (uint32_t i = 0; i < nf; i++)                    /* UNORM : le back buffer D3D9 n'est pas sRGB */
        if (f[i].format == VK_FORMAT_R8G8B8A8_UNORM || f[i].format == VK_FORMAT_B8G8R8A8_UNORM) { choix = f[i]; break; }
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (P.controle && (cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        sc2_log("[VKP] swapchain sans TRANSFER_DST"); basculer("surface sans VK_IMAGE_USAGE_TRANSFER_DST", 0); return -1;
    }
    uint32_t n = cap.minImageCount + 1;
    if (cap.maxImageCount && n > cap.maxImageCount) n = cap.maxImageCount;
    VkCompositeAlphaFlagBitsKHR ca = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(cap.supportedCompositeAlpha & ca)) ca = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    /* Rotation laissée au compositeur (IDENTITY) : l'image n'a pas à être tournée par nous. */
    VkSurfaceTransformFlagBitsKHR tr = (cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                       ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : cap.currentTransform;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = P.surf,
        .minImageCount = n, .imageFormat = choix.format, .imageColorSpace = choix.colorSpace, .imageExtent = e,
        .imageArrayLayers = 1, .imageUsage = u, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = tr,
        .compositeAlpha = ca, .presentMode = mode_presentation(), .clipped = VK_TRUE, .oldSwapchain = P.sc };
    VkSwapchainKHR sc;
    VkResult r = p_vkCreateSwapchainKHR(P.dev, &ci, NULL, &sc);
    if (P.sc) { p_vkQueueWaitIdle(P.q); p_vkDestroySwapchainKHR(P.dev, P.sc, NULL); }   /* ancienne : plus utilisée */
    P.sc = VK_NULL_HANDLE;
    if (r) {
        sc2_log("[VKP] vkCreateSwapchainKHR : échec %d", (int)r);
        if (r < 0) basculer("vkCreateSwapchainKHR", (int)r);              /* passe 10a : plus de recréation à chaque frame */
        return -1;
    }
    P.sc = sc; P.ext = e; P.sfmt = choix.format;
    P.nimg = 8; p_vkGetSwapchainImagesKHR(P.dev, P.sc, &P.nimg, P.img);
    P.a_refaire = 0; P.tr_cree = tr;
    P.n_crees++;
#ifdef __APPLE__
    /* M5 (point 9) : le mode réel (Android garde son texte : les fixtures de verdict-fold lisent « FIFO ») ; currentExtent
     * brut (0xFFFFFFFF : taille indéfinie, celle de sc2v_fenetre est prise) */
    sc2_log("[VKP] swapchain %ux%u, format %d, %u images, %s — création n°%llu (%s ; currentExtent 0x%X×0x%X ; rotation "
            "courante 0x%X, preTransform 0x%X ; usages supportés 0x%X, demandés 0x%X ; compositeAlpha supportés 0x%X, "
            "choisi 0x%X)", e.width, e.height, (int)choix.format, P.nimg, mode_nom(ci.presentMode),
            (unsigned long long)P.n_crees, P.motif ? P.motif : "initiale", cap.currentExtent.width, cap.currentExtent.height,
            (unsigned)cap.currentTransform, (unsigned)tr, (unsigned)cap.supportedUsageFlags, (unsigned)u,
            (unsigned)cap.supportedCompositeAlpha, (unsigned)ca);
#else
    sc2_log("[VKP] swapchain %ux%u, format %d, %u images, FIFO — création n°%llu (%s ; rotation courante 0x%X, preTransform 0x%X ; "
            "usages supportés 0x%X, demandés 0x%X ; compositeAlpha supportés 0x%X, choisi 0x%X)",
            e.width, e.height, (int)choix.format, P.nimg, (unsigned long long)P.n_crees, P.motif ? P.motif : "initiale",
            (unsigned)cap.currentTransform, (unsigned)tr, (unsigned)cap.supportedUsageFlags, (unsigned)u,
            (unsigned)cap.supportedCompositeAlpha, (unsigned)ca);
#endif
    P.motif = NULL;
    return 0;
}

/* ---- présentation -------------------------------------------------------------------------- */

static void barriere_img(VkImage img, VkImageLayout de, VkImageLayout vers, VkAccessFlags sa, VkAccessFlags da)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da,
        .oldLayout = de, .newLayout = vers, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    p_vkCmdPipelineBarrier(P.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Tampon visible de l'hôte (cohérent), lié et projeté. */
static int tampon_hote(VkDeviceSize taille, VkBufferUsageFlags usage, VkBuffer *b, VkDeviceMemory *m, void **ptr)
{
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = taille, .usage = usage };
    if (p_vkCreateBuffer(P.dev, &bi, NULL, b)) return -1;
    VkMemoryRequirements mr; p_vkGetBufferMemoryRequirements(P.dev, *b, &mr);
    VkPhysicalDeviceMemoryProperties mp; p_vkGetPhysicalDeviceMemoryProperties(P.pd, &mp);
    uint32_t t = 0;
    for (; t < mp.memoryTypeCount; t++)
        if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
    if (t == mp.memoryTypeCount || p_vkAllocateMemory(P.dev, &ai, NULL, m)) return -1;
    p_vkBindBufferMemory(P.dev, *b, *m, 0);
    return p_vkMapMemory(P.dev, *m, 0, taille, 0, ptr) ? -1 : 0;
}

/* Passe 7p3 : tampon de pixels noirs opaques (0, 0, 0, 255 : la valeur de l'effacement complet) pour les bandes.
 * Créé une fois à la taille de la plus grande bande ; jamais réécrit (lu seulement par le GPU). Un tampon trop
 * petit après un redimensionnement → retour à l'effacement complet (journalisé). */
static int noir_pret(VkDeviceSize taille)
{
    if (P.zbt >= taille) return 0;
    if (P.zbt) return -1;
    void *ptr;
    if (tampon_hote(taille, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &P.zb, &P.zbm, &ptr)) { P.bandes = 0; return -1; }
    const uint8_t v = P.temoin ? 255 : 0;                  /* témoin (banc) : magenta 255, 0, 255 */
    for (VkDeviceSize i = 0; i < taille; i += 4) { uint8_t *q = (uint8_t *)ptr + i; q[0] = q[2] = v; q[1] = 0; q[3] = 255; }
    P.zbt = taille;
    sc2_log("[VKP] SC2_PRESENT_BANDES=1 : %s écrit dans les bandes seulement (tampon %llu octets), plus d'effacement de "
            "l'image entière", P.temoin ? "MAGENTA (témoin SC2_PRESENT_BANDES_TEMOIN=1)" : "noir", (unsigned long long)taille);
    return 0;
}

/* Banc : tampon de relecture de l'image de swapchain (contrôle du blit et des bandes). */
static int relecture_prete(VkDeviceSize taille)
{
    if (P.rbt >= taille) return 0;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = taille,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (p_vkCreateBuffer(P.dev, &bi, NULL, &P.rb)) return -1;
    VkMemoryRequirements mr; p_vkGetBufferMemoryRequirements(P.dev, P.rb, &mr);
    VkPhysicalDeviceMemoryProperties mp; p_vkGetPhysicalDeviceMemoryProperties(P.pd, &mp);
    uint32_t t = 0;
    for (; t < mp.memoryTypeCount; t++)
        if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
    if (t == mp.memoryTypeCount || p_vkAllocateMemory(P.dev, &ai, NULL, &P.rbm)) return -1;
    p_vkBindBufferMemory(P.dev, P.rb, P.rbm, 0);
    p_vkMapMemory(P.dev, P.rbm, 0, taille, 0, &P.rbp);
    P.rbt = taille;
    return 0;
}

/* Passe 7f — discipline de recréation. Sur Android, vkQueuePresentKHR rend VK_SUBOPTIMAL_KHR à CHAQUE
 * frame quand le preTransform de la swapchain diffère de la rotation courante de l'écran (appareil en
 * portrait natif tenu en paysage : émulateur, Fold replié) ; on laisse cette rotation au compositeur
 * (preTransform IDENTITY, le blit ne sait pas tourner). La 7b recréait la swapchain à chaque
 * SUBOPTIMAL → une swapchain neuve par frame, l'ancienne détruite aussitôt : le compositeur n'a jamais
 * rien affiché (retest 7c/7e : 4 809 créations). Désormais :
 *   - VK_ERROR_OUT_OF_DATE_KHR : recréation (obligatoire) ;
 *   - VK_SUBOPTIMAL_KHR : recréation SEULEMENT si la taille de la surface a changé ; sinon ignoré
 *     (compté, journalisé une fois avec les deux rotations). */
static void hors_date(const char *ou)
{
    P.n_ood++; P.a_refaire = 1; P.motif = "VK_ERROR_OUT_OF_DATE_KHR";
    if (P.n_ood <= 3) sc2_log("[VKP] %s → VK_ERROR_OUT_OF_DATE_KHR : swapchain à recréer", ou);
}
static void sous_optimal(const char *ou)
{
    VkSurfaceCapabilitiesKHR cap;
    if (p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(P.pd, P.surf, &cap)) return;
    VkExtent2D e = cap.currentExtent;
    if (e.width != 0xFFFFFFFFu && (e.width != P.ext.width || e.height != P.ext.height)) {
        P.n_redim++; P.a_refaire = 1; P.motif = "redimensionnement";
        sc2_log("[VKP] %s → VK_SUBOPTIMAL_KHR et surface %ux%u ≠ swapchain %ux%u : recréation", ou, e.width, e.height,
                P.ext.width, P.ext.height);
        return;
    }
    if (!P.n_subopt_ignores++)
        sc2_log("[VKP] %s → VK_SUBOPTIMAL_KHR, taille inchangée (rotation courante 0x%X, preTransform 0x%X) : swapchain "
                "GARDÉE, la rotation reste au compositeur (suivants comptés, pas journalisés)", ou,
                (unsigned)cap.currentTransform, (unsigned)P.tr_cree);
}

#ifndef __ANDROID__
/* Banc seulement — reproduire les deux situations du téléphone sur Xvfb :
 *   SC2_SWAPCHAIN_TEST_SUBOPTIMAL=1 : chaque present est traité comme VK_SUBOPTIMAL_KHR (cas émulateur) ;
 *   SC2_SWAPCHAIN_TEST_REDIM=<frame> : la fenêtre X passe de P.fw×P.fh à 1920×1080 à cette frame
 *   (redimensionnement réel → une recréation attendue). */
static int test_subopt = -1, test_redim = -1, test_echec = 1;   /* test_echec 1 : variable pas encore lue */
#ifdef __APPLE__
/* M5 (point 6) : pas de fenêtre X sur Mac (SC2_SWAPCHAIN_TEST_REDIM sans effet) ; _SUBOPTIMAL et _ECHEC restent actifs */
static void banc_redimensionner(unsigned n) { (void)n; (void)test_redim; }
#else
static void banc_redimensionner(unsigned n)
{
    if (test_redim < 0) { const char *e = getenv("SC2_SWAPCHAIN_TEST_REDIM"); test_redim = e ? atoi(e) : 0; }
    if (!test_redim || n != (unsigned)test_redim || !g_xcb) return;
    void *x = dlopen("libxcb.so.1", RTLD_NOW);
    xcb_void_cookie_t (*conf)(xcb_connection_t *, uint32_t, uint16_t, const uint32_t *) = x ? dlsym(x, "xcb_configure_window") : NULL;
    int (*vider)(xcb_connection_t *) = x ? dlsym(x, "xcb_flush") : NULL;
    if (!conf || !vider) return;
    const uint32_t v[2] = { 1920, 1080 };
    conf(g_xcb, g_xwin, 4 | 8, v); vider(g_xcb);                      /* XCB_CONFIG_WINDOW_WIDTH | HEIGHT */
    sc2v_taille_changee();                                              /* = APP_CMD_WINDOW_RESIZED de l'APK */
    sc2_log("[VKP] banc : SC2_SWAPCHAIN_TEST_REDIM — fenêtre X redimensionnée en 1920x1080 à la frame %u", n);
}
#endif
#endif

/* ---- passe 10r (v13) : deux frames en vol côté backend (sc2_d3d9v.c) ------------------------------------------------
 * Le backend ne vide plus sa file à chaque frame : sa soumission de présentation signale un sémaphore « rendu » que le
 * blit attend (frame N finie sur la file du backend) ; le blit signale « blit fini », attendu par la prochaine soumission
 * du backend (la frame N+1 ne réécrit pas le back buffer avant que le blit l'ait lu). La fence du blit n'est plus attendue
 * juste après le present, mais au début de la présentation suivante, avant de réutiliser P.cb et P.fence : le CPU repart
 * sur la frame N+1 pendant que le GPU finit la frame N. Un sémaphore binaire signalé est consommé sur TOUS les chemins
 * (soumission vide qui l'attend). SC2_VK_SYNCHRONE=1 côté backend : d3dv_en_vol() = 0, comportement d'avant. */
extern void d3dv_semaphore_rendu_demander(void);                 /* déclarées dans recomp-sc2/runtime/sc2_d3d9v.h */
extern uint64_t d3dv_semaphore_rendu(void);
extern void d3dv_attendre_semaphore(uint64_t s, uint32_t etape);
extern int d3dv_en_vol(void);
/* soumission précédente de la présentation (blit ou consommation) finie : 0 ; bloquée → bascule, -1 */
static int fence_precedente(unsigned n)
{
    if (!P.fence_soumise) return 0;
    VkResult rf = p_vkWaitForFences(P.dev, 1, &P.fence, VK_TRUE, 2000000000ull);
    if (rf && P.n_fence_ko++ < 3)
        sc2_log("[VKP] vkWaitForFences (avant la frame n°%u) → %d : blit précédent non terminé (cas n°%llu ; suivants comptés)",
                n, (int)rf, (unsigned long long)P.n_fence_ko);
    if (rf == VK_TIMEOUT) rf = p_vkWaitForFences(P.dev, 1, &P.fence, VK_TRUE, 5000000000ull);
    if (rf) { basculer("vkWaitForFences (blit de présentation non terminé)", (int)rf); return -1; }
    P.fence_soumise = 0;
    return 0;
}
/* sémaphore « rendu » non attendu par un blit : soumission vide qui l'attend (suivie par P.fence si elle est libre) */
static void rendu_consommer(VkSemaphore s)
{
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &s,
                        .pWaitDstStageMask = &st };
    VkFence f = VK_NULL_HANDLE;
    if (!P.fence_soumise) { p_vkResetFences(P.dev, 1, &P.fence); f = P.fence; }
    if (!p_vkQueueSubmit(P.q, 1, &si, f) && f) P.fence_soumise = 1;
}
void sc2v_consommer_rendu(void)                       /* __wrap_d3dv_present : présentation non appelée cette frame */
{
    if (!P.dev_ok) return;
    pthread_mutex_lock(&g_mu);
    VkSemaphore s = (VkSemaphore)(uintptr_t)d3dv_semaphore_rendu();
    if (s && !fence_precedente(0)) rendu_consommer(s);
    pthread_mutex_unlock(&g_mu);
}

void sc2v_barre_voulue(int v)
{
    pthread_mutex_lock(&g_barre_mu);
    barre_voulue_lire();
    if (!g_bforce && g_bvoulue != !!v) {
        g_bvoulue = !!v;
        sc2_log("[BARRE] %s", v ? "affichée (toucher)" : "masquée (souris ou clavier physique)");
    }
    pthread_mutex_unlock(&g_barre_mu);
}
int sc2v_barre_geo(barre_geo *g)
{
    pthread_mutex_lock(&g_barre_mu); *g = g_bgeo; pthread_mutex_unlock(&g_barre_mu);
    return g->jeu.w ? 0 : -1;
}
int sc2v_barre_appuyer(int32_t sx, int32_t sy, uint16_t *vk, uint8_t *bas, int max)
{
    int n = -1;
    pthread_mutex_lock(&g_barre_mu);
    if (barre_dedans(&g_bgeo, sx, sy)) {
        int k = barre_toucher(&g_bgeo, g_be.page, sx, sy);
        n = 0;
        if (k >= 0) { n = barre_appuyer(&g_be, barre_def_de(g_be.page, k), vk, bas, max); g_n_barre_appuis++; }
    }
    pthread_mutex_unlock(&g_barre_mu);
    return n;
}
/* Tampon hôte de la barre, redessiné seulement quand elle change (état, taille, ordre des canaux). Sous g_mu, APRÈS
 * fence_precedente : la copie de la frame d'avant est finie, le tampon peut être réécrit. 0 = prêt. */
static int barre_preparer(const barre_geo *g, const barre_etat *e)
{
    VkDeviceSize t = (VkDeviceSize)g->barre.w * (VkDeviceSize)g->barre.h * 4;
    int bgr = P.sfmt == VK_FORMAT_B8G8R8A8_UNORM;
    if (t > B.t) {
        if (B.b) { p_vkDestroyBuffer(P.dev, B.b, NULL); p_vkFreeMemory(P.dev, B.m, NULL); B.b = VK_NULL_HANDLE; }
        B.t = 0; B.pret = 0;
        if (tampon_hote(t, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &B.b, &B.m, &B.p)) return -1;
        B.t = t;
        sc2_log("[BARRE] tampon %dx%d (%llu octets), bande %s", g->barre.w, g->barre.h, (unsigned long long)t,
                g->verticale ? "à droite" : "en bas");
    }
    if (!B.pret || B.version != e->version || B.w != g->barre.w || B.h != g->barre.h || B.bgr != bgr) {
        barre_dessiner((uint8_t *)B.p, g, e, bgr);
        B.pret = 1; B.version = e->version; B.w = g->barre.w; B.h = g->barre.h; B.bgr = bgr;
    }
    return 0;
}

int sc2v_presenter(int id, unsigned n)
{
    if (!sc2v_actif() || id < 0 || id >= 256 || !P.c[id].img) return -1;
    pthread_mutex_lock(&g_mu);
    int rc = -1;
    VkSemaphore rendu = (VkSemaphore)(uintptr_t)d3dv_semaphore_rendu();   /* passe 10r (v13) : consommé sur tous les chemins */
    int en_vol = d3dv_en_vol();
    if (fence_precedente(n)) goto fin;                  /* passe 10r (v13) : la frame précédente, plus la nôtre */
#ifdef __APPLE__
    if (!atomic_load(&g_visible)) {                     /* M5 : fenêtre occultée — rien d'acquis, « rendu » consommé à fin */
        g_n_sautees++;
        if (!g_serie_sautees++) sc2_log("[AFFICHAGE] présentation suspendue (fenêtre occultée) au Present n°%u", n);
        rc = 0; goto fin;
    }
    if (g_serie_sautees) {
        sc2_log("[AFFICHAGE] présentation reprise au Present n°%u après %u Present sautés (fenêtre occultée)", n,
                g_serie_sautees);
        g_serie_sautees = 0;
    }
#endif
    static VkFormat verifie; static int blit_ok;
    if (verifie != P.c[id].fmt) {                            /* le blit exige BLIT_SRC sur le format du back buffer */
        VkFormatProperties fp; p_vkGetPhysicalDeviceFormatProperties(P.pd, P.c[id].fmt, &fp);
        blit_ok = !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT);
        verifie = P.c[id].fmt;
        if (!blit_ok) sc2_log("[VKP] format %d du back buffer sans BLIT_SRC : copie CPU", (int)verifie);
    }
    if (!blit_ok) { basculer("format du back buffer sans BLIT_SRC", 0); goto fin; }   /* passe 10a */
    if (!P.fenetre && !P.fw) goto fin;                       /* pas de fenêtre : rien à présenter */
    if (!P.surf && surface_creer()) goto fin;
    if (P.sc && atomic_exchange(&P.verifier_taille, 0)) {
        VkSurfaceCapabilitiesKHR cap;
        if (!p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(P.pd, P.surf, &cap) && cap.currentExtent.width != 0xFFFFFFFFu
            && (cap.currentExtent.width != P.ext.width || cap.currentExtent.height != P.ext.height)) {
            P.n_redim++; P.a_refaire = 1; P.motif = "redimensionnement signalé par la plate-forme";
            sc2_log("[VKP] taille signalée changée : surface %ux%u ≠ swapchain %ux%u → recréation", cap.currentExtent.width,
                    cap.currentExtent.height, P.ext.width, P.ext.height);
        }
    }
    if ((!P.sc || P.a_refaire) && swapchain_creer()) goto fin;

    uint32_t k;
    sc2tp_debut(TP_ACQUISITION);
    VkResult r = p_vkAcquireNextImageKHR(P.dev, P.sc, 1000000000ull, P.acq, VK_NULL_HANDLE, &k);
    sc2tp_fin(TP_ACQUISITION);
    sc2tp_debut(TP_ENREG);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { hors_date("vkAcquireNextImageKHR"); echec_tolere("vkAcquireNextImageKHR (OUT_OF_DATE répétés)", (int)r); goto fin; }
    if (r && r != VK_SUBOPTIMAL_KHR) {
        /* passe 10a : journal borné (il partait à chaque frame) ; négatif → bascule, VK_TIMEOUT / VK_NOT_READY → borné */
        if (P.n_acq_ko++ < 3) sc2_log("[VKP] vkAcquireNextImageKHR : %d (cas n°%llu ; suivants comptés)", (int)r,
                                      (unsigned long long)P.n_acq_ko);
        if (r < 0) basculer("vkAcquireNextImageKHR", (int)r); else echec_tolere("vkAcquireNextImageKHR (répété)", (int)r);
        goto fin;
    }
    /* SUBOPTIMAL à l'acquisition : l'image est utilisable ; la décision se prend après le present */

    /* rectangle de destination : rapport du back buffer conservé, centré (bandes noires) */
    uint32_t sw = P.c[id].w, sh = P.c[id].h, dw = P.ext.width, dh = P.ext.height, rw, rh;
    int32_t ox, oy;
    /* passe 7o : le calcul que vers_jeu inverse ; 21/10 : image calée en haut ou à gauche quand la barre occupe la bande */
    pthread_mutex_lock(&g_barre_mu);
    barre_geo bg = barre_disposer(sw, sh, dw, dh, barre_voulue_lire());
    g_bgeo = bg; barre_etat be = g_be;
    pthread_mutex_unlock(&g_barre_mu);
    ox = bg.jeu.x; oy = bg.jeu.y; rw = (uint32_t)bg.jeu.w; rh = (uint32_t)bg.jeu.h;
    int barre = bg.active && !barre_preparer(&bg, &be);
    atomic_store(&g_rect, ((uint64_t)sw << 48) | ((uint64_t)sh << 32) | ((uint64_t)dw << 16) | dh);

    VkImage dst = P.img[k];
    p_vkResetCommandBuffer(P.cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    p_vkBeginCommandBuffer(P.cb, &bi);
    barriere_img(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    /* bandes autour du rectangle du blit (gauche, droite, haut, bas ; vides écartées) */
    VkBufferImageCopy bd[4]; uint32_t nbd = 0; VkDeviceSize plus = 0;
    const int32_t bx[4] = { 0, ox + (int32_t)rw, 0, 0 }, by[4] = { 0, 0, 0, oy + (int32_t)rh };
    const int32_t bw[4] = { ox, (int32_t)dw - ox - (int32_t)rw, (int32_t)dw, (int32_t)dw };
    const int32_t bh[4] = { (int32_t)dh, (int32_t)dh, oy, (int32_t)dh - oy - (int32_t)rh };
    for (int q = 0; q < 4; q++) if (bw[q] > 0 && bh[q] > 0) {
        bd[nbd++] = (VkBufferImageCopy){ .bufferRowLength = (uint32_t)bw[q], .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                         .imageOffset = { bx[q], by[q], 0 }, .imageExtent = { (uint32_t)bw[q], (uint32_t)bh[q], 1 } };
        if ((VkDeviceSize)bw[q] * (VkDeviceSize)bh[q] * 4 > plus) plus = (VkDeviceSize)bw[q] * (VkDeviceSize)bh[q] * 4;
    }
    int voie_bandes = P.bandes && (!nbd || !noir_pret(plus));
    if (voie_bandes) {           /* passe 7p3 : bandes seulement (le blit couvre le reste) */
        if (nbd) p_vkCmdCopyBufferToImage(P.cb, P.zb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nbd, bd);
        P.n_bandes++;
    } else {
        if (P.bandes) { sc2_log("[VKP] bandes plus grandes que le tampon noir : effacement complet"); P.bandes = 0; }
        VkClearColorValue noir = { .float32 = { 0, 0, 0, 1 } };
        VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p_vkCmdClearColorImage(P.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &noir, 1, &sr);
    }
    barriere_img(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .srcOffsets = { { 0, 0, 0 }, { (int32_t)sw, (int32_t)sh, 1 } },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .dstOffsets = { { ox, oy, 0 }, { ox + (int32_t)rw, oy + (int32_t)rh, 1 } } };
    if (barre) {                     /* 21/10 : la barre dans sa bande (hors du rectangle du blit : pas de barrière) */
        VkBufferImageCopy bc = { .bufferRowLength = (uint32_t)bg.barre.w, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageOffset = { bg.barre.x, bg.barre.y, 0 },
                                 .imageExtent = { (uint32_t)bg.barre.w, (uint32_t)bg.barre.h, 1 } };
        p_vkCmdCopyBufferToImage(P.cb, B.b, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    }
    /* Le back buffer est en GENERAL (backend 5c/5d) et le backend a attendu sa file : lisible. */
    p_vkCmdBlitImage(P.cb, P.c[id].img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    /* mêmes frames que les PPM du backend (1, 10, 100, 200…) : comparaison canal par canal */
    int relire = P.controle && (n == 1 || n == 10 || n % 100 == 0) && !relecture_prete((VkDeviceSize)dw * dh * 4);
    if (relire) {
        barriere_img(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { dw, dh, 1 } };
        p_vkCmdCopyImageToBuffer(P.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P.rb, 1, &bc);
        barriere_img(dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
    } else
        barriere_img(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    p_vkEndCommandBuffer(P.cb);

    /* passe 10r (v13) : attendre aussi le rendu de la frame (file du backend) ; signaler « blit fini » pour le backend */
    VkSemaphore att[2] = { P.acq, rendu }, sig[2] = { P.fin, P.blit_fin };
    VkPipelineStageFlags ps[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = rendu ? 2 : 1, .pWaitSemaphores = att,
                        .pWaitDstStageMask = ps, .commandBufferCount = 1, .pCommandBuffers = &P.cb,
                        .signalSemaphoreCount = en_vol ? 2 : 1, .pSignalSemaphores = sig };
    p_vkResetFences(P.dev, 1, &P.fence);
    P.fence_soumise = 0;
    sc2tp_fin(TP_ENREG);
    sc2tp_debut(TP_SOUMISSION);
    r = p_vkQueueSubmit(P.q, 1, &si, P.fence);
    sc2tp_fin(TP_SOUMISSION);
    /* passe 10a : le sémaphore d'acquisition resterait signalé sans attente — la frame suivante le réutiliserait */
    if (r) { sc2_log("[VKP] vkQueueSubmit : %d", (int)r); basculer("vkQueueSubmit", (int)r); goto fin; }
    P.fence_soumise = 1;
    rendu = VK_NULL_HANDLE;                             /* passe 10r (v13) : attendu par le blit */
    if (en_vol) d3dv_attendre_semaphore((uint64_t)(uintptr_t)P.blit_fin, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &P.fin,
                            .swapchainCount = 1, .pSwapchains = &P.sc, .pImageIndices = &k };
    sc2tp_debut(TP_PRESENT);
    r = p_vkQueuePresentKHR(P.q, &pi);
    sc2tp_fin(TP_PRESENT);
    static int premier_present;                     /* passe 9d : le 1er code retour, quel qu'il soit */
    if (!premier_present++) sc2_log("[VKP] 1er vkQueuePresentKHR (frame n°%u, image %u) → %d", n, k, (int)r);
    /* le blit doit avoir lu le back buffer avant que le backend ne le réécrive. Passe 10r (v13) : en vol, c'est le
     * sémaphore « blit fini » qui l'assure côté GPU, et la fence n'est attendue qu'à la frame suivante — sauf pour la
     * relecture de contrôle du banc (lue ci-dessous par le CPU) et en mode synchrone. */
    sc2tp_debut(TP_BARRIERE);
    VkResult rf = VK_SUCCESS;
    if (relire || !en_vol) { rf = p_vkWaitForFences(P.dev, 1, &P.fence, VK_TRUE, 2000000000ull); if (!rf) P.fence_soumise = 0; }
    sc2tp_fin(TP_BARRIERE);
    if (rf && P.n_fence_ko++ < 3)                    /* passe 9d : VK_TIMEOUT (2 s) ou perte du device */
        sc2_log("[VKP] vkWaitForFences (frame n°%u) → %d : le blit n'est peut-être pas terminé (cas n°%llu ; suivants comptés)",
                n, (int)rf, (unsigned long long)P.n_fence_ko);
    /* passe 10a : la frame suivante remettrait à zéro fence et tampon de commandes ENCORE en vol (comportement
     * indéfini, perte du device). VK_TIMEOUT : une 2e attente de 5 s ; toujours rien, ou erreur → bascule. */
    if (rf == VK_TIMEOUT) { rf = p_vkWaitForFences(P.dev, 1, &P.fence, VK_TRUE, 5000000000ull); if (!rf) P.fence_soumise = 0; }
    if (rf) { basculer("vkWaitForFences (blit de présentation non terminé)", (int)rf); goto fin; }
#ifndef __ANDROID__
    if (test_subopt < 0) { const char *e = getenv("SC2_SWAPCHAIN_TEST_SUBOPTIMAL"); test_subopt = e && *e == '1'; }
    if (test_subopt && r == VK_SUCCESS) r = VK_SUBOPTIMAL_KHR;
    /* passe 10a : SC2_SWAPCHAIN_TEST_ECHEC=<VkResult négatif> — le 1er present réussi est traité comme cet échec
     * (preuve de la bascule copie CPU au banc ; ex. -4 = VK_ERROR_DEVICE_LOST, -1000000000 = VK_ERROR_SURFACE_LOST_KHR) */
    if (test_echec > 0) { const char *e = getenv("SC2_SWAPCHAIN_TEST_ECHEC"); test_echec = e ? atoi(e) : 0; }
    if (test_echec < 0 && r == VK_SUCCESS) { r = (VkResult)test_echec; test_echec = 0; }
    banc_redimensionner(n);
#endif
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        hors_date("vkQueuePresentKHR");
        echec_tolere("vkQueuePresentKHR (OUT_OF_DATE répétés)", (int)r);   /* passe 10a : recréation, mais bornée */
    }
    else if (r == VK_SUBOPTIMAL_KHR) sous_optimal("vkQueuePresentKHR");
    else if (r) {
        /* passe 10a : négatif (1er present compris) → bascule ; avant, retenté à chaque frame, sémaphore indéfini */
        sc2_log("[VKP] vkQueuePresentKHR : %d", (int)r);
        if (r < 0) basculer(P.presentes ? "vkQueuePresentKHR" : "1er vkQueuePresentKHR", (int)r);
        goto fin;
    }
    if (r != VK_ERROR_OUT_OF_DATE_KHR) P.echecs_suite = 0;
    P.presentes++;
    if (P.presentes == 1) sc2_log("[VKP] 1re frame présentée par la swapchain (back buffer %ux%u → %ux%u en (%d, %d), image %ux%u)",
                                  sw, sh, rw, rh, ox, oy, dw, dh);
    if (relire) {
        /* moyennes R, G, B de l'image (rectangle du blit) et des bandes : contrôle du banc. Ordre des
         * octets selon le format de la swapchain (B8G8R8A8 : B, G, R). Passe 8d : la voie qui a écrit les
         * bandes, le nombre de pixels de bande non noirs et une empreinte FNV-1a 64 (R, G, B de TOUS les pixels)
         * de l'image et des bandes : A (effacement) et B (bandes seules) doivent donner les mêmes empreintes,
         * le témoin magenta une empreinte de bandes différente. */
        const uint8_t *p = P.rbp; uint64_t sc[3] = { 0, 0, 0 }, sb = 0, ni = 0, nb = 0, nnoir = 0;
        uint64_t hi = 0xcbf29ce484222325ull, hb = hi;
        int bgr = P.sfmt == VK_FORMAT_B8G8R8A8_UNORM || P.sfmt == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint32_t y = 0; y < dh; y++) for (uint32_t x = 0; x < dw; x++) {
            const uint8_t *q = p + ((size_t)y * dw + x) * 4;
            int dedans = (int32_t)x >= ox && x < (uint32_t)ox + rw && (int32_t)y >= oy && y < (uint32_t)oy + rh;
            uint64_t *h = dedans ? &hi : &hb;
            for (int c = 0; c < 3; c++) { *h ^= q[c]; *h *= 0x100000001b3ull; }
            if (!dedans && (q[0] | q[1] | q[2])) nnoir++;
            if (x & 1 || y & 1) continue;                       /* moyennes : 1 pixel sur 4, comme avant */
            if (dedans) { sc[0] += q[bgr ? 2 : 0]; sc[1] += q[1]; sc[2] += q[bgr ? 0 : 2]; ni++; }
            else { sb += q[0] + q[1] + q[2]; nb++; }
        }
        sc2_log("[VKP] contrôle frame n°%u : image de swapchain %ux%u, moyenne RGB de l'image %.2f %.2f %.2f, bandes %.2f (%s) ; "
                "voie %s, pixels de bande non noirs %llu, empreinte image %016llx bandes %016llx",
                n, dw, dh, ni ? (double)sc[0] / ni : 0, ni ? (double)sc[1] / ni : 0, ni ? (double)sc[2] / ni : 0,
                nb ? (double)sb / (3.0 * nb) : 0, nb ? "bandes présentes" : "pas de bandes",
                voie_bandes ? (P.temoin ? "bandes-temoin" : "bandes-seules") : "effacement-complet",
                (unsigned long long)nnoir, (unsigned long long)hi, (unsigned long long)hb);
    }
    rc = 0;
fin:
    if (rendu) rendu_consommer(rendu);                 /* passe 10r (v13) : pas de blit cette frame */
    if (rc) P.echecs++;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

/* Passe 7o : géométrie du dernier blit (back buffer sw×sh dans la swapchain dw×dh), lue par le fil de
 * l'activité pour convertir les touchers ; un seul mot atomique (16 bits chacun) : jamais de demi-mise à jour. */
int sc2v_geometrie(uint32_t *sw, uint32_t *sh, uint32_t *dw, uint32_t *dh)
{
    uint64_t v = atomic_load(&g_rect);
    if (!v) return -1;
    *sw = (uint32_t)(v >> 48); *sh = (uint32_t)(v >> 32) & 0xFFFF; *dw = (uint32_t)(v >> 16) & 0xFFFF; *dh = (uint32_t)v & 0xFFFF;
    return 0;
}

/* Passe 10o : la voie de présentation en quelques mots, pour la ligne [FPS] (fil du jeu). */
void sc2v_etat_court(char *o, size_t cap)
{
    if (!P.voulu) snprintf(o, cap, "SC2_SWAPCHAIN=0");
    else if (!P.inst_ok || !P.dev_ok) snprintf(o, cap, "swapchain indisponible (extensions refusées)");
    else if (P.bascule) snprintf(o, cap, "swapchain basculée : %s, code %d", P.bascule_ou ? P.bascule_ou : "?", P.bascule_code);
    else if (!P.sc) snprintf(o, cap, "swapchain pas encore créée");
    else snprintf(o, cap, "swapchain %ux%u", P.ext.width, P.ext.height);
}

void sc2v_bilan(char *o, size_t cap)
{
    char etat[160];                                      /* passe 10a : la bascule, son code et son point */
    if (P.bascule) snprintf(etat, sizeof etat, "BASCULÉE sur la copie CPU (code %d, %s)", P.bascule_code, P.bascule_ou);
    else snprintf(etat, sizeof etat, "%s", sc2v_actif() ? "active" : P.voulu ? "DEMANDÉE mais indisponible" : "inactive");
    snprintf(o, cap, "swapchain %s : %llu frames présentées par le GPU, %llu repli(s) sur la copie CPU ; créée %llu fois "
             "(OUT_OF_DATE %llu, redimensionnements %llu), SUBOPTIMAL ignorés %llu ; bandes seules %llu ; vkWaitForFences en échec %llu ; "
             "acquisitions en échec %llu ; réarmements après bascule %u ; barre de raccourcis : %llu appui(s)", etat,
             (unsigned long long)P.presentes, (unsigned long long)P.echecs, (unsigned long long)P.n_crees,
             (unsigned long long)P.n_ood, (unsigned long long)P.n_redim, (unsigned long long)P.n_subopt_ignores,
             (unsigned long long)P.n_bandes, (unsigned long long)P.n_fence_ko, (unsigned long long)P.n_acq_ko, P.rearmements,
             (unsigned long long)g_n_barre_appuis);
#ifdef __APPLE__
    size_t l = cap ? strlen(o) : 0;                      /* M5 : sc2v_visible(0) */
    if (l + 1 < cap) snprintf(o + l, cap - l, " ; présentations sautées (fenêtre occultée) %u", g_n_sautees);
#endif
}
