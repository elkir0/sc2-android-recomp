/* sc2_android_hooks.c — accroches de la plate-forme Android sur le runtime recomp-sc2 (passes 7a-7d).
 *
 * Le runtime (recomp-sc2/runtime/) n'est PAS modifié : tout passe par l'éditeur de liens
 * (--wrap=<symbole> : les appels inter-fichiers de <symbole> arrivent sur __wrap_<symbole>,
 * l'original reste joignable par __real_<symbole>). Le même fichier sert à l'APK et au banc
 * Linux/qemu (android/sim/) qui le valide sans téléphone.
 *
 *   mmap                 zone invitée DÉCALÉE (g_hote_delta, mode du portage macOS) : sur Android
 *                        le tas Java d'ART occupe le bas des 4 Go (« dalvik-main space » vers
 *                        0x12C00000) où le runtime pose son arène à adresse fixe. MAP_FIXED_NOREPLACE
 *                        dans NOTRE zone réservée devient MAP_FIXED.
 *   dlopen               « libvulkan.so.1 » (nom Linux du chargeur) → « libvulkan.so » (Android).
 *   d3dv_present         après chaque Present du moteur : relecture du back buffer → sc2a_frame() ;
 *                        passe 7b (SC2_SWAPCHAIN) : d'abord blit GPU vers la swapchain (sc2_vk_presente.c).
 *   (entrées)            passe 7d : plus d'accroche — le tactile passe par l'API runtime sc2_input.h (5f).
 *                        La demande de sortie est traitée sur le fil du jeu au Present.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "sc2_iat.h"
#include "sc2_d3d9v.h"
#include "sc2_android.h"
#include "sc2_trace_present.h"
#include "sc2_entrees_bt.h"                            /* passe 8h */
#ifdef SC2_SWAPCHAIN
#include "sc2_vk_presente.h"
#endif

#ifndef __APPLE__   /* V3 (M0, 22/10) : sur macOS, la zone invitée est posée par zone_hote_init (sc2_runtime.c) et Vulkan
                     * par charger_vk_apple (sc2_d3d9v.c) ; ld64 n'a pas de --wrap (renommages : mac/mac.cmake) */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* ---- zone invitée décalée --------------------------------------------------------------- */

#define ZONE_TAILLE (1ull << 32)          /* toute la plage 32 bits : un pointeur invité fou faute */
static uintptr_t g_zone;
void *__real_mmap(void *a, size_t n, int prot, int flags, int fd, off_t off);

/* Deux propriétés que le runtime suppose sans le dire (exactes quand g_hote_delta = 0) :
 *   - GVA_OF(p) = (uint32_t)p : l'adresse hôte TRONQUÉE est la VA invitée → zone alignée sur 4 Go ;
 *   - GPTR(0) == NULL : des stubs passaient GPTR(A(n)) d'un argument invité NULL (CreateEventW,
 *     OpenEventW — corrigés en passe 7b dans sc2_iat.c ; filet gardé pour les autres stubs, ex.
 *     sc2_d3d9.c) → les 64 premiers Ko de la zone (la « page nulle » de Windows) sont une page de
 *     zéros en LECTURE seule : un nom NULL y lit "" (= sans nom pour win32_compat), une écriture
 *     par pointeur nul faute toujours. Écart : une LECTURE invitée à l'adresse 0 rend 0 au lieu de
 *     fauter (cf. docs/PIPELINE-ANDROID.md §7). */
/* 4 Go alignés sur 4 Go : d'abord par indices (adresses multiples de 4 Go, sans MAP_FIXED : rien
 * d'existant n'est écrasé) — 4 Go de réservation seulement, compatible avec ulimit -v 8000000 du
 * banc ; à défaut, 8 Go réservés puis rognés. */
static uintptr_t reserver_alignee(void)
{
    const int fl = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    for (uintptr_t h = 0x1000000000ull; h <= 0x7000000000ull; h += ZONE_TAILLE) {   /* 64 Go .. 448 Go */
        void *z = __real_mmap((void *)h, ZONE_TAILLE, PROT_NONE, fl, -1, 0);
        if (z == MAP_FAILED) continue;
        if ((uintptr_t)z == h) return h;
        munmap(z, ZONE_TAILLE);
    }
    void *z = __real_mmap(NULL, 2 * ZONE_TAILLE, PROT_NONE, fl, -1, 0);
    if (z == MAP_FAILED) return 0;
    uintptr_t b = (uintptr_t)z, a = (b + ZONE_TAILLE - 1) & ~(uintptr_t)(ZONE_TAILLE - 1);
    if (a > b) munmap(z, a - b);
    if (b + 2 * ZONE_TAILLE > a + ZONE_TAILLE) munmap((void *)(a + ZONE_TAILLE), b + 2 * ZONE_TAILLE - (a + ZONE_TAILLE));
    return a;
}

int sc2a_zone_init(void)
{
    if (g_zone) return 0;
    uintptr_t a = reserver_alignee();
    if (!a) { perror("sc2a: réservation de la zone invitée (4 Go alignés)"); return -1; }
    if (__real_mmap((void *)a, 0x10000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *)a) {
        perror("sc2a: page nulle de la zone invitée"); return -1;
    }
    g_zone = a;
    g_hote_delta = g_zone;                /* VA invitée v ↔ adresse hôte g_zone + v */
    return 0;
}
uintptr_t sc2a_zone(void) { return g_zone; }

void *__real_mmap(void *a, size_t n, int prot, int flags, int fd, off_t off);
void *__wrap_mmap(void *a, size_t n, int prot, int flags, int fd, off_t off)
{
    uintptr_t p = (uintptr_t)a;
    if (g_zone && (flags & MAP_FIXED_NOREPLACE) && p >= g_zone && p + n <= g_zone + ZONE_TAILLE)
        flags = (flags & ~MAP_FIXED_NOREPLACE) | MAP_FIXED;   /* la zone est à nous : on la remplace */
    return __real_mmap(a, n, prot, flags, fd, off);
}

/* ---- Vulkan ---------------------------------------------------------------------------- */

void *__real_dlopen(const char *nom, int mode);
void *__wrap_dlopen(const char *nom, int mode)
{
#ifdef __ANDROID__
    if (nom && !strcmp(nom, "libvulkan.so.1")) nom = "libvulkan.so";
#endif
    return __real_dlopen(nom, mode);
}
#endif   /* !__APPLE__ */

/* ---- frames ----------------------------------------------------------------------------- */

static uint8_t *g_px; static size_t g_px_cap;
static uint32_t g_w, g_h;
static atomic_uint g_presents;

void sc2a_taille_ecran(uint32_t w, uint32_t h) { g_w = w; g_h = h; }

/* Passe 9d — observabilité de l'affichage.
 *   Present sans fenêtre : le moteur rend (et écrit ses PPM) alors qu'aucune ANativeWindow n'existe (écran éteint,
 *   verrouillé, activité en arrière-plan) — rien n'est relu ni posté. Le 1er de chaque série est journalisé.
 *   Taille des cibles : d3dv_lire écrit w×h×4 octets de la cible relue dans g_px, dimensionné sur g_w×g_h
 *   (SC2_ECRAN) ; une cible d'une autre taille est refusée (comptée, la 1re journalisée) au lieu de déborder.
 *   La taille est connue par --wrap=d3dv_cible (android/CMakeLists.txt) ; sans ce wrap (banc android/sim en copie
 *   CPU), __real_d3dv_cible faible reste nulle, aucune taille n'est connue et la relecture se fait comme avant. */
static atomic_ullong g_sans_fenetre, g_cible_refusee;
static struct { uint32_t w, h; } g_cible[256];

unsigned sc2a_presents(void) { return atomic_load(&g_presents); }
void sc2a_bilan_presents(uint64_t *sans_fenetre, uint64_t *cible_refusee)
{
    *sans_fenetre = atomic_load(&g_sans_fenetre); *cible_refusee = atomic_load(&g_cible_refusee);
}
void sc2a_cible_vue(int id, uint32_t w, uint32_t h, int profondeur)
{
    if (id >= 0 && id < 256 && !profondeur) { g_cible[id].w = w; g_cible[id].h = h; }
}
#ifndef SC2_SWAPCHAIN   /* APK swapchain : __wrap_d3dv_cible est celui de sc2_vk_presente.c, qui appelle sc2a_cible_vue */
int __real_d3dv_cible(uint64_t cle, uint32_t w, uint32_t h, uint32_t fmt, int profondeur) __attribute__((weak));
int __wrap_d3dv_cible(uint64_t cle, uint32_t w, uint32_t h, uint32_t fmt, int profondeur)
{
    int id = __real_d3dv_cible(cle, w, h, fmt, profondeur);
    sc2a_cible_vue(id, w, h, profondeur);
    return id;
}
#endif

/* Passe 10o — profil PERMANENT et léger de la boucle (5 lectures d'horloge par frame, aucun journal à lui seul) :
 *   jeu     fin du Present précédent → entrée du Present (moteur lifté + émission des draws par le backend)
 *   backend __real_d3dv_present (soumission de la frame + vkQueueWaitIdle : le temps GPU du rendu)
 *   gpu     sc2v_presenter (blit vers la swapchain + present + barrière)
 *   lire    d3dv_lire (copie CPU : relecture du back buffer)     ┐ voie
 *   peindre sc2a_frame (copie CPU : lock + copie + post)          ┘ copie CPU
 * Moyennes par frame depuis la ligne précédente, ajoutées à chaque ligne [FPS] (sc2a_profil_texte). SC2_TRACE_PRESENT
 * (passe 7p3) reste le détail fin (acquisition, enregistrement, soumission…). */
enum { PF_JEU, PF_BACKEND, PF_GPU, PF_LIRE, PF_PEINDRE, PF_N };
static double g_pf[PF_N], g_pf_sortie; static unsigned g_pf_n, g_pf_cpu;
static double pf_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
void sc2a_profil_texte(char *o, size_t cap)          /* fil du jeu (compter_fps), remet la fenêtre à zéro */
{
    unsigned n = g_pf_n ? g_pf_n : 1;
    snprintf(o, cap, "ms/frame : jeu %.1f, backend %.1f, présentation %s %.1f", g_pf[PF_JEU] / n, g_pf[PF_BACKEND] / n,
             g_pf_cpu ? "copie CPU" : "GPU", (g_pf[PF_GPU] + g_pf[PF_LIRE] + g_pf[PF_PEINDRE]) / n);
    if (g_pf_cpu) { size_t k = strlen(o); snprintf(o + k, cap - k, " (relecture %.1f + peinture %.1f) sur %u frame(s)",
                                                   g_pf[PF_LIRE] / n, g_pf[PF_PEINDRE] / n, g_pf_cpu); }
    memset(g_pf, 0, sizeof g_pf); g_pf_n = g_pf_cpu = 0;
}

static void livrer_deplacement(void);       /* entrées, plus bas */
int __real_d3dv_present(int id, const char *chemin, double moy[3]);
#ifdef SC2_SWAPCHAIN
extern void d3dv_semaphore_rendu_demander(void);     /* recomp-sc2/runtime/sc2_d3d9v.h (passe 10r, v13) */
#endif
/* 21/10 : PAUSE en arrière-plan — l'activité passe en pause (APP_CMD_PAUSE) : le fil du jeu s'arrête à son Present
 * suivant et attend la reprise ; la durée de l'arrêt est retirée de l'horloge du jeu (g_sc2_pause_ns, sc2_horloge.h) :
 * aucun saut de temps à la reprise. Son en pause pendant ce temps. Une demande de sortie débloque toujours le jeu. */
#include "sc2_horloge.h"
#include "sc2_audio_android.h"
static pthread_mutex_t g_susp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_susp_cv = PTHREAD_COND_INITIALIZER;
static int g_suspendu; static unsigned g_n_pauses; static double g_s_pauses;
void sc2a_suspendre(int s)
{
    pthread_mutex_lock(&g_susp_mu);
    if (g_suspendu != !!s) {
        g_suspendu = !!s;
        sc2aa_pause(g_suspendu);
        if (!g_suspendu) pthread_cond_broadcast(&g_susp_cv);
    }
    pthread_mutex_unlock(&g_susp_mu);
}
static void attendre_si_suspendu(void)          /* fil du jeu, en tête de Present */
{
    pthread_mutex_lock(&g_susp_mu);
    if (g_suspendu && !sc2a_sortie_demandee()) {
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        printf("[PAUSE] arrière-plan : jeu suspendu au Present n°%u\n", atomic_load(&g_presents) + 1);
        while (g_suspendu && !sc2a_sortie_demandee()) pthread_cond_wait(&g_susp_cv, &g_susp_mu);
        clock_gettime(CLOCK_MONOTONIC, &b);
        int64_t d = (int64_t)(b.tv_sec - a.tv_sec) * 1000000000 + (b.tv_nsec - a.tv_nsec);
        atomic_fetch_add(&g_sc2_pause_ns, d);
        g_n_pauses++; g_s_pauses += d / 1e9;
        printf("[PAUSE] reprise après %.1f s : horloge du jeu décalée d'autant (pauses : %u, %.1f s en tout)\n", d / 1e9,
               g_n_pauses, g_s_pauses);
    }
    pthread_mutex_unlock(&g_susp_mu);
}

int __wrap_d3dv_present(int id, const char *chemin, double moy[3])
{
    attendre_si_suspendu();
    double t0 = pf_ms(), t1;
    if (g_pf_sortie) g_pf[PF_JEU] += t0 - g_pf_sortie;
    g_pf_n++;
    sc2tp_entree_present();                         /* passe 7p3 : fin de JEU, début de BACKEND (SC2_TRACE_PRESENT) */
#ifdef SC2_SWAPCHAIN
    /* passe 10r (v13) : deux frames en vol — la soumission de cette frame signale « rendu », attendu par le blit */
    int vol = sc2v_actif();
    if (vol) d3dv_semaphore_rendu_demander();
#endif
    int r = __real_d3dv_present(id, chemin, moy);
    sc2tp_fin(TP_BACKEND);
    t1 = pf_ms(); g_pf[PF_BACKEND] += t1 - t0; t0 = t1;
    unsigned n = atomic_fetch_add(&g_presents, 1) + 1;
#ifdef SC2_SWAPCHAIN
    /* passe 7b : blit GPU vers la swapchain ; la copie CPU ne sert plus que de repli */
    if (!r && sc2v_actif()) {
        int pr = sc2v_presenter(id, n);
        t1 = pf_ms(); g_pf[PF_GPU] += t1 - t0; t0 = t1;
        if (!pr) { sc2a_frame(n, NULL, g_w, g_h); goto fin; }
    }
    if (vol) sc2v_consommer_rendu();                /* passe 10r (v13) : sans blit, le sémaphore « rendu » reste à attendre */
#endif
    static int en_serie, series;                    /* passe 9d : série de Present sans fenêtre en cours */
    if (!r && g_w && !sc2a_frame_voulue(n)) {
        atomic_fetch_add(&g_sans_fenetre, 1);
        if (!en_serie++ && series++ < 20)
            printf("[AFFICHAGE] Present n°%u : rendu sans fenêtre (aucune ANativeWindow : écran éteint/verrouillé ou "
                   "activité en arrière-plan) — rien n'est relu ni posté ; suivants comptés jusqu'au retour de la fenêtre\n", n);
    } else if (!r && g_w) {
        if (en_serie) printf("[AFFICHAGE] Present n°%u : fenêtre revenue après %d Present sans fenêtre\n", n, en_serie);
        en_serie = 0;
        uint32_t cw = id >= 0 && id < 256 ? g_cible[id].w : 0, ch = id >= 0 && id < 256 ? g_cible[id].h : 0;
        if (cw && (cw != g_w || ch != g_h)) {       /* d3dv_lire écrirait cw×ch×4 octets dans un tampon g_w×g_h×4 */
            if (atomic_fetch_add(&g_cible_refusee, 1) == 0)
                printf("[AFFICHAGE] Present n°%u : cible %d de %ux%u ≠ back buffer attendu %ux%u (SC2_ECRAN) — relecture "
                       "CPU refusée (suivantes comptées)\n", n, id, cw, ch, g_w, g_h);
        } else {
            size_t taille = (size_t)g_w * g_h * 4;
            if (taille > g_px_cap) { free(g_px); g_px = malloc(taille); g_px_cap = g_px ? taille : 0; }
            if (g_px && !d3dv_lire(id, g_px)) {
                t1 = pf_ms(); g_pf[PF_LIRE] += t1 - t0; t0 = t1; g_pf_cpu++;
                sc2a_frame(n, g_px, g_w, g_h);
                t1 = pf_ms(); g_pf[PF_PEINDRE] += t1 - t0; t0 = t1;
            }
        }
    }
#ifdef SC2_SWAPCHAIN
fin:
#endif
    livrer_deplacement();
    g_pf_sortie = pf_ms();
    sc2tp_sortie_present();
    if (sc2a_sortie_demandee()) sc2a_sortir("Present");
    return r;
}

/* ---- sortie ----------------------------------------------------------------------------- */

static atomic_int g_sortie;
void sc2a_demander_sortie(void)
{
    atomic_store(&g_sortie, 1);
    pthread_mutex_lock(&g_susp_mu); pthread_cond_broadcast(&g_susp_cv); pthread_mutex_unlock(&g_susp_mu);   /* 21/10 */
}
int  sc2a_sortie_demandee(void) { return atomic_load(&g_sortie); }

/* Sur le fil du jeu : bilan complet du runtime (sc2_stop → [BILAN] → exit(0)). */
void sc2a_sortir(const char *ou)
{
    static atomic_int une;
    if (atomic_exchange(&une, 1)) for (;;) pause();     /* un autre fil fait déjà le bilan */
    sc2a_avant_sortie();
    sc2_stop(0, "sortie demandée par la plate-forme — traitée au %s", ou);
}

/* ---- entrées : tactile → API runtime de la passe 5f (sc2_input.h) ---------------------------
 * Passe 7d : la glue n'imite plus la pompe à messages (--wrap=sc2_iat_p4_find retiré) ; elle pousse
 * des événements dans runtime/sc2_input_sim.c, qui sert PeekMessageW / GetMessageW / TranslateMessage /
 * DispatchMessageW / GetCursorPos / GetKeyboardState (+ GetAsyncKeyState / GetKeyState) et trace
 * chaque message livré à la wndproc du jeu ([INPUT]). Un seul chemin actif : pas de doubles événements.
 *
 * Activation : AVANT sc2_poc_main. sc2_iat_install() résout chaque import une fois pour toutes par
 * sc2_iat_p4_find → sc2_input_find, qui rend NULL tant que le module est inactif : activé après le
 * boot, les imports de la pompe resteraient les stubs de sc2_iat_p4.c (prouvé au banc, TRACE-PASSE7D).
 *
 * Déplacements : le jeu ne pompe qu'à chaque frame et la file du runtime est bornée (256) ; un doigt
 * qui glisse produit jusqu'à 120 événements/s. Le dernier déplacement est donc gardé et livré au plus
 * une fois par frame (au Present) ; appui et relâche sont livrés tout de suite, précédés de la
 * position exacte. */
#include "sc2_input.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_mx, g_my; static int g_move_en_attente;
static uint64_t g_injectes, g_moves_fusionnes;

void sc2a_entrees_activer(void) { sc2_input_activer(); }

void sc2a_tactile(int action, int32_t x, int32_t y)
{
    pthread_mutex_lock(&g_mu);
    if (action == SC2A_BOUGE) {
        if (g_move_en_attente) g_moves_fusionnes++;
        g_mx = x; g_my = y; g_move_en_attente = 1;
    } else {
        g_move_en_attente = 0;                    /* la position de l'appui/relâche la remplace */
        sc2_input_souris(x, y);
        sc2_input_bouton(0, action == SC2A_APPUI);
        g_injectes += 2;
    }
    pthread_mutex_unlock(&g_mu);
}

void sc2a_touche(uint32_t vk)
{
    pthread_mutex_lock(&g_mu);
    sc2_input_touche(vk, 1); sc2_input_touche(vk, 0);
    g_injectes += 2;
    pthread_mutex_unlock(&g_mu);
}

/* Passe 8h : souris et clavier (sc2_entrees_bt.c, fil principal). Même discipline que le tactile : déplacements
 * fusionnés (livrés au Present), appui/relâche/molette tout de suite, précédés de la position exacte. */
void sc2a_deplacer(int32_t x, int32_t y) { sc2a_tactile(SC2A_BOUGE, x, y); }
void sc2a_bouton(int bouton, int bas, int32_t x, int32_t y)
{
    pthread_mutex_lock(&g_mu);
    g_move_en_attente = 0;
    sc2_input_souris(x, y);
    sc2_input_bouton(bouton, bas);
    g_injectes += 2;
    pthread_mutex_unlock(&g_mu);
}
void sc2a_molette(int32_t delta, int horizontale, int32_t x, int32_t y)
{
    pthread_mutex_lock(&g_mu);
    g_move_en_attente = 0;
    sc2_input_souris(x, y);
    sc2_input_molette(delta, horizontale);
    g_injectes += 2;
    pthread_mutex_unlock(&g_mu);
}
void sc2a_cle(uint32_t vk, int bas)
{
    pthread_mutex_lock(&g_mu);
    sc2_input_touche(vk, bas);
    g_injectes++;
    pthread_mutex_unlock(&g_mu);
}

/* Fil du jeu, à chaque Present : le dernier déplacement en attente. */
static void livrer_deplacement(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_move_en_attente) { sc2_input_souris(g_mx, g_my); g_injectes++; g_move_en_attente = 0; }
    pthread_mutex_unlock(&g_mu);
}

void sc2a_bilan_entrees(uint64_t *injectes, uint64_t *fusionnes)
{
    pthread_mutex_lock(&g_mu); *injectes = g_injectes; *fusionnes = g_moves_fusionnes; pthread_mutex_unlock(&g_mu);
}
