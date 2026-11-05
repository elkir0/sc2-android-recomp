/* mac/sc2_mac_hote.c — hôte macOS, partie C (V3, lot M0, 22/10 ; v3-critique.md §3.4, v3-conception-A.md §1.2-1.3, §2.2).
 *
 * La couche plate-forme d'Android est compilée EN PLACE dans recomp-sc2 (android/jni/sc2_android_hooks.c,
 * sc2_vk_presente.c, sc2_trace_present.c ; renommages de mac/mac.cmake au lieu des --wrap). Ce fichier lui fournit ce
 * que l'activité Android lui fournit (sc2a_frame_voulue, sc2a_frame, sc2a_avant_sortie, sc2a_geo_invalider), plus :
 *   - le mode banc (sc2m_banc) : le fil d'exécution d'avant la V3, sur le fil principal, à un crochet de Present près ;
 *   - le fil du jeu du mode fenêtre (sc2m_jeu_lancer), la préparation du fil principal, la ligne [FPS], le bilan ;
 *   - la mémoire du code de sortie (greffe 1, crochet de M10) ;
 *   - les TALONS FAIBLES des lots suivants (fin du fichier) : une définition forte d'un autre objet l'emporte.
 * Aucun appel AppKit ici : C pur, appelable depuis tout fil.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sc2_host.h"               /* sc2_log, sc2_thread_fault_stack, sc2_shadow, g_sc2_ss (runtime) */
#include "sc2_mac.h"
#include "sc2_trace_present.h"
#include "sc2_vk_presente.h"

/* ---- mode ---------------------------------------------------------------------------------------------------------- */

static atomic_int g_mode = -1;
int sc2m_mode_fenetre(void)
{
    int m = atomic_load(&g_mode);
    if (m < 0) {
        const char *e = getenv("SC2_FENETRE");
        m = e && !strcmp(e, "1");
        atomic_store(&g_mode, m);
    }
    return m;
}

/* SC2_MAC_SON : 0, 1 ou muet (sortie réelle au volume nul : M6) ; absente : 1 en fenêtre, 0 au banc. */
static int son_voulu(int fenetre)
{
    const char *e = getenv("SC2_MAC_SON");
    if (!e || !*e) return fenetre;
    return !strcmp(e, "1") || !strcmp(e, "muet");
}

/* ---- bilan de l'hôte ------------------------------------------------------------------------------------------------ */

static atomic_uint g_presentees, g_derniere;
static atomic_int g_bilan_fait, g_bilan_pose;

unsigned sc2m_frames_presentees(void) { return atomic_load(&g_presentees); }

void sc2m_bilan_hote(void)
{
    if (atomic_exchange(&g_bilan_fait, 1)) return;
    uint64_t inj = 0, fus = 0, sf = 0, cr = 0;
    sc2a_bilan_entrees(&inj, &fus);
    sc2a_bilan_presents(&sf, &cr);
    printf("[MAC] sortie (%s) : %u Present, %u présentés par la swapchain (dernier n°%u) ; Present sans fenêtre %llu, "
           "cibles refusées %llu ; %llu événements injectés dans sc2_input, %llu déplacements fusionnés\n",
           sc2m_mode_fenetre() ? "fenêtre" : "banc", sc2a_presents(), atomic_load(&g_presentees),
           atomic_load(&g_derniere), (unsigned long long)sf, (unsigned long long)cr, (unsigned long long)inj,
           (unsigned long long)fus);
    char b[640]; sc2v_bilan(b, sizeof b); printf("[MAC] %s\n", b);
    char e[512] = ""; sc2m_entrees_bilan(e, sizeof e); if (e[0]) printf("[MAC] %s\n", e);
    char t[640]; sc2tp_bilan(t, sizeof t); if (t[0]) printf("[BILAN] [PRESENT] tout le run : %s\n", t);
    sc2aa_arreter();                                 /* idempotent ; talon sans M6 */
    fflush(stdout);
}

/* Deux gestionnaires atexit, jamais dans le canon par défaut (qui reste sans une ligne de plus) :
 *  - « bilan seul » : posé d'avance par sc2m_banc (SC2_SWAPCHAIN, SC2_TRACE_PRESENT, SC2_MAC_SON), donc AVANT le
 *    chargement de MoltenVK, et exécuté APRÈS ses finaliseurs (ordre inverse d'atexit / __cxa_atexit) : il ne fait que
 *    le bilan (texte, aucun appel Vulkan), et rien si la sortie ci-dessous l'a déjà fait (sc2m_bilan_hote idempotent) ;
 *  - « sortie » (M5, revue de M0) : posé au 1er Present RÉELLEMENT présenté par la swapchain (mode fenêtre, ou banc
 *    avec SC2_SWAPCHAIN=1), donc après le chargement de MoltenVK et son 1er vkQueuePresentKHR : exécuté AVANT ses
 *    finaliseurs. Bilan, puis swapchain et surface libérées tant que MoltenVK est entier — sauf sur une faute : code
 *    noté ≥ 128 (crochet de M10) → rien ; aucun code noté (banc, ou M10 absent : faute possible, et le fil fautif, qui
 *    exécute lui-même ces gestionnaires, peut tenir le verrou du présentateur) → sans attendre ce verrou. */
static void bilan_atexit(void) { sc2m_bilan_hote(); }
static void bilan_poser(void) { if (!atomic_exchange(&g_bilan_pose, 1)) atexit(bilan_atexit); }

static atomic_int g_sortie_posee;
static void sortie_atexit(void)
{
    sc2m_bilan_hote();
    int c = sc2m_sortie_code();
    if (c >= 128) return;                            /* faute notée : rien qui puisse bloquer */
    if (c >= 0) { sc2v_fenetre(NULL, 0, 0); return; }   /* sortie notée (M10) : le présentateur est au repos */
    if (sc2v_liberer_essai())
        printf("[MAC] sortie : verrou du présentateur tenu (faute pendant la présentation ?) — swapchain laissée telle "
               "quelle\n");
    else
        printf("[MAC] sortie : swapchain et surface libérées avant les finaliseurs de MoltenVK\n");
    fflush(stdout);
}

/* ---- [FPS] (format de sc2_android_main.c, compter_fps), fil du jeu ------------------------------------------------- */

static void compter_fps(unsigned n)
{
    static struct timespec t0, tp; static unsigned n0, np; static double periode = -1;
    if (periode < 0) { const char *e = getenv("SC2_FPS_PERIODE"); periode = e ? atof(e) : 5.0; }
    if (periode <= 0) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    if (!n0) { t0 = tp = t; n0 = np = n; return; }
    double dt = (double)(t.tv_sec - tp.tv_sec) + (double)(t.tv_nsec - tp.tv_nsec) / 1e9;
    if (dt < periode) return;
    double tt = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
    char voie[160], prof[200];
    sc2v_etat_court(voie, sizeof voie);
    sc2a_profil_texte(prof, sizeof prof);
    sc2_log("[FPS] %.1f images/s sur %.1f s (frames %u→%u, swapchain ; %s) ; moyenne depuis la 1re : %.1f ; %s",
            (n - np) / dt, dt, np, n, voie, tt > 0 ? (n - n0) / tt : 0.0, prof);
    tp = t; np = n;
}

/* ---- fournis à la couche plate-forme (sc2_android.h), fil du jeu ---------------------------------------------------- */

int sc2a_frame_voulue(unsigned n) { (void)n; return 0; }   /* pas de copie CPU sur Mac : une bascule fige l'image */

void sc2a_frame(unsigned n, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    (void)w; (void)h;
    if (rgba) return;                                /* copie CPU : jamais demandée (sc2a_frame_voulue) */
    static uint64_t vues;                            /* fil du jeu : frames présentées par le GPU déjà comptées */
    uint64_t p = sc2v_presentees();
    if (p != vues) {                                 /* égal : présentation sautée (fenêtre occultée, sc2v_visible) */
        vues = p;
        atomic_fetch_add(&g_presentees, 1);
        atomic_store(&g_derniere, n);
        if (!atomic_exchange(&g_sortie_posee, 1)) atexit(sortie_atexit);
    }
    compter_fps(n);
}

/* Fil du jeu, au Present (sc2a_sortir) ; ou fil du secours de M7 (le présentateur peut alors tenir son verrou :
 * sc2v_fenetre attend, le secours de 8 s tranche). Aucun appel AppKit. */
void sc2a_avant_sortie(void)
{
    sc2m_bilan_hote();
    sc2v_fenetre(NULL, 0, 0);                        /* swapchain et surface libérées AVANT les finaliseurs de MoltenVK */
}

/* Référence faible du présentateur (basculer, sous son verrou) : sur Android, la géométrie de la copie CPU est
 * reposée ; le Mac n'a pas de copie CPU, l'image reste figée jusqu'au réarmement par la fenêtre (sc2v_fenetre, M7). */
void sc2a_geo_invalider(void)
{
    static atomic_int n;
    if (!atomic_fetch_add(&n, 1))
        sc2_log("[MAC] bascule du présentateur : pas de copie CPU sur Mac, image figée jusqu'au réarmement de la "
                "swapchain par la fenêtre");
}

/* ---- mode banc -------------------------------------------------------------------------------------------------- */

int sc2m_banc(int argc, char **argv)
{
    int bilan = 0;
    const char *e = getenv("SC2_SWAPCHAIN");
    if (e && *e == '1') {                            /* surface sans écran : branche Apple du présentateur (M5) */
        unsigned w = 1280, h = 720, a, b;
        const char *ec = getenv("SC2_ECRAN");
        if (ec && sscanf(ec, "%ux%u", &a, &b) == 2 && a >= 320 && b >= 200 && a <= 8192 && b <= 8192) { w = a; h = b; }
        sc2v_fenetre(NULL, w, h);
        sc2_log("[MAC] banc : SC2_SWAPCHAIN=1 → surface sans écran %ux%u (présentateur)", w, h);
        bilan = 1;                                   /* bilan seul : la libération attend le 1er Present présenté */
    }
    if (son_voulu(0)) { sc2aa_demarrer(); bilan = 1; }
    if ((e = getenv("SC2_TRACE_PRESENT")) && *e == '1') bilan = 1;
    if (getenv("SC2_MAC_EVENEMENTS"))
        sc2_log("[MAC] banc : SC2_MAC_EVENEMENTS ignorée (mode fenêtre seulement : SC2_FENETRE=1)");
    if (bilan) bilan_poser();
    return sc2_poc_main(argc, argv);
}

/* ---- environnement d'origine (relance) ------------------------------------------------------------------------------ */

extern char **environ;
static char **g_env;

void sc2m_env_capturer(void)
{
    if (g_env) return;
    size_t n = 0;
    while (environ && environ[n]) n++;
    char **c = calloc(n + 1, sizeof *c);
    if (!c) return;
    for (size_t i = 0, k = 0; i < n; i++)
        if ((c[k] = strdup(environ[i]))) k++;        /* une copie refusée est omise : la liste reste terminée par NULL */
    g_env = c;
}
char *const *sc2m_env_origine(void) { return g_env; }

/* ---- fil principal, fil du jeu ------------------------------------------------------------------------------------- */

static sc2_shadow g_ss_vide;                         /* n = 0 : une faute du fil principal n'imprime aucun cadre du jeu */

void sc2m_fil_principal_preparer(void)
{
    sc2_thread_fault_stack();                        /* sigaltstack de CE fil : sigaction vaut pour tout le processus */
    g_sc2_ss = &g_ss_vide;
}

static char g_exe[1100];
static atomic_int g_demande, g_lance, g_fini;

int sc2m_jeu_lance(void) { return atomic_load(&g_lance); }
int sc2m_jeu_fini(void) { return atomic_load(&g_fini); }

static void *fil_jeu(void *p)
{
    (void)p;
    pthread_setname_np("sc2-jeu");                   /* macOS : nom du fil appelant */
    sc2a_entrees_activer();                          /* AVANT sc2_iat_install (sc2_android_hooks.c, entrées) */
    if (son_voulu(1)) sc2aa_demarrer();              /* avant le 1er System::update */
    sc2_log("[MAC] fil du jeu « sc2-jeu » : SC2.exe = %s", g_exe);
    char *argv[] = { "recomp-sc2", g_exe, NULL };
    int r = sc2_poc_main(2, argv);                   /* ne revient qu'en cas d'échec de chargement */
    sc2_log("[MAC] le runtime a rendu %d sans exit() (image introuvable ?)", r);
    atomic_store(&g_fini, 1);
    sc2m_jeu_rendu(r);
    return NULL;
}

int sc2m_jeu_lancer(const char *exe)
{
    if (!exe || atomic_exchange(&g_demande, 1)) return -1;
    snprintf(g_exe, sizeof g_exe, "%s", exe);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20);       /* comme sc2_android_main.c (lancer_jeu) */
    pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int r = pthread_create(&t, &at, fil_jeu, NULL);
    pthread_attr_destroy(&at);
    if (r) {
        sc2_log("[MAC] pthread_create(fil du jeu) : erreur %d", r);
        atomic_store(&g_fini, 1);
        return -1;
    }
    atomic_store(&g_lance, 1);
    return 0;
}

/* ---- code de sortie (greffe 1 : noté par le crochet de M10) -------------------------------------------------------- */

static atomic_int g_code = -1;
void sc2m_sortie_noter(int code) { atomic_store(&g_code, code); }
int  sc2m_sortie_code(void) { return atomic_load(&g_code); }

void sc2m_sortie_sans_finaliseurs(void)
{
    int c = sc2m_sortie_code();
    if (c < 0) return;                               /* aucun code noté (crochet absent) : exit finit avec le sien */
    printf("[MAC] code de sortie %d\n", c);
    fflush(NULL);
    _exit(c);
}

/* ==== TALONS FAIBLES des lots suivants (v3-critique.md §3.4, M0) ====================================================
 * Des DÉFINITIONS faibles : la définition forte du lot (son propre fichier) l'emporte au lien. Comportement neutre :
 * le banc n'en appelle aucun dans le canon par défaut. */
#define SC2M_TALON __attribute__((weak))

/* M7 — hôte Cocoa */
SC2M_TALON int sc2m_fenetre_lancer(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[MAC] mode fenêtre absent de ce binaire (hôte Cocoa : lot M7) — SC2_FENETRE absente ou 0 pour le banc\n");
    fflush(stdout);
    return 3;
}
SC2M_TALON void sc2m_jeu_rendu(int rc)
{
    sc2_log("[MAC] fin sans hôte Cocoa (lot M7) : code %d", rc ? rc : 4);
    exit(rc ? rc : 4);
}
SC2M_TALON void sc2m_ca_vider(void) { }

/* M5 — présentateur */
SC2M_TALON int sc2v_dladdr(const void *a, Dl_info *i) { return dladdr(a, i); }
SC2M_TALON void sc2v_visible(int visible) { (void)visible; }

/* M6 — son (android/jni/sc2_audio_android.c, branche __APPLE__, quand SC2_MAC_SON_COREAUDIO passe à ON) */
SC2M_TALON int sc2aa_demarrer(void)
{
    sc2_log("[MAC] son absent de ce binaire (sortie CoreAudio : lot M6) — SC2_MAC_SON sans effet");
    return -1;
}
SC2M_TALON void sc2aa_arreter(void) { }
SC2M_TALON void sc2aa_pause(int pause) { (void)pause; }

/* M8 — entrées Cocoa */
SC2M_TALON void sc2m_entree(void *nsevent, void *vue) { (void)nsevent; (void)vue; }
SC2M_TALON void sc2m_entrees_reinitialiser(void) { }
SC2M_TALON void sc2m_entrees_resync(void) { }
SC2M_TALON void sc2m_entrees_bilan(char *o, size_t cap) { if (cap) o[0] = 0; }

/* M9 — lanceur */
SC2M_TALON void sc2m_lanceur_menu(void *nsmenu_principal) { (void)nsmenu_principal; }
SC2M_TALON int sc2m_lanceur_install_choisir(char *install, size_t cap)
{
    if (cap) install[0] = 0;
    sc2_log("[MAC] choix de l'installation absent de ce binaire (lanceur : lot M9) — poser SC2_INSTALL");
    return -1;
}

/* M10 — sortie : la variable du runtime (sc2_runtime.c la définira, NULL, et l'appellera dans sc2_stop) et le crochet */
SC2M_TALON void (*sc2_sortie_crochet)(int code) = NULL;
SC2M_TALON void sc2m_sortie_crochet(int code) { (void)code; }
