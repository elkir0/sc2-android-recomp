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
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "sc2_vk_presente.h"

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
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceMemoryProperties)
#define VKD(X) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) \
    X(vkCmdBlitImage) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkResetCommandBuffer) X(vkCreateBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCmdCopyImageToBuffer)
#define DECL(f) static PFN_##f p_##f;
VKI(DECL) VKD(DECL)

static struct {
    int voulu, inst_ok, dev_ok;           /* SC2_SWAPCHAIN=1 ; extensions obtenues */
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev;
    uint32_t qf; VkQueue q; int file_propre;
    VkCommandPool pool; VkCommandBuffer cb; VkSemaphore acq, fin; VkFence fence;
    /* cibles du backend : id → image */
    struct { VkImage img; uint32_t w, h; VkFormat fmt; } c[256];
    VkImage derniere; VkFormat derniere_fmt; uint32_t derniere_w, derniere_h;
    /* fenêtre / surface / swapchain */
    void *fenetre; uint32_t fw, fh;       /* fw/fh : banc (surface sans taille propre) */
    int fenetre_changee;
    VkSurfaceKHR surf; VkSwapchainKHR sc; VkImage img[8]; uint32_t nimg; VkExtent2D ext; VkFormat sfmt;
    int a_refaire;
    uint64_t presentes, echecs;
    int controle;                         /* banc : relecture de l'image de swapchain (SC2_SWAPCHAIN_CONTROLE) */
    VkBuffer rb; VkDeviceMemory rbm; void *rbp; VkDeviceSize rbt;
} P;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- interposition du chargeur ------------------------------------------------------------ */

static int ext_presente(const VkExtensionProperties *e, uint32_t n, const char *nom)
{
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, nom)) return 1;
    return 0;
}

#ifdef __ANDROID__
#define EXT_SURFACE_PLATEFORME VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
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
    int ok = ext_presente(e, n, VK_KHR_SURFACE_EXTENSION_NAME) && ext_presente(e, n, EXT_SURFACE_PLATEFORME);
    if (!ok) {
        char l[1024] = ""; size_t o = 0;
        for (uint32_t i = 0; i < n && o < sizeof l - 80; i++) o += (size_t)snprintf(l + o, sizeof l - o, " %s", e[i].extensionName);
        sc2_log("[VKP] instance : %s et/ou %s absentes du chargeur — swapchain indisponible (annoncées :%s)",
                VK_KHR_SURFACE_EXTENSION_NAME, EXT_SURFACE_PLATEFORME, l);
    }
    if (ok) { ext[k++] = VK_KHR_SURFACE_EXTENSION_NAME; ext[k++] = EXT_SURFACE_PLATEFORME; }
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
#else
        p_vkCreateXcbSurfaceKHR = (sc2_PFN_vkCreateXcbSurfaceKHR)r_gipa(P.inst, "vkCreateXcbSurfaceKHR");
#endif
        sc2_log("[VKP] instance : %s + %s ajoutées", VK_KHR_SURFACE_EXTENSION_NAME, EXT_SURFACE_PLATEFORME);
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
        p_vkCreateSemaphore(P.dev, &sci, NULL, &P.fin) || p_vkCreateFence(P.dev, &fci, NULL, &P.fence)) return r;
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

void *__real_dlsym(void *h, const char *nom);
void *__wrap_dlsym(void *h, const char *nom)
{
    void *f = __real_dlsym(h, nom);
    if (!f || !nom || strcmp(nom, "vkGetInstanceProcAddr")) return f;
    const char *e = getenv("SC2_SWAPCHAIN");
    P.voulu = e && *e == '1';
    const char *k = getenv("SC2_SWAPCHAIN_CONTROLE");
    P.controle = k && *k == '1';
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
    return id;
}

/* ---- fenêtre, surface, swapchain ----------------------------------------------------------- */

int sc2v_actif(void) { return P.voulu && P.inst_ok && P.dev_ok; }

static void swapchain_liberer(void)      /* sous g_mu */
{
    if (P.q) p_vkQueueWaitIdle(P.q);
    if (P.sc) { p_vkDestroySwapchainKHR(P.dev, P.sc, NULL); P.sc = VK_NULL_HANDLE; }
    P.nimg = 0;
}
static void surface_liberer(void)        /* sous g_mu */
{
    swapchain_liberer();
    if (P.surf) { p_vkDestroySurfaceKHR(P.inst, P.surf, NULL); P.surf = VK_NULL_HANDLE; }
}

void sc2v_fenetre(void *fenetre, uint32_t w, uint32_t h)
{
    pthread_mutex_lock(&g_mu);
    if (P.inst_ok && P.dev_ok) surface_liberer();       /* l'ancienne fenêtre n'est plus utilisable */
    P.fenetre = fenetre; P.fw = w; P.fh = h; P.fenetre_changee = 1;
    pthread_mutex_unlock(&g_mu);
}

static int surface_creer(void)           /* sous g_mu */
{
    VkResult r;
#ifdef __ANDROID__
    VkAndroidSurfaceCreateInfoKHR si = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                         .window = (ANativeWindow *)P.fenetre };
    r = p_vkCreateAndroidSurfaceKHR ? p_vkCreateAndroidSurfaceKHR(P.inst, &si, NULL, &P.surf) : VK_ERROR_EXTENSION_NOT_PRESENT;
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
    if (r) { sc2_log("[VKP] création de la surface : échec %d", (int)r); P.surf = VK_NULL_HANDLE; return -1; }
    VkBool32 ok = 0;
    p_vkGetPhysicalDeviceSurfaceSupportKHR(P.pd, P.qf, P.surf, &ok);
    if (!ok) { sc2_log("[VKP] la famille %u ne peut pas présenter sur cette surface", P.qf); surface_liberer(); return -1; }
    return 0;
}

static int swapchain_creer(void)         /* sous g_mu */
{
    VkSurfaceCapabilitiesKHR cap;
    if (p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(P.pd, P.surf, &cap)) return -1;
    VkExtent2D e = cap.currentExtent;
    if (e.width == 0xFFFFFFFFu) { e.width = P.fw ? P.fw : 1280; e.height = P.fh ? P.fh : 720; }
    if (!e.width || !e.height) return -1;                 /* fenêtre réduite */
    VkSurfaceFormatKHR f[32]; uint32_t nf = 32;
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(P.pd, P.surf, &nf, f);
    VkSurfaceFormatKHR choix = nf ? f[0] : (VkSurfaceFormatKHR){ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    for (uint32_t i = 0; i < nf; i++)                    /* UNORM : le back buffer D3D9 n'est pas sRGB */
        if (f[i].format == VK_FORMAT_R8G8B8A8_UNORM || f[i].format == VK_FORMAT_B8G8R8A8_UNORM) { choix = f[i]; break; }
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (P.controle && (cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { sc2_log("[VKP] swapchain sans TRANSFER_DST"); return -1; }
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
        .compositeAlpha = ca, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = P.sc };
    VkSwapchainKHR sc;
    VkResult r = p_vkCreateSwapchainKHR(P.dev, &ci, NULL, &sc);
    if (P.sc) p_vkDestroySwapchainKHR(P.dev, P.sc, NULL);
    P.sc = VK_NULL_HANDLE;
    if (r) { sc2_log("[VKP] vkCreateSwapchainKHR : échec %d", (int)r); return -1; }
    P.sc = sc; P.ext = e; P.sfmt = choix.format;
    P.nimg = 8; p_vkGetSwapchainImagesKHR(P.dev, P.sc, &P.nimg, P.img);
    P.a_refaire = 0;
    sc2_log("[VKP] swapchain %ux%u, format %d, %u images, FIFO", e.width, e.height, (int)choix.format, P.nimg);
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

int sc2v_presenter(int id, unsigned n)
{
    if (!sc2v_actif() || id < 0 || id >= 256 || !P.c[id].img) return -1;
    pthread_mutex_lock(&g_mu);
    int rc = -1;
    static VkFormat verifie; static int blit_ok;
    if (verifie != P.c[id].fmt) {                            /* le blit exige BLIT_SRC sur le format du back buffer */
        VkFormatProperties fp; p_vkGetPhysicalDeviceFormatProperties(P.pd, P.c[id].fmt, &fp);
        blit_ok = !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT);
        verifie = P.c[id].fmt;
        if (!blit_ok) sc2_log("[VKP] format %d du back buffer sans BLIT_SRC : copie CPU", (int)verifie);
    }
    if (!blit_ok) goto fin;
    if (!P.fenetre && !P.fw) goto fin;                       /* pas de fenêtre : rien à présenter */
    if (!P.surf && surface_creer()) goto fin;
    if ((!P.sc || P.a_refaire) && swapchain_creer()) goto fin;

    uint32_t k;
    VkResult r = p_vkAcquireNextImageKHR(P.dev, P.sc, 1000000000ull, P.acq, VK_NULL_HANDLE, &k);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { P.a_refaire = 1; goto fin; }
    if (r && r != VK_SUBOPTIMAL_KHR) { sc2_log("[VKP] vkAcquireNextImageKHR : %d", (int)r); goto fin; }
    if (r == VK_SUBOPTIMAL_KHR) P.a_refaire = 1;

    /* rectangle de destination : rapport du back buffer conservé, centré (bandes noires) */
    uint32_t sw = P.c[id].w, sh = P.c[id].h, dw = P.ext.width, dh = P.ext.height, rw, rh;
    if ((uint64_t)dw * sh >= (uint64_t)dh * sw) { rh = dh; rw = (uint32_t)((uint64_t)sw * dh / sh); }
    else { rw = dw; rh = (uint32_t)((uint64_t)sh * dw / sw); }
    int32_t ox = (int32_t)(dw - rw) / 2, oy = (int32_t)(dh - rh) / 2;

    VkImage dst = P.img[k];
    p_vkResetCommandBuffer(P.cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    p_vkBeginCommandBuffer(P.cb, &bi);
    barriere_img(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue noir = { .float32 = { 0, 0, 0, 1 } };
    VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdClearColorImage(P.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &noir, 1, &sr);
    barriere_img(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .srcOffsets = { { 0, 0, 0 }, { (int32_t)sw, (int32_t)sh, 1 } },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                       .dstOffsets = { { ox, oy, 0 }, { ox + (int32_t)rw, oy + (int32_t)rh, 1 } } };
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

    VkPipelineStageFlags ps = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &P.acq,
                        .pWaitDstStageMask = &ps, .commandBufferCount = 1, .pCommandBuffers = &P.cb,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &P.fin };
    p_vkResetFences(P.dev, 1, &P.fence);
    if ((r = p_vkQueueSubmit(P.q, 1, &si, P.fence))) { sc2_log("[VKP] vkQueueSubmit : %d", (int)r); goto fin; }
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &P.fin,
                            .swapchainCount = 1, .pSwapchains = &P.sc, .pImageIndices = &k };
    r = p_vkQueuePresentKHR(P.q, &pi);
    /* le blit doit avoir lu le back buffer avant que le backend ne le réécrive */
    p_vkWaitForFences(P.dev, 1, &P.fence, VK_TRUE, 2000000000ull);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) P.a_refaire = 1;
    else if (r) { sc2_log("[VKP] vkQueuePresentKHR : %d", (int)r); goto fin; }
    P.presentes++;
    if (P.presentes == 1) sc2_log("[VKP] 1re frame présentée par la swapchain (back buffer %ux%u → %ux%u en (%d, %d), image %ux%u)",
                                  sw, sh, rw, rh, ox, oy, dw, dh);
    if (relire) {
        /* moyennes R, G, B de l'image (rectangle du blit) et des bandes : contrôle du banc. Ordre des
         * octets selon le format de la swapchain (B8G8R8A8 : B, G, R). */
        const uint8_t *p = P.rbp; uint64_t sc[3] = { 0, 0, 0 }, sb = 0, ni = 0, nb = 0;
        int bgr = P.sfmt == VK_FORMAT_B8G8R8A8_UNORM || P.sfmt == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint32_t y = 0; y < dh; y += 2) for (uint32_t x = 0; x < dw; x += 2) {
            const uint8_t *q = p + ((size_t)y * dw + x) * 4;
            int dedans = (int32_t)x >= ox && x < (uint32_t)ox + rw && (int32_t)y >= oy && y < (uint32_t)oy + rh;
            if (dedans) { sc[0] += q[bgr ? 2 : 0]; sc[1] += q[1]; sc[2] += q[bgr ? 0 : 2]; ni++; }
            else { sb += q[0] + q[1] + q[2]; nb++; }
        }
        sc2_log("[VKP] contrôle frame n°%u : image de swapchain %ux%u, moyenne RGB de l'image %.2f %.2f %.2f, bandes %.2f (%s)",
                n, dw, dh, ni ? (double)sc[0] / ni : 0, ni ? (double)sc[1] / ni : 0, ni ? (double)sc[2] / ni : 0,
                nb ? (double)sb / (3.0 * nb) : 0, nb ? "bandes présentes" : "pas de bandes");
    }
    rc = 0;
fin:
    if (rc) P.echecs++;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

void sc2v_bilan(char *o, size_t cap)
{
    snprintf(o, cap, "swapchain %s : %llu frames présentées par le GPU, %llu repli(s) sur la copie CPU",
             sc2v_actif() ? "active" : P.voulu ? "DEMANDÉE mais indisponible" : "inactive",
             (unsigned long long)P.presentes, (unsigned long long)P.echecs);
}
