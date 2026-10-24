/* test_presentation_mac.c — V3, lot M5 (22/10) : branche __APPLE__ du présentateur (android/jni/sc2_vk_presente.c)
 * contre MoltenVK, sans fenêtre ni jeu (v3-critique.md §3.4, fiche M5, critère 2).
 *
 * Même assemblage que recomp-sc2 sur Mac (mac/mac.cmake) : le backend réel (sc2_d3d9v.c avec les quatre renommages
 * d3dv_present → d3dv_present_reel, d3dv_cible → d3dv_cible_reel, dlsym → sc2v_dlsym, dladdr → sc2v_dladdr), le
 * présentateur (__wrap_X → X, __real_X → X_reel) et sa trace ; ce test tient le rôle de la couche plate-forme
 * (sc2_android_hooks.c : d3dv_present) et de l'hôte (sc2_log, sc2a_cible_vue, sc2a_geo_invalider).
 *
 * Scénario : SC2_SWAPCHAIN=1, SC2_SWAPCHAIN_CONTROLE=1 ; surface SANS ÉCRAN 1280×720 (sc2v_fenetre(NULL, 1280, 720),
 * VK_EXT_headless_surface) ; back buffer 1280×720 X8R8G8B8 effacé en 0xFF204060 à chaque frame ; Present n° 1 à 100
 * comme la couche (sémaphore « rendu » demandé, d3dv_present_reel, sc2v_presenter, sinon sc2v_consommer_rendu).
 * Attendu :
 *   - trois lignes « contrôle frame n°1/10/100 … moyenne RGB de l'image 32.00 64.00 96.00 » et aucune bascule ;
 *   - « [VK] bibliothèque : …libMoltenVK.dylib (MoltenVK direct, sans loader) » malgré l'interposition (sc2v_dladdr) ;
 *   - « [VKP] instance : … ajoutées » une fois ; mode de présentation par défaut mailbox (ou repli fifo journalisé) ;
 *   - fenêtre occultée (sc2v_visible(0)) aux Present n° 50 à 52 : présentation sautée (rendu 0, rien présenté, un
 *     journal par série), puis reprise ;
 *   - 2e surface sans écran de 1280×1024 (sc2v_fenetre) : surface et swapchain recréées, image centrée à 32 64 96,
 *     bandes noires, et barre de raccourcis jamais active sur Mac (SC2_BARRE absente) malgré sc2v_barre_voulue(1) et
 *     la place (Android l'afficherait ici) ;
 *   - 3e surface : CAMetalLayer hors fenêtre (VK_EXT_metal_surface, la voie de l'app ; bounds, contentsScale 1 et
 *     drawableSize posés : MoltenVK tire currentExtent de bounds × contentsScale) → Present n° 300 à 32 64 96 ;
 *   - sortie : sc2v_liberer_essai rend -1 sous le verrou du présentateur (appelé depuis un journal émis sous ce verrou,
 *     comme une faute pendant la présentation dont le fil exécute exit), puis 0 au repos, swapchain détruite.
 * Constaté (MoltenVK 1.4.2, M4) : modes de présentation « fifo,immediate » sur les deux surfaces — le défaut mailbox
 * du Mac retombe sur fifo (journalisé).
 * SKIP (77) sans MoltenVK (backend inactif). Un sc2v_liberer_essai qui attendrait le verrou bloquerait : délai ctest.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc2_d3d9v.h"
#include "sc2_vk_presente.h"

int d3dv_present_reel(int id, const char *chemin, double moy[3]);   /* backend renommé (presentation.cmake) */
uint64_t sc2v_presentees(void);                                      /* branche Apple (mac/sc2_mac.h) */
int sc2v_liberer_essai(void);
void sc2v_visible(int visible);

static int g_pass, g_fail;
#define CHECK(cond, ...) do { char m_[300]; snprintf(m_, sizeof m_, __VA_ARGS__); \
                              if (cond) { g_pass++; printf("PASS  %s\n", m_); } \
                              else { g_fail++; printf("FAIL  %s\n", m_); } } while (0)

/* ---- journal : tout ce que backend, présentateur et trace écrivent, gardé pour les contrôles ---------------------- */
static char g_jl[1 << 20]; static size_t g_jn;
static int g_essai_sous_verrou = 2;                  /* 2 : pas encore essayé ; sinon le rendu de sc2v_liberer_essai */
static void garder(const char *l)
{
    printf("  | %s\n", l);
    size_t n = strlen(l);
    if (g_jn + n + 2 < sizeof g_jl) { memcpy(g_jl + g_jn, l, n); g_jn += n; g_jl[g_jn++] = '\n'; g_jl[g_jn] = 0; }
    /* « contrôle frame n°100 » est journalisé SOUS le verrou du présentateur, sur le fil qui présente */
    if (g_essai_sous_verrou == 2 && strstr(l, "contrôle frame n°100 "))
        g_essai_sous_verrou = sc2v_liberer_essai();
}
void sc2_log(const char *fmt, ...)
{
    char l[2048]; va_list a; va_start(a, fmt); vsnprintf(l, sizeof l, fmt, a); va_end(a);
    garder(l);
}
static void journal_backend(const char *fmt, ...)
{
    char l[2048]; va_list a; va_start(a, fmt); vsnprintf(l, sizeof l, fmt, a); va_end(a);
    garder(l);
}
static int compter(const char *motif)
{
    int n = 0;
    for (const char *p = g_jl; (p = strstr(p, motif)); p += strlen(motif)) n++;
    return n;
}
static const char *ligne_de(const char *motif, char *o, size_t cap)
{
    const char *p = strstr(g_jl, motif);
    if (!p) { snprintf(o, cap, "(absente)"); return o; }
    while (p > g_jl && p[-1] != '\n') p--;
    size_t n = strcspn(p, "\n");
    snprintf(o, cap, "%.*s", (int)(n < cap ? n : cap - 1), p);
    return o;
}

/* ---- fournis par la couche plate-forme et l'hôte dans recomp-sc2 ---------------------------------------------------- */
void sc2a_cible_vue(int id, uint32_t w, uint32_t h, int profondeur) { (void)id; (void)w; (void)h; (void)profondeur; }
static int g_bascules;
void sc2a_geo_invalider(void) { g_bascules++; }      /* appelé par basculer() */

#define W 1280
#define H 720

/* ---- CAMetalLayer par le runtime Objective-C (le test reste en C) --------------------------------------------------- */
typedef struct { double w, h; } taille_cg;          /* CGSize (arm64 : deux double) */
typedef struct { double x, y, w, h; } rect_cg;       /* CGRect */
void *objc_getClass(const char *nom);
void *sel_registerName(const char *nom);
void objc_msgSend(void);
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *pool);
static void *calque_creer(uint32_t w, uint32_t h)
{
    void *cls = objc_getClass("CAMetalLayer");
    if (!cls) return NULL;
    void *(*msg)(void *, void *) = (void *(*)(void *, void *))objc_msgSend;
    void *c = msg(msg(cls, sel_registerName("alloc")), sel_registerName("init"));   /* +1, sans pool */
    if (!c) return NULL;
    /* MoltenVK tire currentExtent de bounds × contentsScale (un calque neuf : 0×0, swapchain jamais créée), pas de
     * drawableSize : les deux sont posés, échelle 1 */
    ((void (*)(void *, void *, rect_cg))objc_msgSend)(c, sel_registerName("setBounds:"), (rect_cg){ 0, 0, w, h });
    ((void (*)(void *, void *, double))objc_msgSend)(c, sel_registerName("setContentsScale:"), 1.0);
    ((void (*)(void *, void *, taille_cg))objc_msgSend)(c, sel_registerName("setDrawableSize:"), (taille_cg){ w, h });
    return c;
}
static void calque_liberer(void *c)
{
    if (c) ((void (*)(void *, void *))objc_msgSend)(c, sel_registerName("release"));
}

/* d3dv_present de sc2_android_hooks.c, réduit à la voie swapchain : 0 = présentée */
static int present(int bb, unsigned n)
{
    int vol = sc2v_actif();
    if (vol) d3dv_semaphore_rendu_demander();
    int r = d3dv_present_reel(bb, NULL, NULL);
    if (!r && sc2v_actif() && !sc2v_presenter(bb, n)) return 0;
    if (vol) sc2v_consommer_rendu();
    return -1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setenv("SC2_SWAPCHAIN", "1", 1);
    setenv("SC2_SWAPCHAIN_CONTROLE", "1", 1);
    unsetenv("SC2_PRESENT"); unsetenv("SC2_BARRE"); unsetenv("SC2_VK_SYNCHRONE");
    d3dv_journal = journal_backend;
    if (!d3dv_init()) { printf("SKIP  backend Vulkan indisponible (MoltenVK absent ?)\n"); return 77; }
    printf("device : %s\n", d3dv_nom_device());
    char l[600];
    CHECK(compter("[VK] bibliothèque : ") == 1 && compter("libMoltenVK.dylib (MoltenVK direct, sans loader)") == 1,
          "sc2v_dladdr : bibliothèque du vrai gipa malgré l'interposition — %s", ligne_de("[VK] bibliothèque", l, sizeof l));
    CHECK(compter("[VKP] SC2_SWAPCHAIN=1") == 1, "chargeur interposé (SC2_SWAPCHAIN=1)");
    CHECK(compter("[VKP] instance : ") == 1 && compter(" ajoutées") == 1 && strstr(g_jl, "VK_EXT_headless_surface"),
          "instance : extensions de surface ajoutées, headless compris — %s", ligne_de("[VKP] instance", l, sizeof l));
    CHECK(sc2v_actif(), "présentateur actif (instance et device interposés)");
    if (!sc2v_actif()) { printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail); return 1; }

    sc2v_fenetre(NULL, W, H);                        /* banc : surface sans écran (sc2m_banc) */
    int bb = d3dv_cible(0x1000, W, H, 22 /* X8R8G8B8 */, 0);   /* le d3dv_cible du présentateur : id → VkImage */
    CHECK(bb >= 0, "back buffer 1280×720 créé par le d3dv_cible du présentateur (n°%d)", bb);
    d3dv_rt(bb);
    sc2v_barre_voulue(1);                            /* sur Android : affichée au toucher ; sur Mac : jamais */

    int presentes = 0, sautees_ok = 1, autres_ok = 1;
    for (unsigned n = 1; n <= 100; n++) {
        if (n == 50) sc2v_visible(0);
        if (n == 53) sc2v_visible(1);
        d3dv_viewport(0, 0, W, H, 0.f, 1.f);
        d3dv_scene(1);
        d3dv_clear(0, NULL, D3DV_CLEAR_TARGET, 0xFF204060u, 1.f, 0);
        d3dv_scene(0);
        uint64_t avant = sc2v_presentees();
        int r = present(bb, n);
        int gpu = sc2v_presentees() != avant;
        if (n >= 50 && n <= 52) sautees_ok &= !r && !gpu;   /* rendu 0 (pas de copie CPU), rien présenté */
        else autres_ok &= !r && gpu;
        presentes += gpu;
    }
    CHECK(autres_ok && presentes == 97 && sc2v_presentees() == 97,
          "Present n° 1 à 100 hors occultation : présentés par la swapchain (%d, compteur %llu)", presentes,
          (unsigned long long)sc2v_presentees());
    CHECK(sautees_ok && compter("[AFFICHAGE] présentation suspendue (fenêtre occultée)") == 1
          && compter("[AFFICHAGE] présentation reprise au Present n°53 après 3 Present sautés") == 1,
          "sc2v_visible(0) aux n° 50-52 : présentation sautée (rendu 0, un journal par série), reprise au n° 53");
    static const unsigned ctl[3] = { 1, 10, 100 };
    for (int i = 0; i < 3; i++) {
        char m[64]; snprintf(m, sizeof m, "contrôle frame n°%u : ", ctl[i]);
        ligne_de(m, l, sizeof l);
        CHECK(strstr(l, "image de swapchain 1280x720, moyenne RGB de l'image 32.00 64.00 96.00") != NULL,
              "contrôle frame n°%u : moyenne RGB 32.00 64.00 96.00 (0xFF204060)", ctl[i]);
    }
    CHECK(compter("contrôle frame n°") == 3, "trois contrôles (n° 1, 10, 100), aucun pendant l'occultation");
    CHECK(!g_bascules && !compter("bascule"), "aucune bascule (%d appel(s) de sc2a_geo_invalider)", g_bascules);
    CHECK(compter("[VKP] surface sans écran (VK_EXT_headless_surface) créée (1280x720") == 1,
          "surface sans écran créée une fois — %s", ligne_de("[VKP] surface", l, sizeof l));
    ligne_de("[VKP] swapchain ", l, sizeof l);
    CHECK(strstr(l, "[VKP] swapchain 1280x720,") && (strstr(l, "mailbox") || strstr(l, "fifo")) && compter("[VKP] swapchain ") == 1,
          "swapchain 1280×720 créée une fois, mode réel au journal — %s", l);
    printf("      H7 (currentExtent d'une surface sans écran) : %s\n",
           strstr(l, "currentExtent 0xFFFFFFFF×0xFFFFFFFF") ? "0xFFFFFFFF (taille de sc2v_fenetre prise)" : "défini");
    ligne_de("[VKP] mode de présentation", l, sizeof l);
    CHECK(strstr(l, "SC2_PRESENT=(défaut mailbox") && (strstr(l, "mode de présentation mailbox ")
          || strstr(l, "mode de présentation fifo (SC2_PRESENT=(défaut mailbox) : non supporté, repli fifo")),
          "défaut mailbox sur Mac (ou repli fifo journalisé) — %s", l);
    uint32_t sw, sh, dw, dh;
    CHECK(!sc2v_geometrie(&sw, &sh, &dw, &dh) && sw == W && sh == H && dw == W && dh == H,
          "géométrie du blit 1280×720 → 1280×720");
    CHECK(g_essai_sous_verrou == -1, "sc2v_liberer_essai sous le verrou du présentateur (même fil) : -1, sans attendre (%d)",
          g_essai_sous_verrou);
    char e[160]; sc2v_etat_court(e, sizeof e);
    CHECK(!strcmp(e, "swapchain 1280x720"), "rien libéré par l'essai refusé (%s)", e);

    /* 2e surface sans écran, plus haute que l'image (bande de 304 lignes) : place pour la barre, qu'Android afficherait
     * ici après sc2v_barre_voulue(1) ; sur Mac, jamais. L'image reste centrée, les bandes noires. */
    sc2v_fenetre(NULL, W, 1024);
    d3dv_scene(1); d3dv_clear(0, NULL, D3DV_CLEAR_TARGET, 0xFF204060u, 1.f, 0); d3dv_scene(0);
    CHECK(!present(bb, 200) && sc2v_presentees() == 98, "nouvelle surface 1280×1024 : Present n° 200 présenté");
    ligne_de("contrôle frame n°200 : ", l, sizeof l);
    CHECK(strstr(l, "image de swapchain 1280x1024, moyenne RGB de l'image 32.00 64.00 96.00, bandes 0.00 (bandes présentes)")
          != NULL, "contrôle frame n°200 : image 32.00 64.00 96.00, bandes noires — %s", l);
    barre_geo g; memset(&g, 0, sizeof g);
    int rg = sc2v_barre_geo(&g);
    CHECK(!rg && !g.active && g.jeu.x == 0 && g.jeu.y == 152 && g.jeu.w == W && g.jeu.h == H,
          "barre de raccourcis inactive sur Mac malgré sc2v_barre_voulue(1) et la place (image du jeu %dx%d en (%d, %d))",
          g.jeu.w, g.jeu.h, g.jeu.x, g.jeu.y);
    CHECK(compter("[VKP] surface sans écran (VK_EXT_headless_surface) créée (1280x1024") == 1
          && compter("[VKP] swapchain 1280x1024,") == 1 && !compter("bascule"), "surface et swapchain recréées, sans bascule");

    /* 3e surface : un CAMetalLayer hors fenêtre (VK_EXT_metal_surface, la voie de l'app), 1280×720 en pixels. Créé par
     * le runtime Objective-C (QuartzCore est chargé par MoltenVK) ; classe absente : partie sautée, dite. */
    uint64_t gpu_attendu = 98;
    void *calque = calque_creer(W, H);
    if (!calque) printf("      CAMetalLayer indisponible : surface Metal non essayée\n");
    else {
        void *pool = objc_autoreleasePoolPush();
        sc2v_fenetre(calque, W, H);
        d3dv_scene(1); d3dv_clear(0, NULL, D3DV_CLEAR_TARGET, 0xFF204060u, 1.f, 0); d3dv_scene(0);
        int r3 = present(bb, 300);
        objc_autoreleasePoolPop(pool);
        gpu_attendu += !r3;
        CHECK(!r3 && sc2v_presentees() == 99, "surface Metal (CAMetalLayer hors fenêtre) : Present n° 300 présenté");
        CHECK(compter("[VKP] surface Metal (CAMetalLayer) créée (1280x720") == 1, "surface Metal créée une fois — %s",
              ligne_de("[VKP] surface Metal", l, sizeof l));
        ligne_de("création n°3 ", l, sizeof l);
        CHECK(strstr(l, "[VKP] swapchain 1280x720,") && !compter("bascule"), "swapchain Metal 1280×720, sans bascule — %s", l);
        ligne_de("contrôle frame n°300 : ", l, sizeof l);
        CHECK(strstr(l, "image de swapchain 1280x720, moyenne RGB de l'image 32.00 64.00 96.00") != NULL,
              "contrôle frame n°300 (Metal) : moyenne RGB 32.00 64.00 96.00 — %s", l);
    }

    CHECK(sc2v_liberer_essai() == 0, "sc2v_liberer_essai au repos : 0");
    sc2v_etat_court(e, sizeof e);
    CHECK(!strcmp(e, "swapchain pas encore créée"), "swapchain et surface libérées (%s)", e);
    calque_liberer(calque);
    CHECK(present(bb, 301) == -1 && sc2v_presentees() == gpu_attendu,
          "après la libération : plus rien de présenté (pas de fenêtre)");
    char b[700]; sc2v_bilan(b, sizeof b);
    printf("      bilan : %s\n", b);
    char att[64]; snprintf(att, sizeof att, "swapchain active : %llu frames présentées", (unsigned long long)gpu_attendu);
    CHECK(strstr(b, att) && strstr(b, "présentations sautées (fenêtre occultée) 3"), "bilan : %s, 3 sautées", att);
    d3dv_bilan();
    printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
