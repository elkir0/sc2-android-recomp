/* test_plateforme_ordre.c — V3, lot M7 (24/10/2026) : ordre du crochet de Present de la couche plate-forme
 * (android/jni/sc2_android_hooks.c, compilée TELLE QUELLE, avec les renommages de mac/mac.cmake et SC2_SWAPCHAIN=1),
 * contre des bouchons du backend, du présentateur, de l'hôte, du son et du runtime (v3-critique.md §3.4 fiche M7,
 * critère 8 ; A §1.4 et B §1.5 : « attendre_si_suspendu → réel → présenter | consommer → livraison → sortie »).
 *
 * Quatre cas, chacun dans un processus fils (les statiques de la couche repartent de zéro ; sc2a_sortir n'agit qu'une
 * fois par processus) :
 *   1. normal : present_reel, puis presenter, puis frame(NULL), puis livraison du déplacement de souris en attente ;
 *   2. pause (sc2a_suspendre(1)) : le Present suivant attend sans appeler le backend, le son est en pause ; reprise :
 *      le Present se termine et g_sc2_pause_ns augmente de la durée de l'arrêt ;
 *   3. sortie demandée PENDANT la pause : le fil se réveille, présente encore une image, puis sc2a_avant_sortie, puis
 *      sc2_stop(0) ;
 *   4. présentation en échec (sc2v_presenter rend -1) : sc2v_consommer_rendu est appelé (sémaphore « rendu » demandé
 *      par la couche, jamais attendu par un blit), et aucune frame n'est comptée présentée.
 * Plus un témoin : un bogue injecté dans l'ordre attendu (cas 1, livraison attendue AVANT la présentation) doit être
 * vu comme un échec par le même comparateur (SC2_ORDRE_TEMOIN=1, test_plateforme_ordre_temoin à sortie inversée).
 * Sortie 0 = conforme. Aucun Vulkan, aucun AppKit : POSIX seulement. */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sc2_android.h"
#include "sc2_entrees_bt.h"                   /* sc2a_deplacer */
#include "sc2_horloge.h"

int d3dv_present(int id, const char *chemin, double moy[3]);   /* = __wrap_d3dv_present de la couche (mac.cmake) */

/* ---- journal des appels des bouchons ---------------------------------------------------------------------------- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_ev[64][48]; static int g_n;
static void ev(const char *fmt, ...)
{
    pthread_mutex_lock(&g_mu);
    if (g_n < 64) { va_list a; va_start(a, fmt); vsnprintf(g_ev[g_n], sizeof g_ev[0], fmt, a); va_end(a); g_n++; }
    pthread_mutex_unlock(&g_mu);
}
static int n_ev(const char *nom)
{
    int k = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; i++) k += !strcmp(g_ev[i], nom);
    pthread_mutex_unlock(&g_mu);
    return k;
}
/* La sous-suite attendue apparaît dans cet ordre (d'autres appels peuvent s'intercaler) ; 1 = oui. */
static int dans_l_ordre(const char *const *att, int na, char *quoi, size_t cap)
{
    int i = 0;
    pthread_mutex_lock(&g_mu);
    for (int k = 0; k < g_n && i < na; k++) if (!strcmp(g_ev[k], att[i])) i++;
    size_t o = (size_t)snprintf(quoi, cap, "vu :");
    for (int k = 0; k < g_n && o < cap; k++) o += (size_t)snprintf(quoi + o, cap - o, " %s", g_ev[k]);
    pthread_mutex_unlock(&g_mu);
    return i == na;
}

/* ---- bouchons ----------------------------------------------------------------------------------------------------- */
static int g_actif = 1, g_presenter_rc = 0;
static atomic_int g_reel, g_fini;                     /* g_fini : le Present du fil d'essai est revenu (ou sorti) */
void sc2_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
int  d3dv_present_reel(int id, const char *chemin, double moy[3]) { (void)id; (void)chemin; (void)moy; atomic_fetch_add(&g_reel, 1); ev("present_reel"); return 0; }
int  d3dv_lire(int id, uint8_t *rgba) { (void)id; (void)rgba; ev("lire"); return -1; }
void d3dv_semaphore_rendu_demander(void) { ev("rendu_demande"); }
int  sc2v_actif(void) { return g_actif; }
int  sc2v_presenter(int id, unsigned n) { (void)id; (void)n; ev("presenter"); return g_presenter_rc; }
void sc2v_consommer_rendu(void) { ev("consommer_rendu"); }
int  sc2a_frame_voulue(unsigned n) { (void)n; return 0; }
void sc2a_frame(unsigned n, const uint8_t *rgba, uint32_t w, uint32_t h) { (void)n; (void)w; (void)h; ev(rgba ? "frame(rgba)" : "frame(NULL)"); }
void sc2a_avant_sortie(void) { ev("avant_sortie"); }
void sc2_stop(int code, const char *fmt, ...)
{
    (void)fmt;
    ev("stop(%d)", code);
    atomic_store(&g_fini, 1);
    pthread_exit(NULL);                               /* noreturn, comme exit() : le test attend ce fil */
}
void sc2aa_pause(int p) { ev("son_pause(%d)", p); }
void sc2tp_entree_present(void) { }
void sc2tp_sortie_present(void) { }
void sc2tp_fin(int phase) { (void)phase; }
void sc2tp_debut(int phase) { (void)phase; }
void sc2_input_activer(void) { }
void sc2_input_souris(int32_t x, int32_t y) { ev("souris(%d,%d)", x, y); }
void sc2_input_bouton(int b, int bas) { ev("bouton(%d,%d)", b, bas); }
void sc2_input_touche(uint32_t vk, int bas) { ev("touche(%u,%d)", vk, bas); }
void sc2_input_molette(int32_t d, int h) { ev("molette(%d,%d)", d, h); }

/* ---- cas ----------------------------------------------------------------------------------------------------------- */
static int g_pass, g_fail;
#define CHECK(c, ...) do { int c_ = (c); char m_[600]; snprintf(m_, sizeof m_, __VA_ARGS__);   /* c AVANT le message */ \
                           if (c_) { g_pass++; printf("PASS  %s\n", m_); } else { g_fail++; printf("FAIL  %s\n", m_); } } while (0)

static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void dormir_ms(int n) { struct timespec t = { n / 1000, (long)(n % 1000) * 1000000L }; nanosleep(&t, NULL); }
static void *un_present(void *p) { (void)p; double m[3]; d3dv_present(0, NULL, m); atomic_store(&g_fini, 1); return NULL; }
/* revue MIN-2 : un Present qui ne revient pas est un ÉCHEC NOMMÉ en ms millisecondes, pas un délai de ctest. 0 = revenu. */
static int attendre_present(pthread_t t, int ms, const char *quoi)
{
    for (int i = 0; i < ms / 10 && !atomic_load(&g_fini); i++) dormir_ms(10);
    if (atomic_load(&g_fini)) { pthread_join(t, NULL); return 0; }
    CHECK(0, "%s : le Present ne revient pas %d ms après (%s)", quoi, ms, "fil bloqué dans attendre_si_suspendu ?");
    fflush(stdout);
    _exit(1);                                         /* le fil reste bloqué : le processus fils s'arrête ici */
}

static int cas_normal(int temoin)
{
    sc2a_deplacer(10, 20);                           /* déplacement en attente : livré au Present */
    double m[3];
    d3dv_present(0, NULL, m);
    static const char *const att[] = { "present_reel", "presenter", "frame(NULL)", "souris(10,20)" };
    static const char *const bogue[] = { "present_reel", "souris(10,20)", "presenter" };   /* ordre faux injecté */
    char q[1200];
    int ok = temoin ? dans_l_ordre(bogue, 3, q, sizeof q) : dans_l_ordre(att, 4, q, sizeof q);
    CHECK(ok, "%s : present_reel → presenter → frame(NULL) → souris (%s)", temoin ? "TÉMOIN (ordre faux)" : "normal", q);
    CHECK(sc2a_presents() == 1, "normal : un Present compté (%u)", sc2a_presents());
    CHECK(!n_ev("consommer_rendu"), "normal : sémaphore « rendu » pris par la présentation, pas de consommation");
    return 0;
}

static int cas_pause(void)
{
    int64_t avant = atomic_load(&g_sc2_pause_ns);
    sc2a_suspendre(1);
    pthread_t t;
    pthread_create(&t, NULL, un_present, NULL);
    dormir_ms(300);
    CHECK(atomic_load(&g_reel) == 0, "pause : le Present attend sans appeler le backend (%d appel(s))", atomic_load(&g_reel));
    CHECK(n_ev("son_pause(1)") == 1, "pause : son en pause");
    double t0 = ms();
    sc2a_suspendre(0);
    attendre_present(t, 3000, "pause : reprise");
    int64_t d = atomic_load(&g_sc2_pause_ns) - avant;
    static const char *const att[] = { "son_pause(1)", "son_pause(0)", "present_reel", "presenter", "frame(NULL)" };
    char q[1200];
    CHECK(dans_l_ordre(att, 5, q, sizeof q), "pause : son repris, puis le Present se termine (%s)", q);
    CHECK(d >= 250000000 && d < 5000000000LL, "pause : g_sc2_pause_ns augmenté de %.0f ms (≥ 250 ; reprise en %.1f ms)",
          d / 1e6, ms() - t0);
    return 0;
}

static int cas_sortie_pendant_pause(void)
{
    sc2a_suspendre(1);
    pthread_t t;
    pthread_create(&t, NULL, un_present, NULL);
    dormir_ms(200);
    CHECK(atomic_load(&g_reel) == 0, "sortie en pause : le fil attend (%d appel(s) du backend)", atomic_load(&g_reel));
    sc2a_demander_sortie();                           /* réveille la pause */
    attendre_present(t, 3000, "sortie en pause : réveil"); /* le bouchon de sc2_stop termine le fil */
    static const char *const att[] = { "present_reel", "presenter", "frame(NULL)", "avant_sortie", "stop(0)" };
    char q[1200];
    CHECK(dans_l_ordre(att, 5, q, sizeof q), "sortie en pause : encore une image, puis sc2a_avant_sortie, puis sc2_stop(0) (%s)", q);
    CHECK(n_ev("stop(0)") == 1 && n_ev("avant_sortie") == 1, "sortie en pause : un seul bilan");
    return 0;
}

static int cas_echec(void)
{
    g_presenter_rc = -1;
    double m[3];
    d3dv_present(0, NULL, m);
    static const char *const att[] = { "rendu_demande", "present_reel", "presenter", "consommer_rendu" };
    char q[1200];
    CHECK(dans_l_ordre(att, 4, q, sizeof q), "échec : rendu demandé, réel, presenter (-1), puis sc2v_consommer_rendu (%s)", q);
    CHECK(!n_ev("frame(NULL)"), "échec : aucune frame comptée présentée");
    return 0;
}

static int lancer(const char *nom, int (*f)(void))
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) { alarm(20); f(); fflush(stdout); _exit(g_fail ? 1 : 0); }   /* alarm : filet si un cas se bloque */
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s  cas « %s »%s\n", ok ? "PASS" : "FAIL", nom, WIFSIGNALED(st) ? " (signal)" : "");
    return ok;
}
static int normal(void) { return cas_normal(0); }
static int temoin(void) { return cas_normal(1); }

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (getenv("SC2_ORDRE_TEMOIN")) return lancer("témoin : ordre faux injecté", temoin) ? 0 : 1;
    int ok = lancer("normal", normal) & lancer("pause et reprise", cas_pause)
           & lancer("sortie demandée pendant la pause", cas_sortie_pendant_pause) & lancer("présentation en échec", cas_echec);
    printf("RÉSULTAT : %s\n", ok ? "4 cas conformes" : "ÉCHEC");
    return ok ? 0 : 1;
}
