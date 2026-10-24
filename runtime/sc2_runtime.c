/* sc2_runtime.c — chargeur d'image, arène invitée, TEB/PEB/TLS, dispatch, arrêt.
 * Voir sc2_host.h pour le modèle. CARTE-INIT §0 pour les jalons de boot.
 */
#define _GNU_SOURCE
#ifdef __APPLE__
#define _XOPEN_SOURCE
#define _DARWIN_C_SOURCE 1   /* pthread_get_stack{addr,size}_np : masquées par _XOPEN_SOURCE sans lui */
#include <stdint.h>
#include <pthread.h>
static uint32_t mach_thread_id(void);
#define gettid() mach_thread_id()
/* Identifiant système du fil (petit entier, comme gettid sous Linux). pthread_self() tronqué donnait un pointeur à
 * bit de poids fort, qu'un identifiant de fil Windows n'a jamais (banc natif macOS, 19/10). */
static uint32_t mach_thread_id(void){ uint64_t t = 0; pthread_threadid_np(NULL, &t); return (uint32_t)t; }
#define MAP_FIXED_NOREPLACE MAP_FIXED   /* alias DANGEREUX (écrase) : borné par mmap_zone ci-dessous */
#endif
#include "sc2_host.h"
#include "sc2_d3d9.h"
#include "sc2_lifted.h"
#include "sc2_image.h"
#include "recomp_sc2_list.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#ifdef __APPLE__
/* Passe 7u — garde-fou : Darwin n'a pas MAP_FIXED_NOREPLACE, l'alias ci-dessus ÉCRASE en silence ce qui est déjà
 * mappé. Tout mmap FIXED de ce fichier doit donc viser [zone, zone+taille) (réservée par zone_hote_init) ; sinon
 * l'appel est tracé et refusé (MAP_FAILED), au lieu de corrompre la mémoire de l'hôte. */
static uintptr_t g_zone_hote, g_zone_taille;
static void *mmap_zone(void *a, size_t n, int prot, int flags, int fd, off_t off, const char *ou)
{
    if (flags & MAP_FIXED) {
        uintptr_t d = (uintptr_t)a;
        if (!g_zone_taille || d < g_zone_hote || n > g_zone_taille || d - g_zone_hote > g_zone_taille - n) {
            sc2_log("[ZONE] mmap FIXED REFUSÉ %p+0x%zX hors zone %p..%p (%s)", a, n, (void *)g_zone_hote,
                    (void *)(g_zone_hote + g_zone_taille), ou);
            return MAP_FAILED;
        }
    }
    return mmap(a, n, prot, flags, fd, off);
}
#define mmap(a, n, p, f, fd, o) mmap_zone(a, n, p, f, fd, o, __func__)
#endif
#include <unistd.h>

uint64_t g_sc2_dispatches;
__thread uint32_t g_sc2_teb;
uint32_t g_sc2_peb;
__thread sigjmp_buf *g_sc2_recover;
__thread uint32_t g_sc2_recover_va;
static sc2_shadow g_main_ss;
__thread sc2_shadow *g_sc2_ss = &g_main_ss;

/* ========================================================================== */
/* Trace                                                                      */
/* ========================================================================== */

void sc2_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* Passe 10n : SC2_TRACE (défaut 0 = runtime silencieux). Une lecture de getenv, puis un entier. */
int sc2_trace_actif(void)
{
    static volatile int v = -1;
    if (v < 0) { const char *e = getenv("SC2_TRACE"); v = e && strtoul(e, NULL, 0) ? 1 : 0; }
    return v;
}

/* ========================================================================== */
/* Table des fonctions liftées                                                 */
/* ========================================================================== */

#define DECL(a) void L_##a(CPU *c);
LIFTED_FUNCS(DECL)
#undef DECL
typedef struct { uint32_t va; void (*fn)(CPU *); } lifted_ent;
#define ENT(a) { 0x##a##u, L_##a },
static const lifted_ent g_lifted[] = { LIFTED_FUNCS(ENT) };
#undef ENT
#define N_LIFTED ((int)(sizeof g_lifted / sizeof g_lifted[0]))
static uint8_t g_executed[N_LIFTED];
static int g_n_executed;

int sc2_lifted_count(void)    { return N_LIFTED; }
int sc2_lifted_executed(void) { return g_n_executed; }

/* Passe 10r : va → indice en O(1). Chaque appel indirect du jeu (méthodes virtuelles, pointeurs de fonction) passe par
 * dispatch_un : la dichotomie sur 86 653 entrées y prenait 21 % du binaire (profil sc2prof, banc -O2). Adressage
 * ouvert, sondage linéaire, 2^18 cases remplies au tiers ; construite avant main (g_lifted est une constante). */
#define HACHE_BITS 18
#define HACHE_MASQUE ((1u << HACHE_BITS) - 1)
static int32_t g_hache[1u << HACHE_BITS];
static inline uint32_t hache_va(uint32_t va) { return (va * 2654435761u) >> (32 - HACHE_BITS); }
__attribute__((constructor)) static void hache_construire(void)
{
    memset(g_hache, 0xFF, sizeof g_hache);
    for (int i = 0; i < N_LIFTED; i++) {
        uint32_t h = hache_va(g_lifted[i].va);
        while (g_hache[h] >= 0) h = (h + 1) & HACHE_MASQUE;
        g_hache[h] = i;
    }
}
static int lifted_find(uint32_t va)
{
    for (uint32_t h = hache_va(va);; h = (h + 1) & HACHE_MASQUE) {
        int32_t i = g_hache[h];
        if (i < 0) return -1;
        if (g_lifted[i].va == va) return i;
    }
}
/* Passe 9h : fonction liftée la plus proche en dessous de va (adresse de retour → « L_xxxxxxxx+décalage » dans les
 * journaux des imports). Approximation : la fonction englobante si elle est liftée et contiguë. */
uint32_t sc2_lifted_precedente(uint32_t va)
{
    int lo = 0, hi = N_LIFTED - 1, r = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_lifted[m].va <= va) { r = m; lo = m + 1; } else hi = m - 1;
    }
    return r < 0 ? 0 : g_lifted[r].va;
}

/* Jalons du chemin de boot (CARTE-INIT §0) : imprimés au premier passage. */
static const struct { uint32_t va; const char *nom; } g_jalons[] = {
    { 0x00BAF6E0u, "entry" },
    { 0x01041A94u, "__security_init_cookie" },
    { 0x010410F3u, "__tmainCRTStartup" },
    { 0x008092D0u, "sc2_init_WinMain" },
    { 0x00809030u, "sc2_init_FiberMain (dans la fibre de 8 Mo)" },
    { 0x00808F80u, "sc2_init_GameMain" },
    { 0x008037E0u, "GameMain étape 1 : sc2_init_OptionsEtLogs" },
    { 0x00802610u, "GameMain étape 2 : sc2_init_InstanceUniqueEtStockage" },
    { 0x00BB0B70u, "  sc2_init_StockageStartup" },
    { 0x009F6BE0u, "GameMain étape 3 : sc2_init_GfxCreate" },
    { 0x00A00FC0u, "  Direct3DCreate9 (00A00FC0)" },
    { 0x00BB0830u, "GameMain étape 4 : options graphiques (00BB0830)" },
    { 0x00808BF0u, "GameMain étape 5 : sc2_init_CreateMainWindow" },
    { 0x008080F0u, "GameMain étape 6 : sc2_init_LauncherCheckEtJeu" },
    { 0x00809B90u, "  sc2_init_LauncherHandshakeIPC" },
    { 0x00807BA0u, "  sc2_init_GameSubsystems" },
    { 0x00807F20u, "  sc2_init_FrameUpdate (1re frame, avant la boucle)" },
    { 0x00803590u, "  sc2_init_FrameRender (BeginScene … Present)" },
    { 0x00808620u, "GameMain étape 7 : sc2_init_BoucleTick" },
    { 0x00803BC0u, "GameMain étape 8 : shutdown" },
};
static uint8_t g_jalon_vu[sizeof g_jalons / sizeof g_jalons[0]];

/* Passe 10s : chemins froids de dispatch_un (1re exécution, menus, erreurs, manques) hors de sc2_dispatch — intégrés,
 * leurs lectures __thread (g_sc2_ss->n dans les journaux) faisaient appeler le résolveur TLS à chaque dispatch. */
#define SC2_FROID __attribute__((noinline, cold))
SC2_FROID static void jalon(uint32_t va)
{
    for (unsigned i = 0; i < sizeof g_jalons / sizeof g_jalons[0]; i++)
        if (g_jalons[i].va == va && !g_jalon_vu[i]) {
            g_jalon_vu[i] = 1;
            sc2_log("[BOOT] -> 0x%08X %s   (dispatch n°%llu, profondeur %d)", va, g_jalons[i].nom,
                    (unsigned long long)g_sc2_dispatches, g_sc2_ss->n);
        }
}

/* Passe 10l (diagnostic) : SC2_NOUVELLES_FN=<frame>[:<max>] → à partir de la frame Present <frame>, chaque fonction
 * liftée exécutée pour la PREMIÈRE fois du run est journalisée (appelant, profondeur fantôme), <max> lignes au plus
 * (défaut 400). Montre le chemin neuf qu'ouvre une action (clic de bouton) sans connaître ses adresses. Sans la
 * variable : un test par première exécution, rien d'autre. */
uint32_t sc2_frames_presentees(void) __attribute__((weak));
SC2_FROID static void nouvelle_fn(CPU *c, uint32_t va)
{
    static int lu; static uint32_t des, max = 400, n;
    if (!lu) { lu = 1; const char *e = getenv("SC2_NOUVELLES_FN");
               if (e && *e) { des = (uint32_t)strtoul(e, NULL, 10); const char *p = strchr(e, ':'); if (p) max = (uint32_t)strtoul(p + 1, NULL, 10); } else max = 0; }
    if (n >= max || !sc2_frames_presentees) return;
    uint32_t fr = sc2_frames_presentees();
    if (fr < des) return;
    n++;
    uint32_t ret = rd32(c->esp), fn = sc2_lifted_precedente(ret);
    sc2_log("[NOUVELLE] L_%08X depuis 0x%08X (L_%08X+0x%X), profondeur %d, Present n°%u", va, ret, fn, ret - fn, g_sc2_ss->n, fr);
}

/* Passe 9j — menu de jeu natif (plan 9i, research/menu-options-natif.md) : journal borné à 20 lignes.
 *   L_00A2BD70 (thiscall) construit l'UI de jeu ; ecx = l'objet, qui range « GameMenuDialog/GameMenuDialogTemplate »
 *   en +0x3C4 et +0x3B4 (00A2C895, 00A2C8B8). On retient l'objet à l'entrée.
 *   L_00B7DC50 et L_00B80810 (thiscall, 1 argument) : méthodes de cadre qu'emploient les affichages du dialogue
 *   (L_00A29590, L_00A28EA0) et sa construction (0xB80810(0) à l'init). Journalisées quand ecx est l'un des deux
 *   cadres GameMenuDialog : l'ouverture ou la fermeture du menu devient visible au journal, avec la frame Present.
 *   L6 (24/10) — fin de partie : à l'écran Victory!/Defeat! (banc du 23/10, sow-longue), L_00A28EA0 appelle
 *   L_00B7DC50(1) sur le cadre +0x3B4 puis L_00B80810(1) sur le cadre +0x3C4. Le menu F10 ne montre ni l'un ni
 *   l'autre : sa L_00B80810(1) ne touche que +0x3B4 (depuis L_00B7DB50) et L_00B7DC50 y est appelé d'ailleurs
 *   qu'en L_00A28EA0. Chaque ouverture du dialogue appelle donc sc2_banc_fin_partie (faible, banc d'entrées)
 *   une seule fois et journalise « [MENU] fin de partie », hors plafond des 20 lignes. */
uint32_t sc2_frames_presentees(void) __attribute__((weak));
void sc2_banc_fin_partie(const char *quoi) __attribute__((weak));
SC2_FROID static void menu_jeu(CPU *c, uint32_t va)
{
    static uint32_t ui, fin; static unsigned n;
    uint32_t fr = sc2_frames_presentees ? sc2_frames_presentees() : 0xFFFFFFFFu, ret = rd32(c->esp), fn = sc2_lifted_precedente(ret);
    if (va == 0x00A2BD70u) {
        ui = c->ecx; fin = 0;
        if (n++ < 20) sc2_log("[MENU] UI de jeu construite : L_00A2BD70, objet 0x%08X, depuis 0x%08X (L_%08X+0x%X), Present n°%u",
                              ui, ret, fn, ret - fn, fr);
        return;
    }
    if (!ui) return;
    uint32_t f1 = rd32(ui + 0x3C4), f2 = rd32(ui + 0x3B4);
    if (!c->ecx || (c->ecx != f1 && c->ecx != f2)) return;
    uint32_t arg = rd32(c->esp + 4);
    if (!fin && arg == 1 && ((va == 0x00B80810u && c->ecx == f1) || (va == 0x00B7DC50u && fn == 0x00A28EA0u))) {
        char quoi[48];
        snprintf(quoi, sizeof quoi, va == 0x00B80810u ? "L_00B80810(1) cadre +0x3C4" : "L_00B7DC50(1) depuis L_00A28EA0");
        fin = 1;
        sc2_log("[MENU] fin de partie : %s, Present n°%u", quoi, fr);
        if (sc2_banc_fin_partie) sc2_banc_fin_partie(quoi);
    } else if (fin && arg == 0)
        fin = 0;   /* fermeture du dialogue : la prochaine ouverture sera signalée à nouveau */
    if (n++ < 20) sc2_log("[MENU] GameMenuDialog (%s, cadre 0x%08X) : L_%08X(%u) depuis 0x%08X (L_%08X+0x%X), Present n°%u",
                          c->ecx == f1 ? "+0x3C4" : "+0x3B4", c->ecx, va, rd32(c->esp + 4), ret, fn, ret - fn, fr);
}

/* Passe 10k — écrans NATIFS du front-end (glue). L'objet GlueUI est en [0x019322A0] ; il gère 6 écrans, gabarits de la
 * table 0x0157526C (paires début/fin, initialiseur 0x013CF900) : 0 ScreenBackground, 1 ScreenSingle, 2 ScreenReplay,
 * 3 ScreenCredits, 4 ScreenLoading, 5 ScreenMovie. [GlueUI+0x1DC] = masque des écrans affichés ; [GlueUI+0x1F0+4i] =
 * cadre de l'écran i (créé par L_00B08D10). L_00B08FB0(GlueUI, i) (thiscall, ret 4) crée l'écran au besoin, l'anime
 * (0x00B322A0(1, 7)), le rend visible (0x00B80810(1)) et pose son bit ; L_00B09090 le cache. Au menu sans Battle.net,
 * seul l'écran 0 (le fond 3D) est affiché par L_00B0A370 (masque de base [GlueUI+0x1E4]) : les autres attendent les
 * événements du client Battle.net (« Tumor »), absent du DVD — c'est pourquoi rien de 2D n'est dessiné au menu.
 * Appelée à chaque tour de la pompe du jeu (PeekMessageW revenant en 0x0099A3CC), sur le fil du jeu :
 *   - journal de chaque changement du masque (20 lignes au plus) ;
 *   - SC2_GLUE_ECRAN=<i>[@<tours>] : <tours> tours (défaut 60) après que le front-end est établi (bit 0x2 de
 *     [GlueUI+0x1D8], écran 0 affiché, ni chargement ni vidéo), appelle une fois L_00B08FB0(GlueUI, i), comme le ferait
 *     le client. Sans la variable : le journal seul (lectures du jeu, aucun effet). */
void sc2_glue_pompe(CPU *c)
{
    static int lu, idx = -1, fait; static uint32_t attente = 60, tours, masque = ~0u; static unsigned nlog;
    if (!lu) {
        lu = 1; const char *e = getenv("SC2_GLUE_ECRAN");
        if (e && *e) { idx = atoi(e); const char *at = strchr(e, '@'); if (at) attente = (uint32_t)strtoul(at + 1, NULL, 10); }
        if (idx > 5) idx = -1;
    }
    uint32_t g = rd32(GVA(0x019322A0u));
    if (!g) return;
    uint32_t m = rd32(g + 0x1DC), etat = rd32(g + 0x1D8);
    uint32_t fr = sc2_frames_presentees ? sc2_frames_presentees() : 0xFFFFFFFFu;
    if (m != masque) {
        if (nlog++ < 20) sc2_log("[GLUE] écrans affichés 0x%02X (était 0x%02X) : %s%s%s%s%s%s; GlueUI 0x%08X, état 0x%X, Present n°%u", m,
                                 masque == ~0u ? 0 : masque, m & 1 ? "Background " : "", m & 2 ? "Single " : "", m & 4 ? "Replay " : "",
                                 m & 8 ? "Credits " : "", m & 16 ? "Loading " : "", m & 32 ? "Movie " : "", g, etat, fr);
        masque = m;
    }
    /* L2 (24/10) : « Quit Campaign » (menu F10) mène au menu principal natif, inerte sans le client Battle.net (seul le fond
     * 3D, écran 0, s'affiche). En mode lanceur (SC2_RUN_SANS_BNET=1), ce fond affiché APRÈS un premier chargement (bit
     * Loading vu), front-end établi, ni chargement ni vidéo, termine le jeu proprement : sc2_stop(0), et Android revient au
     * lanceur (écran titre). SC2_MENU_PRINCIPAL=garder : le menu reste affiché (comportement d'avant). */
    static int menu_lu, menu_quitter, deja_charge;
    if (!menu_lu) {
        menu_lu = 1;
        const char *b = getenv("SC2_RUN_SANS_BNET"), *mp = getenv("SC2_MENU_PRINCIPAL");
        menu_quitter = b && *b == '1' && !(mp && !strcmp(mp, "garder"));
    }
    if (m & 0x10) deja_charge = 1;
    if (menu_quitter && deja_charge && (etat & 2) && (m & 1) && !(m & 0x30))
        sc2_stop(0, "[GLUE] menu principal atteint (Quit Campaign) en mode lanceur : fin du jeu, retour au lanceur "
                    "(SC2_MENU_PRINCIPAL=garder pour rester au menu) — Present n°%u", fr);
    if (idx < 0 || fait) return;
    if (!(etat & 2) || !(m & 1) || (m & 0x30)) { tours = 0; return; }
    if (++tours < attente) return;
    fait = 1;
    uint32_t avant = m, arg = (uint32_t)idx, ecx = c->ecx;
    sc2_log("[GLUE] SC2_GLUE_ECRAN=%d : L_00B08FB0(GlueUI 0x%08X, %d) depuis la pompe, %u tours après le front-end, Present n°%u",
            idx, g, idx, attente, fr);
    c->ecx = g;
    sc2_call_guest(c, 0x00B08FB0u, 1, &arg, 0);
    c->ecx = ecx;
    sc2_log("[GLUE] écrans affichés 0x%02X → 0x%02X ; cadre de l'écran %d : 0x%08X", avant, rd32(g + 0x1DC), idx, rd32(g + 0x1F0 + 4u * arg));
}
/* V1.1 (22/10) : masque des écrans affichés [GlueUI+0x1DC] (0 sans GlueUI), lu à la demande sur le fil du jeu — QUAND:PRET,
 * QUAND:CHARGEMENT et QUAND:GLUE du banc d'entrées (sc2_entrees_banc.c, référence faible). */
uint32_t sc2_glue_masque(void)
{
    uint32_t g = rd32(GVA(0x019322A0u));
    return g ? rd32(g + 0x1DC) : 0;
}

/* Passe 9y — erreur fatale du JEU : sc2_init_ErreurFatale 0x009A8630 (fastcall, ecx = code, edx = message ANSI) affiche
 * « A fatal error occurred: » puis finit en TerminateProcess(GetCurrentProcess(), 1) par 0x0099BC90. Avant, le journal ne
 * montrait que ce TerminateProcess (appelant 0x0099BC9E) : la ligne [FATAL-JEU] dit QUOI (code + message).
 *
 * Cas du menu sans « -run » (contrat 9y, émulateur M4 17/10, reproduit au banc avec une install sans Campaigns/) :
 *   sc2_init_LauncherCheckEtJeu → sc2_init_LancerRunDocument 0x00806D80 : option « run » vide → 0x008070DD → 0x00B0A560,
 *   qui ouvre le jeu d'archives du FRONT-END (entrée « Dflt » : Campaigns/Liberty.SC2Campaign) par l'ouvreur de mod
 *   0x009902D0 (appel en 0x00B0A664) ; archive requise absente → 0x009909A6 : ErreurFatale(0x20010002, « Campaigns/
 *   Liberty.SC2Campaign »). Ce n'est PAS un handshake du launcher : c'est l'install (le lanceur Android permet « Menu
 *   principal » avec le minimum Versions + Mods + Support, sans la campagne de 4,2 Go).
 * Réponse (même philosophie que le faux succès CreateProcessW) : sur CE seul chemin — code 0x20010002, retour 0x009909AB,
 * ouvreur appelé depuis le front-end (retour 0x00B0A669 dans son cadre ebp) — l'erreur est ignorée : l'ouvreur continue
 * sans la campagne et le menu vit (banc : même scène planète + anneaux qu'avec l'install complète, 2 500 frames).
 * Les missions (« -run ») gardent l'arrêt : une dépendance de carte manquante y reste fatale.
 * SC2_ARCHIVE_MANQUANTE_TOLEREE=0 : comportement d'origine (arrêt). */
SC2_FROID static int erreur_fatale_jeu(CPU *c)
{
    char msg[300]; unsigned k = 0;
    if (c->edx >= 0x10000u) for (; k + 1 < sizeof msg && rd8(c->edx + k); k++) msg[k] = (char)rd8(c->edx + k);
    msg[k] = 0;
    uint32_t ret = rd32(c->esp), fn = sc2_lifted_precedente(ret);
    int ouvreur = ret == 0x009909ABu, front_end = ouvreur && rd32(c->ebp + 4) == 0x00B0A669u;
    sc2_log("[FATAL-JEU] le jeu signale une erreur fatale : code 0x%08X, message « %s », depuis 0x%08X (L_%08X+0x%X)%s",
            c->ecx, msg, ret, fn, ret - fn,
            front_end ? " = archive REQUISE absente du front-end (menu, 0x00B0A560 ← sc2_init_LancerRunDocument)"
            : ouvreur ? " = ouvreur d'archives de mod 0x009902D0 : archive REQUISE absente ou illisible" : "");
    const char *e = getenv("SC2_ARCHIVE_MANQUANTE_TOLEREE");
    if (c->ecx == 0x20010002u && front_end && !(e && *e == '0')) {
        sc2_log("[FATAL-JEU] erreur ignorée (passe 9y) : le menu continue sans « %s » — install incomplète, la campagne "
                "restera inaccessible ; SC2_ARCHIVE_MANQUANTE_TOLEREE=0 pour l'arrêt d'origine", msg);
        c->esp += 4;                                      /* ret de la fastcall : aucun argument sur la pile */
        return 1;
    }
    return 0;
}

static const char *nom_jalon(uint32_t va)
{
    for (unsigned i = 0; i < sizeof g_jalons / sizeof g_jalons[0]; i++)
        if (g_jalons[i].va == va) return g_jalons[i].nom;
    return "";
}

/* ========================================================================== */
/* Dispatch                                                                   */
/* ========================================================================== */

/* Passe 7v — SC2_MANQUE_CONTINUE=1 (diagnostic, cartographie) : un MANQUE n'arrête plus le run. Il est journalisé
 * ([MANQUE-CONTINUE] à la 1re occurrence de l'adresse puis aux occurrences 2^k : adresse, n°, frame, adresse de
 * retour, appelant, this) puis l'appel est simulé : eax = edx = 0, adresse de retour dépilée (convention cdecl ; les
 * arguments d'un callee-pop « ret N » restent sur la pile). Le bilan ([BILAN] [MANQUE] : 1re frame, occurrences) est
 * écrit par sc2_stop. Sans la variable : arrêt inchangé. Essai : SC2_MANQUE_SIMULER (dispatch_un). */
uint32_t sc2_frames_presentees(void) __attribute__((weak));
#define MQ_MAX 4096
static struct { uint32_t va, ret, appelant, frame1; uint64_t n, disp1; } g_mq[MQ_MAX];
static int g_mq_n, g_mq_plein;
static pthread_mutex_t g_mq_mu = PTHREAD_MUTEX_INITIALIZER;

static int manque_continuer(CPU *c, uint32_t va, const char *quoi)
{
    static int actif = -1;
    if (actif < 0) { const char *e = getenv("SC2_MANQUE_CONTINUE"); actif = e && *e && *e != '0'; }
    if (!actif) return 0;
    uint32_t ret = rd32(c->esp), fr = sc2_frames_presentees ? sc2_frames_presentees() : 0xFFFFFFFFu;
    uint32_t appelant = g_sc2_ss->n > 0 && g_sc2_ss->n <= (int)(sizeof g_sc2_ss->va / sizeof g_sc2_ss->va[0])
                        ? g_sc2_ss->va[g_sc2_ss->n - 1] : 0;
    uint64_t n = 0; int k;
    pthread_mutex_lock(&g_mq_mu);
    for (k = 0; k < g_mq_n && g_mq[k].va != va; k++) ;
    if (k == g_mq_n && g_mq_n < MQ_MAX) {
        g_mq[k].va = va; g_mq[k].ret = ret; g_mq[k].appelant = appelant; g_mq[k].frame1 = fr;
        g_mq[k].disp1 = g_sc2_dispatches; g_mq_n++;
    }
    if (k < g_mq_n) n = ++g_mq[k].n; else g_mq_plein = 1;
    pthread_mutex_unlock(&g_mq_mu);
    if (n && (n & (n - 1)) == 0)
        sc2_log("[MANQUE-CONTINUE] %s 0x%08X n=%llu frame %d ret 0x%08X appelant 0x%08X ecx %08X edx %08X [ecx] %08X",
                quoi, va, (unsigned long long)n, (int)fr, ret, appelant, c->ecx, c->edx,
                c->ecx >= 0x00800000u && c->ecx < 0xF0000000u ? rd32(c->ecx) : 0u);
    c->eax = 0; c->edx = 0; c->esp += 4;
    return 1;
}

static void manque_bilan(void)
{
    if (!g_mq_n) return;
    printf("[BILAN] [MANQUE] %d adresse(s) manquante(s) contournée(s) (SC2_MANQUE_CONTINUE)%s ; ordre d'apparition :\n",
           g_mq_n, g_mq_plein ? " — table pleine, liste tronquée" : "");
    for (int k = 0; k < g_mq_n; k++)
        printf("[BILAN] [MANQUE] 0x%08X  1re frame %d  occurrences %llu  1er dispatch %llu  ret 0x%08X  appelant 0x%08X\n",
               g_mq[k].va, (int)g_mq[k].frame1, (unsigned long long)g_mq[k].n, (unsigned long long)g_mq[k].disp1,
               g_mq[k].ret, g_mq[k].appelant);
}

SC2_FROID static void manque(CPU *c, uint32_t va, const char *quoi)
{
    if (manque_continuer(c, va, quoi)) return;
    if (g_sc2_recover) {
        sc2_log("[TOLÉRANT] %s 0x%08X dans l'initialiseur 0x%08X : initialiseur abandonné",
                quoi, va, g_sc2_recover_va);
        siglongjmp(*g_sc2_recover, 1);
    }
    sc2_stop(3, "%s 0x%08X (adresse de retour invitée [esp] = 0x%08X)", quoi, va, rd32(c->esp));
}

/* Passe 7w8 : santé de la PILE HÔTE. Bornes de la pile du fil (sc2_thread_fault_stack, au départ de chaque fil),
 * profondeur maximale atteinte par sc2_dispatch, sauts SC2_SAUT pris. Un code lifté sans trampoline (lift périmé :
 * sauts entre morceaux = `dispatch(c, X); return;`, non éliminés à -O0) fait grossir la pile à chaque tour de boucle
 * — crash Android 7w8 (boucle 0x00F6F670 ↔ 0x00F6F87F, pile de 16 Mo du fil de jeu). */
static __thread uintptr_t t_pile_lo, t_pile_hi;      /* pile native du FIL */
void *GetCurrentFiber(void);                         /* win32_compat.c (LPVOID, BOOL = int, SIZE_T = size_t) */
int w32_fiber_stack(void *fiber, void **base, size_t *size);
static __thread void *t_pile_fibre;                  /* fibre hôte (win32_compat) dont les bornes sont en cache */
static __thread uintptr_t t_fib_lo, t_fib_hi;
static uintptr_t g_pile_max, g_pile_taille_max;
static uint64_t g_n_sauts;
/* Le jeu tourne sur des FIBRES hôtes (pile mmap de 64 Mo + page de garde, win32_compat) : la pile courante est celle
 * de la fibre courante si elle en a une, sinon celle du fil. */
static void pile_courante(uintptr_t *lo, uintptr_t *hi)
{
    void *f = GetCurrentFiber();
    if (f && f != t_pile_fibre) {
        void *b = NULL; size_t n = 0;
        t_pile_fibre = f;
        if (w32_fiber_stack(f, &b, &n) && b) { t_fib_lo = (uintptr_t)b; t_fib_hi = (uintptr_t)b + n; }
        else t_fib_lo = t_fib_hi = 0;                /* fibre issue du fil converti : pile du fil */
    }
    if (f && t_fib_hi) { *lo = t_fib_lo; *hi = t_fib_hi; }
    else { *lo = t_pile_lo; *hi = t_pile_hi; }
}
static void dispatch_un(void *vc, uint32_t va);
void sc2_dispatch(void *vc, uint32_t va)
{
    CPU *c = (CPU *)vc;                              /* passe 10r : état porté par le CPU, aucun accès __thread ici */
    if (!c->fantome) c->fantome = g_sc2_ss;                    /* 1er appel sur ce CPU : sa pile fantôme */
    uint32_t *ancien = c->saut;
    if ((c->n_disp++ & 63) == 0) {                 /* passe 10r : profondeur max échantillonnée (1 appel sur 64) */
        uintptr_t ici = (uintptr_t)&ancien, lo, hi;
        pile_courante(&lo, &hi);
        if (hi && ici >= lo && ici < hi && hi - ici > g_pile_max) {
            g_pile_max = hi - ici; g_pile_taille_max = hi - lo;              /* course bénigne : diagnostic */
        }
    }
    for (;;) {
        uint32_t saut = 0;
        c->saut = &saut;
        dispatch_un(vc, va);
        c->saut = ancien;
        if (!saut) return;
        __atomic_add_fetch(&g_n_sauts, 1, __ATOMIC_RELAXED);
        va = saut;                                      /* SC2_SAUT : même niveau de pile */
    }
}
/* Passe 7w8 : santé du code lifté (racines des passes 7j/7q, absentes d'un lift périmé) et de la pile hôte. */
static int g_lift_perime = -1;
static void lift_sante(void)
{
    static const uint32_t racines[] = { 0x010CAB80u /* 7j */, 0x0129DC30u /* 7q */ };
    int manque = 0;
    for (unsigned k = 0; k < sizeof racines / sizeof racines[0]; k++) if (lifted_find(racines[k]) < 0) manque++;
    g_lift_perime = manque > 0;
    if (manque)
        sc2_log("[LIFT] ⚠ code lifté PÉRIMÉ : %d racine(s) des passes 7j/7q absente(s) (0x010CAB80, 0x0129DC30) — lift "
                "antérieur au trampoline SC2_SAUT probable : débordement de PILE HÔTE attendu dans les longues boucles "
                "(chargement de mission). Relifter : scripts/lift-boot.py + lift-roots-runs.txt", manque);
    else
        sc2_log("[LIFT] racines 7j/7q présentes (0x010CAB80, 0x0129DC30) : lift à jour");
    sc2_log("[TRACES] SC2_TRACE=%d : %s", sc2_trace_actif(), sc2_trace_actif() ? "traces par appel et par frame actives"
            : "runtime silencieux ([FS] [IAT] [MPQ] [D3D9] [TEX]… coupés ; bilan, arrêts et erreurs gardés)");
}
static void pile_bilan(void)
{
    printf("[BILAN] [PILE] pile hôte : profondeur max %lu Kio sur %lu Kio (%.1f %%) ; sauts SC2_SAUT %llu ; lift %s\n",
           (unsigned long)(g_pile_max >> 10), (unsigned long)(g_pile_taille_max >> 10),
           g_pile_taille_max ? 100.0 * (double)g_pile_max / (double)g_pile_taille_max : 0.0,
           (unsigned long long)g_n_sauts, g_lift_perime < 0 ? "non vérifié" : g_lift_perime ? "PÉRIMÉ" : "à jour");
}

static void dispatch_un(void *vc, uint32_t va)
{
    CPU *c = (CPU *)vc;   /* passe 10s : pile fantôme lue par c->fantome (posé par sc2_dispatch), jamais par g_sc2_ss :
                           * intégrée dans sc2_dispatch, une lecture __thread même sur un chemin froid faisait remonter
                           * l'appel au résolveur TLS en tête de fonction, à CHAQUE dispatch (2,9 % du fil du jeu) */
    g_sc2_dispatches++;
    if (va >= SC2_IAT_STUB_BASE && va < SC2_IAT_STUB_BASE + 0x10000u) {
        if (sc2_iat_call(c, va)) return;
    }
    if (va >= SC2_COM_BASE && va < SC2_COM_END && sc2_com_call(c, va)) return;   /* D3D9 factice */
    int i = lifted_find(va);
    /* Passe 7v — SC2_MANQUE_SIMULER=<va>,<va>,… (hex, 16 au plus) : ces adresses, bien que liftées, sont traitées
     * comme manquantes (essai de SC2_MANQUE_CONTINUE, ou reproduction d'un build antérieur à leur lift). */
    static int ms_n = -1; static uint32_t ms_va[16];
    if (ms_n < 0) {
        const char *e = getenv("SC2_MANQUE_SIMULER"); int n = 0;
        while (e && *e && n < 16) { char *f; ms_va[n] = (uint32_t)strtoul(e, &f, 16); if (f == e) break; n++; e = *f ? f + 1 : f; }
        ms_n = n;
    }
    for (int k = 0; k < ms_n; k++) if (ms_va[k] == va) i = -1;
    if (i < 0) {
        manque(c, va, va >= SC2_IMAGE_BASE && va < SC2_IMAGE_BASE + SC2_SIZE_OF_IMAGE
                      ? "MANQUE : aucune fonction liftée à" : "MANQUE : saut hors image vers");
        return;
    }
    if (!g_executed[i]) { g_executed[i] = 1; g_n_executed++; jalon(va); nouvelle_fn(c, va); }
    if (va == 0x00A2BD70u || va == 0x00B7DC50u || va == 0x00B80810u) menu_jeu(c, va);   /* passe 9j */
    if (va == 0x009A8630u && erreur_fatale_jeu(c)) return;                                /* passe 9y */
    /* Passe 10r : chemin rapide quand aucun outil de diagnostic n'est demandé (compteur, vidage, traceur, espion,
     * registres sauvés) — décidé une fois : leurs tests et lectures d'état passaient à chaque appel indirect. */
    static int rapide = -1;
    if (rapide < 0)
        rapide = !getenv("SC2_COMPTE_VA") && !getenv("SC2_VIDAGE") && !getenv("SC2_TRACE_VA") && !getenv("SC2_ESPION")
                 && !getenv("SC2_SAUVES_VA");
    if (rapide) {
        sc2_shadow *ss = (sc2_shadow *)c->fantome;
        if (ss->n < (int)(sizeof ss->va / sizeof ss->va[0])) { ss->va[ss->n] = va; ss->esp[ss->n] = c->esp; }
        ss->n++;
        g_lifted[i].fn(c);
        ss->n--;
        return;
    }
    /* Compteur (passe 7j) : SC2_COMPTE_VA=<va>,<va>,… (hex, 16 au plus) compte les entrées de chaque va et journalise
     * la n-ième entrée pour n puissance de 2 (registres, 4 premiers arguments, profondeur fantôme) : la forme d'une
     * croissance sans fin. Sans la variable : un test par dispatch. */
    /* SC2_COMPTE_RET=<adresse de retour> : ne compte que les appels revenant là ; SC2_COMPTE_PAS=<k> : journalise
     * aussi toutes les k entrées. */
    static int cp_n = -1, cp_ecx, cp_dump; static uint32_t cp_va[16], cp_ret, cp_doff, cp_dpas; static uint64_t cp_cnt[16], cp_pas;
    if (cp_n < 0) {
        const char *e = getenv("SC2_COMPTE_VA"), *r = getenv("SC2_COMPTE_RET"), *p = getenv("SC2_COMPTE_PAS"); int n = 0;
        while (e && *e && n < 16) { char *f; cp_va[n] = (uint32_t)strtoul(e, &f, 16); if (f == e) break; n++; e = *f ? f + 1 : f; }
        cp_ret = r ? (uint32_t)strtoul(r, NULL, 16) : 0; cp_pas = p ? strtoull(p, NULL, 10) : 0;
        cp_ecx = getenv("SC2_COMPTE_ECX") != NULL;
        const char *dd = getenv("SC2_COMPTE_DUMP"); if (dd) cp_dump = sscanf(dd, "%x,%u", &cp_doff, &cp_dpas) >= 1;
        cp_n = n;
    }
    for (int k = 0; k < cp_n; k++)
        if (cp_va[k] == va && (!cp_ret || rd32(c->esp) == cp_ret)) {
            uint64_t n = __atomic_add_fetch(&cp_cnt[k], 1, __ATOMIC_RELAXED);
            if ((n & (n - 1)) == 0 || (cp_pas && n % cp_pas == 0)) {
                sc2_log("[COMPTE] 0x%08X n=%llu  ecx %08X edx %08X eax %08X ebx %08X esi %08X edi %08X  args %08X %08X %08X %08X  prof %d ret %08X",
                        va, (unsigned long long)n, c->ecx, c->edx, c->eax, c->ebx, c->esi, c->edi, rd32(c->esp + 4), rd32(c->esp + 8),
                        rd32(c->esp + 12), rd32(c->esp + 16), ((sc2_shadow *)c->fantome)->n, rd32(c->esp));
                if (cp_ecx && c->ecx >= 0x00800000u && c->ecx < 0xF0000000u)   /* SC2_COMPTE_ECX=1 : objet this */
                    sc2_log("[COMPTE]   [ecx] %08X %08X %08X %08X %08X %08X %08X %08X", rd32(c->ecx), rd32(c->ecx + 4),
                        rd32(c->ecx + 8), rd32(c->ecx + 12), rd32(c->ecx + 16), rd32(c->ecx + 20), rd32(c->ecx + 24),
                        rd32(c->ecx + 28));
                if (cp_dump && c->ecx >= 0x00800000u && c->ecx < 0xF0000000u) {   /* SC2_COMPTE_DUMP=off,pas */
                    uint32_t b = cp_dpas ? rd32(c->ecx + cp_doff) + rd32(c->esp + 4) * cp_dpas   /* [ecx+off][arg0] */
                                         : c->ecx + cp_doff; float f[9];                         /* pas 0 : ecx+off */
                    if (b >= 0x00800000u && b < 0xF0000000u) {
                        for (int q = 0; q < 9; q++) { uint32_t w = rd32(b + 4u * q); memcpy(&f[q], &w, 4); }
                        sc2_log("[COMPTE]   @%08X  %g %g %g | %g %g %g | %g %g %g", b, f[0], f[1], f[2], f[3], f[4],
                                f[5], f[6], f[7], f[8]);
                    }
                }
            }
            break;
        }
    /* Vidage (passe 7j) : SC2_VIDAGE=<va>,<off ptr>,<off n>,<taille élément>,<fichier> — à la 1re entrée de va, écrit
     * n = [ecx+off n] éléments depuis [ecx+off ptr] (off ptr = « - » : l'objet ecx lui-même, n = taille en octets)
     * dans le fichier. Diagnostic hors ligne (tableaux du jeu). */
    static int vd_etat = -1; static uint32_t vd_va, vd_op, vd_on, vd_t; static char vd_f[256]; static int vd_obj;
    if (vd_etat < 0) {
        const char *e = getenv("SC2_VIDAGE"); char op[32];
        vd_etat = e && sscanf(e, "%x,%31[^,],%x,%x,%255s", &vd_va, op, &vd_on, &vd_t, vd_f) == 5;
        if (vd_etat) { vd_obj = op[0] == '-'; vd_op = vd_obj ? 0 : (uint32_t)strtoul(op, NULL, 16); }
    }
    if (vd_etat == 1 && va == vd_va) {
        vd_etat = 2;
        uint32_t base = vd_obj ? c->ecx : rd32(c->ecx + vd_op), n = vd_obj ? vd_on : rd32(c->ecx + vd_on);
        uint64_t oct = (uint64_t)n * (vd_obj ? 1 : vd_t);
        FILE *vf = fopen(vd_f, "wb");
        if (vf && oct < 0x40000000u) { fwrite(GPTR(base), 1, (size_t)oct, vf); }
        if (vf) fclose(vf);
        sc2_log("[VIDAGE] 0x%08X : %llu octets depuis 0x%08X (n=%u) → %s", va, (unsigned long long)oct, base, n, vd_f);
    }
    /* Traceur (passe 4) : SC2_TRACE_VA=<va hex> → à l'entrée de va, les SC2_TRACE_N
     * dispatchs suivants (défaut 400) sont journalisés avec la profondeur fantôme. */
    static uint32_t tr_va = 1; static int tr_n = -1;
    if (tr_va == 1) { const char *e = getenv("SC2_TRACE_VA"); tr_va = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
    if (tr_va && va == tr_va && tr_n < 0) { const char *e = getenv("SC2_TRACE_N"); tr_n = e ? atoi(e) : 400; }
    if (tr_n > 0) {
        tr_n--;
        sc2_log("[TRACE] %*s0x%08X  (eax %08X ecx %08X edx %08X, [esp+4] %08X %08X)", (((sc2_shadow *)c->fantome)->n % 40) * 2, "",
                va, c->eax, c->ecx, c->edx, rd32(c->esp + 4), rd32(c->esp + 8));
    }
    /* Espion (passe 4) : SC2_ESPION=<adresse de retour hex> journalise la cible
     * des appels revenant à cette adresse, leurs 4 premiers arguments et eax rendu. */
    static uint32_t espion = 1;
    if (espion == 1) { const char *e = getenv("SC2_ESPION"); espion = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
    if (espion && rd32(c->esp) == espion) {
        uint32_t ecx = c->ecx, a0 = rd32(c->esp + 4), a1 = rd32(c->esp + 8);
        sc2_shadow *s2 = ((sc2_shadow *)c->fantome);
        if (s2->n < (int)(sizeof s2->va / sizeof s2->va[0])) { s2->va[s2->n] = va; s2->esp[s2->n] = c->esp; }
        s2->n++; g_lifted[i].fn(c); s2->n--;
        sc2_log("[ESPION] appel 0x%08X (ecx 0x%08X, args 0x%X 0x%X) → eax 0x%08X [eax+4]=0x%08X",
                va, ecx, a0, a1, c->eax, (c->eax >= 0x00800000u && c->eax < 0x38000000u) ? rd32(c->eax + 4) : 0);
        return;
    }
    sc2_shadow *ss = ((sc2_shadow *)c->fantome);
    if (ss->n < (int)(sizeof ss->va / sizeof ss->va[0])) { ss->va[ss->n] = va; ss->esp[ss->n] = c->esp; }
    ss->n++;
    /* Vérif (passe 7j) : SC2_SAUVES_VA=<va>,… ou « * » : ebx/esi/edi/ebp (sauvés par l'appelé en cdecl/stdcall/
     * thiscall) identiques au retour ? Les 64 premiers écarts sont journalisés avec la pile fantôme courte.
     * Sans la variable : un test par dispatch. */
    static int sv_n = -1, sv_tous; static uint32_t sv_va[16]; static int sv_vus;
    if (sv_n < 0) {
        const char *e = getenv("SC2_SAUVES_VA"); int n = 0;
        if (e && *e == '*') sv_tous = 1;
        else while (e && *e && n < 16) { char *f; sv_va[n] = (uint32_t)strtoul(e, &f, 16); if (f == e) break; n++; e = *f ? f + 1 : f; }
        sv_n = n;
    }
    int sv = sv_tous;
    for (int k = 0; !sv && k < sv_n; k++) sv = sv_va[k] == va;
    if (sv) {
        uint32_t b = c->ebx, si = c->esi, di = c->edi, bp = c->ebp, ret = rd32(c->esp);
        g_lifted[i].fn(c);
        if ((c->ebx != b || c->esi != si || c->edi != di || c->ebp != bp) && __atomic_add_fetch(&sv_vus, 1, __ATOMIC_RELAXED) <= 64)
            sc2_log("[SAUVÉS] 0x%08X (retour 0x%08X) : ebx %08X→%08X esi %08X→%08X edi %08X→%08X ebp %08X→%08X  appelant 0x%08X",
                    va, ret, b, c->ebx, si, c->esi, di, c->edi, bp, c->ebp, ss->n >= 2 && ss->n - 2 < (int)(sizeof ss->va / sizeof ss->va[0]) ? ss->va[ss->n - 2] : 0);
    } else
        g_lifted[i].fn(c);
    ss->n--;
}

static uint32_t *reg_of(CPU *c, const char *r)
{
    static const char *n[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    uint32_t *v[] = { &c->eax, &c->ecx, &c->edx, &c->ebx, &c->esp, &c->ebp, &c->esi, &c->edi };
    for (int i = 0; i < 8; i++) if (!strncmp(r, n[i], 3)) return v[i];
    return NULL;
}

/* Passe 10s : build de contrôle des registres locaux (sc2_lifted.h, -DSC2_RL_CONTROLE). Un champ hors masque qui diffère
 * entre rl et *c à une frontière = écriture oubliée par le flot de données de scripts/registres-locaux.py. */
static uint64_t g_rl_ecarts;
SC2_FROID void sc2_rl_ecart(const char *fn, uint32_t ecarts, uint32_t masque)
{
    static const char *n[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "cf", "zf", "sf", "of", "pf", "af" };
    char l[96]; size_t k = 0; l[0] = 0;
    for (int i = 0; i < 14 && k < sizeof l - 4; i++)
        if (ecarts >> i & 1) k += (size_t)snprintf(l + k, sizeof l - k, " %s", n[i]);
    uint64_t m = __atomic_add_fetch(&g_rl_ecarts, 1, __ATOMIC_RELAXED);
    if (m <= 50 || (m & (m - 1)) == 0)
        sc2_log("[RL-ÉCART] n=%llu %s : hors masque 0x%04X et différents de *c :%s", (unsigned long long)m, fn, masque, l);
}

/* Passe 10t : build de contrôle de la pile x87 locale (sc2_lifted.h, -DSC2_RL_CONTROLE, --x87). Un champ x87
 * (st0..st7, top, sw) hors masque qui diffère entre x87 et *c à une frontière = écriture oubliée par le flot de
 * données x87 de scripts/registres-locaux.py. Mêmes règles de limitation que sc2_rl_ecart. */
static uint64_t g_x87_ecarts;
SC2_FROID void sc2_x87_ecart(const char *fn, uint32_t ecarts, uint32_t masque)
{
    static const char *n[] = { "st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7", "top", "sw" };
    char l[96]; size_t k = 0; l[0] = 0;
    for (int i = 0; i < 10 && k < sizeof l - 4; i++)
        if (ecarts >> i & 1) k += (size_t)snprintf(l + k, sizeof l - k, " %s", n[i]);
    uint64_t m = __atomic_add_fetch(&g_x87_ecarts, 1, __ATOMIC_RELAXED);
    if (m <= 50 || (m & (m - 1)) == 0)
        sc2_log("[X87-ÉCART] n=%llu %s : hors masque 0x%03X et différents de *c :%s", (unsigned long long)m, fn, masque, l);
}

/* Passe 10u : build de contrôle des registres SSE locaux (sc2_lifted.h, -DSC2_RL_CONTROLE, --sse). Un registre
 * xmm hors masque qui diffère entre sse et *c à une frontière = écriture oubliée par le flot de données SSE de
 * scripts/registres-locaux.py. Mêmes règles de limitation que sc2_rl_ecart. */
static uint64_t g_sse_ecarts;
SC2_FROID void sc2_sse_ecart(const char *fn, uint32_t ecarts, uint32_t masque)
{
    static const char *n[] = { "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7" };
    char l[96]; size_t k = 0; l[0] = 0;
    for (int i = 0; i < 8 && k < sizeof l - 4; i++)
        if (ecarts >> i & 1) k += (size_t)snprintf(l + k, sizeof l - k, " %s", n[i]);
    uint64_t m = __atomic_add_fetch(&g_sse_ecarts, 1, __ATOMIC_RELAXED);
    if (m <= 50 || (m & (m - 1)) == 0)
        sc2_log("[SSE-ÉCART] n=%llu %s : hors masque 0x%02X et différents de *c :%s", (unsigned long long)m, fn, masque, l);
}

void sc2_todo_c(void *vc, uint32_t va, const char *text)
{
    CPU *c = (CPU *)vc;
    if (!strcmp(text, "rdtsc")) {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        uint64_t v = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
        c->eax = (uint32_t)v; c->edx = (uint32_t)(v >> 32);
        return;
    }
    if (!strcmp(text, "cpuid")) {
        /* Passe 4 : CPU factice plausible, journalisé une fois par feuille.
         * GenuineIntel famille 6 ; SSE/SSE2/CMOV/CX8/FXSR/TSC, PAS de MMX ni de
         * SSE3+ : lift32_cpu ne traduit pas les registres mm* (1 fonction non
         * liftable sur 85 351), on détourne le jeu de ces chemins. */
        static const char marque[48] = "SC2 Partout CPU factice (passe 4)";
        uint32_t f = c->eax, r4[4] = { 0, 0, 0, 0 };
        if (f == 0) { r4[0] = 0xD; r4[1] = 0x756E6547; r4[3] = 0x49656E69; r4[2] = 0x6C65746E; }  /* GenuineIntel */
        else if (f == 1) { r4[0] = 0x000206A7; r4[1] = 0x00010800; r4[2] = 0;
                           r4[3] = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 24) | (1u << 25) | (1u << 26); }
        else if (f == 0x80000000u) r4[0] = 0x80000004u;
        else if (f >= 0x80000002u && f <= 0x80000004u) memcpy(r4, marque + 16 * (f - 0x80000002u), 16);
        static uint32_t vus[16]; static int nvus;
        int deja = 0;
        for (int i = 0; i < nvus; i++) deja |= vus[i] == f;
        if (!deja && nvus < 16) { vus[nvus++] = f;
            sc2_log("[CPUID] feuille 0x%X à 0x%08X → %08X %08X %08X %08X", f, va, r4[0], r4[1], r4[2], r4[3]); }
        c->eax = r4[0]; c->ebx = r4[1]; c->ecx = r4[2]; c->edx = r4[3];
        return;
    }
    /* Passe 7h : SSE non traduit par le lifter, atteint au chargement d'une carte (0x009C16B7 rsqrtps…).
     * rsqrtps / rcpps / movhlps xmmA, xmmB ; rsqrtss / rcpss xmmA, xmmB | dword ptr [reg ± 0xdisp].
     * Valeurs EXACTES (1/sqrt, 1/x) là où le matériel donne une approximation à 12 bits (écart ≤ 1,5·2^-12). */
    {
        char op[16], src[40]; int da = -1, db = -1;
        if (sscanf(text, "%15s xmm%d, %39[^\n]", op, &da, src) == 3 && da >= 0 && da < 8) {
            float v = 0.f; int scal = !strcmp(op, "rsqrtss") || !strcmp(op, "rcpss"), lu = 0;
            if (sscanf(src, "xmm%d", &db) == 1 && db >= 0 && db < 8) lu = 1;
            else if (scal && !strncmp(src, "dword ptr [", 11)) {
                uint32_t *r = reg_of(c, src + 11); char sg = 0; unsigned d = 0;
                if (r && (src[14] == ']' || sscanf(src + 14, " %c 0x%x]", &sg, &d) == 2)) {
                    uint32_t adr = *r + (sg == '-' ? (uint32_t)-d : d);
                    uint32_t bits = rd32(adr); memcpy(&v, &bits, 4); lu = 2;
                }
            }
            if (lu) {
                XMM *A = &c->xmm[da], *B = lu == 1 ? &c->xmm[db] : NULL;
                if (!strcmp(op, "movhlps") && B) { A->u64[0] = B->u64[1]; return; }
                if (!strcmp(op, "rsqrtps") && B) { for (int i = 0; i < 4; i++) A->f32[i] = 1.f / sqrtf(B->f32[i]); return; }
                if (!strcmp(op, "rcpps") && B)   { for (int i = 0; i < 4; i++) A->f32[i] = 1.f / B->f32[i]; return; }
                if (scal) {
                    float x = B ? B->f32[0] : v;
                    A->f32[0] = !strcmp(op, "rsqrtss") ? 1.f / sqrtf(x) : 1.f / x;
                    return;
                }
            }
        }
    }
    const char *b = strstr(text, "cmpxchg8b qword ptr [");
    uint32_t *r = b ? reg_of(c, b + 21) : NULL;
    if (r && b[24] == ']') {                     /* cmpxchg8b qword ptr [reg] */
        uint64_t exp = ((uint64_t)c->edx << 32) | c->eax, nv = ((uint64_t)c->ecx << 32) | c->ebx;
        uint64_t old = __sync_val_compare_and_swap((uint64_t *)GPTR(*r), exp, nv);
        c->zf = old == exp;
        if (!c->zf) { c->eax = (uint32_t)old; c->edx = (uint32_t)(old >> 32); }
        return;
    }
    if (g_sc2_recover) {
        sc2_log("[TOLÉRANT] TODO lifter à 0x%08X (%s) dans l'initialiseur 0x%08X : abandonné",
                va, text, g_sc2_recover_va);
        siglongjmp(*g_sc2_recover, 1);
    }
    /* passe 10p : un talon de FONCTION ENTIÈRE dans un binaire de jeu = clôture liftée périmée (APK v7 : 0x00C72900,
     * liftée depuis la 9n, talon resté dans la copie du M4) — pas un trou du lifter à combler */
    if (!strncmp(text, "fonction non liftable", 21))
        sc2_stop(4, "TODO du lifter à 0x%08X : %s — talon de fonction entière : la clôture src/lifted compilée est "
                 "PÉRIMÉE (scripts/garde-lift.sh l'aurait refusée) ; resynchroniser depuis le canon et reconstruire", va, text);
    sc2_stop(4, "TODO du lifter à 0x%08X : %s", va, text);
}

uint32_t sc2_call_guest(CPU *c, uint32_t va, int nargs, const uint32_t *args, int cdecl)
{
    for (int i = nargs - 1; i >= 0; i--) push32(c, args[i]);
    push32(c, SC2_RET_SENTINEL);
    sc2_dispatch(c, va);
    if (cdecl) c->esp += 4u * (uint32_t)nargs;
    return c->eax;
}

/* Arguments d'un cadre fantôme : 4 mots à [esp+4] ; une chaîne invitée (ASCII ou
 * UTF-16) pointée par un argument est décodée — c'est ce qui nomme un fichier. */
static int lisible(uint32_t v) { return v >= 0x00800000u && v < 0x38000000u; }
static void args_cadre(sc2_shadow *ss, int k, char *o, size_t cap)
{
    size_t n = 0; o[0] = 0;
    uint32_t e = ss->esp[k];
    if (!lisible(e)) return;
    for (int i = 1; i <= 4 && n < cap; i++) {
        uint32_t v = rd32(e + 4u * (uint32_t)i);
        n += (size_t)snprintf(o + n, cap - n, "%s%08X", i > 1 ? " " : "args ", v);
        if (lisible(v) && n < cap) {
            char t[72]; int m = 0, large = rd8(v) && !rd8(v + 1) && rd8(v + 2) && !rd8(v + 3);
            for (uint32_t j = 0; m < 64; j++) {
                uint32_t ch = large ? rd16(v + 2 * j) : rd8(v + j);
                if (!ch) break;
                if (ch < 0x20 || ch > 0x7E) { m = -1; break; }
                t[m++] = (char)ch;
            }
            if (m >= 3) { t[m] = 0; n += (size_t)snprintf(o + n, cap - n, "=\"%s\"", t); }
        }
    }
}

void sc2_dump_ss(const char *pourquoi)
{
    sc2_shadow *ss = g_sc2_ss;
    int n = ss->n < (int)(sizeof ss->va / sizeof ss->va[0]) ? ss->n : (int)(sizeof ss->va / sizeof ss->va[0]);
    printf("[PILE] %s — pile d'appels invitée (fantôme, avec arguments) :\n", pourquoi);
    for (int k = n - 1, m = 0; k >= 0 && m < 40; k--, m++) {
        char ar[400]; args_cadre(ss, k, ar, sizeof ar);
        printf("    #%-3d 0x%08X %-40s %s\n", k, ss->va[k], nom_jalon(ss->va[k]), ar);
    }
    fflush(stdout);
}

/* ---- setjmp / longjmp invités (passe 4) --------------------------------------------- */
typedef struct { uint32_t jb, ebx, esi, edi, ebp, esp; int ss_n; uint32_t *saut; sigjmp_buf h; } sj_ent;
static __thread sj_ent g_sj[64];
static __thread int g_sj_n;
static __thread sj_ent *g_sj_cible;
sigjmp_buf *sc2_setjmp_prepare(void *vc)
{
    CPU *c = (CPU *)vc;
    uint32_t jb = rd32(c->esp + 4);                  /* [esp] = retour, [esp+4] = jmp_buf */
    sj_ent *e = NULL;
    for (int i = 0; i < g_sj_n; i++) if (g_sj[i].jb == jb) e = &g_sj[i];
    if (!e) e = &g_sj[g_sj_n < 64 ? g_sj_n++ : 63];
    e->jb = jb; e->ebx = c->ebx; e->esi = c->esi; e->edi = c->edi; e->ebp = c->ebp; e->esp = c->esp;
    e->ss_n = g_sc2_ss->n; e->saut = c->saut;    /* 7j : le siglongjmp doit retrouver le saut en attente */
    /* jmp_buf MSVC : Ebp, Ebx, Edi, Esi, Esp, Eip — rempli pour la forme */
    wr32(jb, c->ebp); wr32(jb + 4, c->ebx); wr32(jb + 8, c->edi); wr32(jb + 12, c->esi);
    wr32(jb + 16, c->esp + 4); wr32(jb + 20, rd32(c->esp));
    g_sj_cible = NULL;
    return &e->h;
}
void sc2_setjmp_done(void *vc, int v)
{
    CPU *c = (CPU *)vc;
    if (v && g_sj_cible) {
        sj_ent *e = g_sj_cible; g_sj_cible = NULL;
        c->ebx = e->ebx; c->esi = e->esi; c->edi = e->edi; c->ebp = e->ebp; c->esp = e->esp;
        g_sc2_ss->n = e->ss_n; c->saut = e->saut;
    }
    c->eax = (uint32_t)v;
    c->esp += 4;                                      /* ret de _setjmp3 (cdecl : l'appelant dépile) */
}
void sc2_longjmp(void *vc, uint32_t jb, uint32_t val)
{
    (void)vc;
    for (int i = 0; i < g_sj_n; i++)
        if (g_sj[i].jb == jb) {
            static int n_log;
            if (n_log++ < 8) sc2_log("[SETJMP] longjmp(0x%08X, %u) → retour au setjmp (pile fantôme %d → %d)",
                                     jb, val, g_sc2_ss->n, g_sj[i].ss_n);
            g_sj_cible = &g_sj[i];
            siglongjmp(g_sj[i].h, val ? (int)val : 1);
        }
    sc2_stop(14, "longjmp(0x%08X, %u) vers un jmp_buf inconnu de ce thread", jb, val);
}

/* ---- registre des piles fantômes + chien de garde (passe 4) ---------------- */
static struct { sc2_shadow *ss; char nom[96]; } g_ss_reg[128];
static int g_ss_nreg;
static pthread_mutex_t g_ss_mu = PTHREAD_MUTEX_INITIALIZER;
void sc2_ss_register(sc2_shadow *ss, const char *fmt, ...)
{
    pthread_mutex_lock(&g_ss_mu);
    if (g_ss_nreg < 128) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(g_ss_reg[g_ss_nreg].nom, sizeof g_ss_reg[0].nom, fmt, ap);
        va_end(ap);
        g_ss_reg[g_ss_nreg++].ss = ss;
    }
    pthread_mutex_unlock(&g_ss_mu);
}
static void *watchdog(void *p)
{
    unsigned s = (unsigned)(uintptr_t)p;
    sleep(s);
    uint64_t d0 = g_sc2_dispatches;
    sleep(1);
    uint64_t d1 = g_sc2_dispatches;
    printf("\n[GARDE] %u s écoulées : %llu dispatchs, %llu dans la dernière seconde (%s)\n", s,
           (unsigned long long)d1, (unsigned long long)(d1 - d0), d1 - d0 > 1000 ? "BOUCLE active" : "ATTENTE / blocage");
    for (int i = 0; i < g_ss_nreg; i++) {
        sc2_shadow *ss = g_ss_reg[i].ss;
        int n = ss->n < (int)(sizeof ss->va / sizeof ss->va[0]) ? ss->n : (int)(sizeof ss->va / sizeof ss->va[0]);
        printf("[GARDE] pile de %s : %d cadres\n", g_ss_reg[i].nom, ss->n);
        for (int k = n - 1, m = 0; k >= 0 && m < 16; k--, m++)
            printf("    #%-3d 0x%08X %s\n", k, ss->va[k], nom_jalon(ss->va[k]));
    }
    sc2_stop(13, "chien de garde SC2_WATCHDOG=%u : le run ne s'est pas terminé (voir les piles [GARDE] ci-dessus)", s);
    return NULL;
}
void sc2_watchdog_start(void)
{
    sc2_ss_register(&g_main_ss, "fil principal / fibre de boot");
    const char *e = getenv("SC2_WATCHDOG");
    if (!e || atoi(e) <= 0) return;
    pthread_t t;
    pthread_create(&t, NULL, watchdog, (void *)(uintptr_t)(unsigned)atoi(e));
    pthread_detach(t);
}

void sc2_arene_bilan(void);                             /* passe 7j, plus bas */
void sc2_arene_journal_fin(void);                       /* passe 10j, plus bas */
void sc2_stop(int code, const char *fmt, ...)
{
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once);            /* jamais relâché : un seul bilan, puis exit */
    va_list ap;
    printf("\n[ARRÊT] [thread %d] ", (int)gettid());
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n[ARRÊT] pile d'appels invitée (fantôme, la plus récente en haut) :\n");
    sc2_shadow *ss = g_sc2_ss;
    int n = ss->n < (int)(sizeof ss->va / sizeof ss->va[0]) ? ss->n : (int)(sizeof ss->va / sizeof ss->va[0]);
    for (int k = n - 1, shown = 0; k >= 0 && shown < 40; k--, shown++) {
        char ar[400]; args_cadre(ss, k, ar, sizeof ar);
        printf("    #%-3d 0x%08X %-40s %s\n", k, ss->va[k], nom_jalon(ss->va[k]), ar);
    }
    if (n > 40) printf("    … (%d cadres au total)\n", n);
    sc2_arene_bilan();                                  /* passe 7j : rien sans recyclage */
    sc2_mem_ligne("arrêt");                             /* V5.1 : occupation finale par origine */
    sc2_arene_journal_fin();                            /* passe 10j : rien sans SC2_ARENE_JOURNAL */
    pile_bilan();                                       /* passe 7w8 */
    manque_bilan();                                     /* passe 7v : rien sans SC2_MANQUE_CONTINUE */
    printf("[BILAN] jalons de boot atteints :\n");
    for (unsigned i = 0; i < sizeof g_jalons / sizeof g_jalons[0]; i++)
        printf("    [%c] 0x%08X %s\n", g_jalon_vu[i] ? 'x' : ' ', g_jalons[i].va, g_jalons[i].nom);
    printf("[BILAN] %llu dispatchs ; %d fonctions liftées distinctes exécutées sur %d disponibles\n",
           (unsigned long long)g_sc2_dispatches, g_n_executed, N_LIFTED);
    sc2_iat_report();
    fflush(stdout);
    if (sc2_sortie_crochet) sc2_sortie_crochet(code);  /* M10 : hôte Mac en mode fenêtre ; ne revient pas */
    exit(code);
}
void (*sc2_sortie_crochet)(int code) = NULL;            /* M10 : NULL (banc, Android) = exit(code) comme avant */

/* ========================================================================== */
/* Arène invitée                                                              */
/* ========================================================================== */

#define ARENA_BASE 0x08000000u          /* au-dessus de l'image (fin 0x035E4000) */
#define ARENA_SIZE 0x30000000u          /* 768 Mo réservés (MAP_NORESERVE) */
static uint32_t g_arena_top = ARENA_BASE, g_arena_ok;
/* Passe 10j — arène principale à DEUX BOUTS (recyclage actif seulement) : les GRANDS blocs (≥ HAUT_SEUIL) sont servis
 * depuis la fin de l'arène vers le bas (g_arena_haut), les autres depuis le début vers le haut (g_arena_top). Le jeu
 * fait passer chaque grand tampon par VirtualAlloc(MEM_TOP_DOWN) (0x009B85B0, dlmalloc) et en recycle des centaines
 * de tailles croissantes (6,4 Go servis en 20 min de cinématique pour 45 vivants) : mêlés aux petits blocs
 * persistants, leurs trous ne fusionnaient jamais (531 Mo d'étendues libres inutilisables, arène épuisée sur le Fold).
 * Sans recyclage : g_arena_haut reste à la fin, comportement d'avant. */
#define HAUT_SEUIL 0x100000u
#define QUAR_N 16384                                /* quarantaine des petits blocs libérés (voir sc2_free) */
#define QUAR_OCTETS (32u << 20)
static uint32_t g_quar[QUAR_N], g_quar_t, g_quar_n, g_quar_o;
static uint32_t g_arena_haut = ARENA_BASE + ARENA_SIZE;
/* Passe 10l — remise à zéro après un recul de front. Depuis la 10j, une étendue libre qui touche un front lui est
 * rendue (g_arena_top, g_arena_haut ou g_arena2_top recule) ; le chemin linéaire de sc2_alloc resservait ensuite cette
 * mémoire SANS la remettre à zéro, en la croyant neuve (mmap anonyme). Or sc2_alloc rend du zéro par contrat
 * (VirtualAlloc, HeapAlloc HEAP_ZERO_MEMORY, calloc : le dlmalloc du jeu tient une page VirtualAlloc pour nulle).
 * Symptôme : menu → campagne (beaucoup de grands blocs libérés à la sortie du front-end), tailles et pointeurs
 * absurdes ~600 frames plus tard (arène « épuisée » sur 0xF0697950 octets, memcpy vers 0x1F). Marques de plus haute
 * eau par front : seule la partie déjà servie une fois est remise à zéro ; la mémoire neuve garde le chemin d'avant. */
static uint32_t g_top_max = ARENA_BASE, g_haut_min = ARENA_BASE + ARENA_SIZE, g_a2_max;
static uint64_t g_n_rezero;
static pthread_mutex_t g_arena_mu = PTHREAD_MUTEX_INITIALIZER;   /* threads invités réels */
uint32_t sc2_arena_top(void)                       /* VirtualQuery : fin de la zone engagée de l'arène principale */
{
    return __atomic_load_n(&g_arena_haut, __ATOMIC_RELAXED) < ARENA_BASE + ARENA_SIZE ? ARENA_BASE + ARENA_SIZE
           : __atomic_load_n(&g_arena_top, __ATOMIC_RELAXED);
}
/* Passe 7h — arène de DÉBORDEMENT 0x80000000..0xF0000000 (1,75 Go, sous les VA factices des stubs d'import) :
 * SC2.exe est LARGE_ADDRESS_AWARE (pointeurs jusqu'à 4 Go) ; le chargement d'une carte dépasse les 768 Mo de
 * l'arène principale (TRaynor01 : 805 Mo demandés, mémoire vivante — aucun VirtualFree). Mappée seulement au
 * premier débordement (MAP_FIXED_NOREPLACE : rien d'existant n'est écrasé) ; un run qui tient dans l'arène
 * principale n'y touche jamais. */
/* SC2_ARENE2=<base>:<taille> (diagnostic) : autre emplacement de l'arène de débordement. */
static uint32_t ARENA2_BASE = 0x80000000u, ARENA2_SIZE = 0x70000000u;
static uint32_t g_arena2_top;
uint32_t sc2_arena2_top(void) { return __atomic_load_n(&g_arena2_top, __ATOMIC_RELAXED); }
uint32_t sc2_arena2_base(void) { return ARENA2_BASE; }
static int dans_arene(uint32_t va)                 /* va d'un bloc servi (en-tête compris à va - 16) */
{
    return (va >= ARENA_BASE + 16u && va < g_arena_top) || (va >= g_arena_haut + 16u && va < ARENA_BASE + ARENA_SIZE)
        || (g_arena2_top && va >= ARENA2_BASE + 16u && va < g_arena2_top);
}
static int arena2_init(void)                       /* sous g_arena_mu */
{
    if (g_arena2_top) return 1;
    const char *e = getenv("SC2_ARENE2"); unsigned b2, t2;
    if (e && sscanf(e, "%x:%x", &b2, &t2) == 2 && b2 >= 0x40000000u && t2 && (uint64_t)b2 + t2 <= 0xF0000000ull) {
        ARENA2_BASE = b2; ARENA2_SIZE = t2;
    }
    /* Passe 7j : le tas brk de l'hôte (exe lié à 0x40000000) est placé au hasard jusqu'à ~1 Go au-dessus de l'exe
     * (fin ~0x47C6A000) ; s'il a atterri vers 0x80000000, on essaie plus haut (0x90000000, 0xA0000000), toujours
     * sous 0xF0000000. Un run où 0x80000000 est libre est inchangé. */
    static const uint32_t repli[] = { 0x90000000u, 0xA0000000u };
    void *p = mmap(GPTR(ARENA2_BASE), ARENA2_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    for (unsigned k = 0; p != GPTR(ARENA2_BASE) && !(e && *e) && k < sizeof repli / sizeof repli[0]; k++) {
        if (p != MAP_FAILED) munmap(p, ARENA2_SIZE);
        sc2_log("[ARÈNE] arène de débordement 0x%08X indisponible (espace hôte occupé) : essai à 0x%08X", ARENA2_BASE, repli[k]);
        ARENA2_BASE = repli[k]; ARENA2_SIZE = 0xF0000000u - repli[k];
        p = mmap(GPTR(ARENA2_BASE), ARENA2_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    }
    if (p != GPTR(ARENA2_BASE)) {
        if (p != MAP_FAILED) munmap(p, ARENA2_SIZE);
        sc2_log("[ARÈNE] arène de débordement 0x%08X indisponible (espace hôte occupé)", ARENA2_BASE);
        return 0;
    }
    g_arena2_top = ARENA2_BASE;
    sc2_log("[ARÈNE] arène principale pleine (768 Mo) : arène de DÉBORDEMENT 0x%08X..0x%08X ouverte (exe LAA)",
            ARENA2_BASE, ARENA2_BASE + ARENA2_SIZE);
    return 1;
}
static uint64_t g_freed;
static struct timespec g_mem_t0;                     /* V5.1 : t=0 des lignes [MEM] (arena_init) */

static void arena_init(void)
{
    if (g_arena_ok) return;
    void *p = mmap(GPTR(ARENA_BASE), ARENA_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != GPTR(ARENA_BASE)) {
        fprintf(stderr, "sc2: arène invitée indisponible à 0x%08X\n", ARENA_BASE);
        exit(2);
    }
    g_arena_ok = 1;
    clock_gettime(CLOCK_MONOTONIC, &g_mem_t0);          /* V5.1 : origine des temps des lignes [MEM] */
}

/* Passe 7h — recyclage de l'arène (SC2_ARENE_RECYCLE=1, ou automatiquement avec SC2_RUN_SANS_BNET=1) : le
 * chargement d'une carte épuise les 768 Mo sans recyclage (805 Mo servis, TRaynor01). En-tête de bloc (16 o) :
 * [0] taille demandée, [4] capacité du bloc (en-tête compris), [8] marqueur « LIBR » quand le bloc est libre,
 * [12] chaînage de la liste libre. Listes par classe : 16 o de granularité jusqu'à 4 Ko (257 classes), puis par
 * puissance de 2 (premier bloc assez grand). Un bloc réutilisé est remis à zéro (sc2_alloc rend du zéro). Sans
 * la variable : allocateur linéaire d'avant, mêmes adresses (non-régression bit à bit). */
#define RECYCLE_LIBRE 0x5242494Cu                   /* « LIBR » */
#define RECYCLE_ALLOUE 0x4F4C4C41u                  /* « ALLO » : seul un bloc marqué peut être libéré */
static int g_recycle = -1;
static uint32_t g_libres_petits[258], g_libres_grands[33];
static uint64_t g_n_recycles, g_o_recycles;
static int recycle_actif(void)
{
    if (g_recycle < 0) {
        const char *a = getenv("SC2_ARENE_RECYCLE"), *b = getenv("SC2_RUN_SANS_BNET");
        g_recycle = (a && *a == '1') || (b && *b == '1' && !(a && *a == '0'));   /* SC2_ARENE_RECYCLE=0 : coupe */
        if (g_recycle) sc2_log("[ARÈNE] recyclage des blocs libérés ACTIF (listes par taille)");
    }
    return g_recycle;
}
int sc2_arene_recycle(void) { return recycle_actif(); }
static uint32_t *liste_de(uint32_t cap)
{
    if (cap <= 16u * 257u) return &g_libres_petits[cap / 16u];
    int k = 31 - __builtin_clz(cap); return &g_libres_grands[k];
}
static uint32_t recycle_prendre(uint32_t need)       /* sous g_arena_mu ; 0 si rien */
{
    if (need <= 16u * 257u) {
        uint32_t *l = &g_libres_petits[need / 16u], h = *l;
        if (h) { *l = *(uint32_t *)GPTR(h + 12u); return h; }
        return 0;
    }
    for (int k = 31 - __builtin_clz(need); k < 33; k++) {
        uint32_t *prec = &g_libres_grands[k];
        for (uint32_t h = *prec, n = 0; h && n < 64; prec = (uint32_t *)GPTR(h + 12u), h = *prec, n++)
            if (*(uint32_t *)GPTR(h + 4u) >= need) { *prec = *(uint32_t *)GPTR(h + 12u); return h; }
    }
    return 0;
}

/* Passe 7j — GRANDS blocs (≥ 64 Ko, recyclage actif seulement) : liste d'ÉTENDUES libres triée par adresse, fusionnée
 * avec ses voisines à la libération, servie au premier ajustement (le reste reste libre). Les listes par classe ne
 * réutilisent jamais un bloc pour une demande plus grande que tout ce qui a été rendu : or un tableau qui grandit par
 * pas fixe (le maillage découpé de 0x00CF95C0, +128 sommets de 36 o à chaque fois) alloue B(n+1), copie, rend B(n) —
 * 550 blocs de 1 à 2,5 Mo jamais réutilisés épuisaient 1,6 Go. Fusionnés, B(n) + B(n+1) contiennent B(n+2). */
#define GRAND_SEUIL 0x10000u
#define EXT_MAX 8192
static struct { uint32_t a, n; } g_ext[EXT_MAX];
static int g_next;
static uint64_t g_ext_servis, g_ext_rendus, g_n_rendus_front;
static uint32_t ext_prendre(uint32_t need)            /* sous g_arena_mu ; 0 si rien */
{
    int best = -1;                                      /* passe 10j : MEILLEUR ajustement (avant : le premier) — les */
    for (int i = 0; i < g_next; i++)                    /* grandes étendues restent entières pour les grandes demandes */
        if (g_ext[i].n >= need && (best < 0 || g_ext[i].n < g_ext[best].n)) best = i;
    if (best < 0) return 0;
    int i = best;
    uint32_t h = g_ext[i].a;
    if (g_ext[i].n - need < 64u) {                       /* reste trop petit pour un bloc : servi entier */
        need = g_ext[i].n;
        memmove(&g_ext[i], &g_ext[i + 1], (size_t)(g_next - i - 1) * sizeof g_ext[0]); g_next--;
    } else { g_ext[i].a += need; g_ext[i].n -= need; }
    *(uint32_t *)GPTR(h + 4u) = need;                   /* capacité réelle */
    g_ext_servis++;
    return h;
}
static int ext_rendre(uint32_t h, uint32_t cap)       /* sous g_arena_mu ; 0 si la table est pleine */
{
    int lo = 0, hi = g_next;                            /* premier indice d'adresse > h */
    while (lo < hi) { int m = (lo + hi) / 2; if (g_ext[m].a < h) lo = m + 1; else hi = m; }
    int av = lo > 0 && g_ext[lo - 1].a + g_ext[lo - 1].n == h, ap = lo < g_next && h + cap == g_ext[lo].a;
    if (av && ap) {
        g_ext[lo - 1].n += cap + g_ext[lo].n;
        memmove(&g_ext[lo], &g_ext[lo + 1], (size_t)(g_next - lo - 1) * sizeof g_ext[0]); g_next--;
    } else if (av) g_ext[lo - 1].n += cap;
    else if (ap) { g_ext[lo].a = h; g_ext[lo].n += cap; }
    else {
        if (g_next >= EXT_MAX) return 0;
        memmove(&g_ext[lo + 1], &g_ext[lo], (size_t)(g_next - lo) * sizeof g_ext[0]); g_next++;
        g_ext[lo].a = h; g_ext[lo].n = cap;
    }
    g_ext_rendus++;
    /* passe 10j : une étendue qui touche un front de l'arène lui est rendue (le front recule) — la mémoire redevient
     * contiguë, utilisable par toute taille ; seules les étendues extrêmes peuvent toucher un front */
    for (int bord = 0; bord < 2 && g_next; bord++) {
        int i = bord ? 0 : g_next - 1;
        if (g_ext[i].a + g_ext[i].n == g_arena_top) g_arena_top = g_ext[i].a;
        else if (g_arena2_top && g_ext[i].a + g_ext[i].n == g_arena2_top) g_arena2_top = g_ext[i].a;
        else if (g_ext[i].a == g_arena_haut) g_arena_haut += g_ext[i].n;
        else continue;
        g_n_rendus_front++;
        memmove(&g_ext[i], &g_ext[i + 1], (size_t)(g_next - i - 1) * sizeof g_ext[0]); g_next--;
        bord--;                                         /* la nouvelle extrême touche peut-être aussi un front */
    }
    return 1;
}
void sc2_arene_bilan(void)
{
    if (g_recycle == 1)
        sc2_log("[ARÈNE] bilan : %llu blocs recyclés (%llu o) ; grands blocs : %llu servis par étendue, %llu rendus, "
                "%d étendue(s) libre(s) ; passe 10j : %llu étendue(s) rendue(s) aux fronts, front bas %u Mo, front haut %u Mo, "
                "quarantaine %u blocs (%u Ko) ; passe 10l : %llu bloc(s) remis à zéro après un recul de front", (unsigned long long)g_n_recycles, (unsigned long long)g_o_recycles,
                (unsigned long long)g_ext_servis, (unsigned long long)g_ext_rendus, g_next, (unsigned long long)g_n_rendus_front,
                (g_arena_top - ARENA_BASE) >> 20, (ARENA_BASE + ARENA_SIZE - g_arena_haut) >> 20, g_quar_n, g_quar_o >> 10, (unsigned long long)g_n_rezero);
}

/* Passe 10j — journal d'arène, diagnostic : SC2_ARENE_JOURNAL=<secondes>. Chaque bloc servi garde dans son en-tête
 * ([12], libre tant que le bloc est alloué) l'index de son APPELANT invité (adresse de retour de l'import qui a
 * alloué : malloc, HeapAlloc, VirtualAlloc… ; 0 = allocation interne du runtime). Toutes les <secondes> :
 *   [ARÈNE-J] t=… principale … Mo / 768, débordement … Mo, vivants … Mo (n blocs), recyclage …, étendues libres …
 *   [ARÈNE-J]   appelant 0x… : … Mo vivants en … blocs (moy …), … allocations au total     (les 8 plus gros)
 * Sans la variable : aucun coût hors un test d'entier, en-têtes inchangés. */
extern __thread uint32_t sc2_appel_ret;
#define JNAPP 4096
#define JTAG 0x80000000u
static int g_jrn = -1; static double g_jrn_pas, g_jrn_suivant; static struct timespec g_jrn_t0;
static struct { uint32_t ret; uint64_t o_viv, n_viv, n_tot, o_tot; } g_japp[JNAPP];
static uint64_t g_j_viv, g_j_nviv, g_j_napp_pleins;
static double jrn_t(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)(t.tv_sec - g_jrn_t0.tv_sec) + (double)(t.tv_nsec - g_jrn_t0.tv_nsec) / 1e9; }
static int jrn_actif(void)
{
    if (g_jrn < 0) {
        const char *e = getenv("SC2_ARENE_JOURNAL");
        g_jrn = e && atof(e) > 0; g_jrn_pas = g_jrn ? atof(e) : 0; g_jrn_suivant = g_jrn_pas;
        clock_gettime(CLOCK_MONOTONIC, &g_jrn_t0);
        if (g_jrn) sc2_log("[ARÈNE-J] journal d'arène toutes les %.0f s (appelants invités des allocations)", g_jrn_pas);
    }
    return g_jrn;
}
static uint32_t jrn_idx(uint32_t ret)                 /* sous g_arena_mu */
{
    for (uint32_t k = (ret * 2654435761u) % JNAPP, n = 0; n < JNAPP; k = (k + 1) % JNAPP, n++) {
        if (g_japp[k].ret == ret && (g_japp[k].n_tot || !ret)) return k;
        if (!g_japp[k].n_tot && !g_japp[k].ret) { g_japp[k].ret = ret; return k; }
    }
    g_j_napp_pleins++; return 0;
}
static void jrn_rapport(double t)                     /* sous g_arena_mu */
{
    uint64_t o_ext = 0; for (int i = 0; i < g_next; i++) o_ext += g_ext[i].n;
    sc2_log("[ARÈNE-J] t=%.0f s : principale %u Mo / 768 (haut %u Mo), débordement %u Mo ; vivants %llu Mo (%llu blocs) ; recyclés %llu (%llu Mo) ; "
            "étendues libres %d (%llu Mo)%s", t, ((g_arena_top - ARENA_BASE) + (ARENA_BASE + ARENA_SIZE - g_arena_haut)) >> 20,
            (ARENA_BASE + ARENA_SIZE - g_arena_haut) >> 20, g_arena2_top ? (g_arena2_top - ARENA2_BASE) >> 20 : 0u,
            (unsigned long long)(g_j_viv >> 20), (unsigned long long)g_j_nviv, (unsigned long long)g_n_recycles,
            (unsigned long long)(g_o_recycles >> 20), g_next, (unsigned long long)(o_ext >> 20), g_recycle == 1 ? "" : " ; recyclage INACTIF");
    int vus[8]; int nv = 0;
    for (int r = 0; r < 8; r++) {
        int best = -1;
        for (int k = 0; k < JNAPP; k++) {
            if (!g_japp[k].n_tot) continue;
            int deja = 0; for (int q = 0; q < nv; q++) deja |= vus[q] == k;
            if (!deja && (best < 0 || g_japp[k].o_viv > g_japp[best].o_viv)) best = k;
        }
        if (best < 0 || !g_japp[best].o_viv) break;
        vus[nv++] = best;
        sc2_log("[ARÈNE-J]   appelant 0x%08X : %llu Mo vivants en %llu blocs (moy %llu o), %llu allocations (%llu Mo) au total",
                g_japp[best].ret, (unsigned long long)(g_japp[best].o_viv >> 20), (unsigned long long)g_japp[best].n_viv,
                (unsigned long long)(g_japp[best].n_viv ? g_japp[best].o_viv / g_japp[best].n_viv : 0),
                (unsigned long long)g_japp[best].n_tot, (unsigned long long)(g_japp[best].o_tot >> 20));
    }
}
#define MO_DECALE 24u                                 /* V5.1 : origine du bloc, bits 24..30 de [12] (journal : 31 et 0..11) */
#define MO_MASQUE 0x7F000000u
static void jrn_alloc(uint32_t hdr, uint32_t octets)  /* sous g_arena_mu */
{
    uint32_t k = jrn_idx(sc2_appel_ret);
    g_japp[k].o_viv += octets; g_japp[k].n_viv++; g_japp[k].n_tot++; g_japp[k].o_tot += octets;
    g_j_viv += octets; g_j_nviv++;
    *(uint32_t *)GPTR(hdr + 12u) = (*(uint32_t *)GPTR(hdr + 12u) & MO_MASQUE) | JTAG | k;   /* V5.1 : origine gardée */
    double t = jrn_t();
    if (t >= g_jrn_suivant) { g_jrn_suivant = t + g_jrn_pas; jrn_rapport(t); }
}
static void jrn_libere(uint32_t hdr)                  /* sous g_arena_mu ; bloc encore marqué */
{
    uint32_t tag = *(uint32_t *)GPTR(hdr + 12u), k = tag & (JNAPP - 1u);   /* V5.1 : bits de l'origine écartés */
    if (!(tag & JTAG)) return;
    uint64_t o = ((*(uint32_t *)GPTR(hdr) + 15u) & ~15u) + 16u;
    if (g_japp[k].o_viv >= o) g_japp[k].o_viv -= o; else g_japp[k].o_viv = 0;
    if (g_japp[k].n_viv) g_japp[k].n_viv--;
    if (g_j_viv >= o) g_j_viv -= o; else g_j_viv = 0;
    if (g_j_nviv) g_j_nviv--;
    *(uint32_t *)GPTR(hdr + 12u) = tag & MO_MASQUE;    /* V5.1 : l'origine reste, sc2_free la lit ensuite */
}
void sc2_arene_journal_fin(void) { if (g_jrn == 1) { pthread_mutex_lock(&g_arena_mu); jrn_rapport(jrn_t()); pthread_mutex_unlock(&g_arena_mu); } }

/* V5.1 (24/10) — mesure par ORIGINE (sc2_alloc_de) : octets vivants (capacité, en-tête compris), blocs vivants, octets
 * servis au total. Sans recyclage, rien n'est rendu : vivants = servis. Sous g_arena_mu. */
static uint64_t g_mo_viv[SC2_MO_N], g_mo_nviv[SC2_MO_N], g_mo_tot[SC2_MO_N];
static void mo_servi(unsigned o, uint32_t cap) { g_mo_viv[o] += cap; g_mo_nviv[o]++; g_mo_tot[o] += cap; }
static void mo_rendu(uint32_t tag, uint32_t cap)
{
    unsigned o = (tag & MO_MASQUE) >> MO_DECALE; if (o >= SC2_MO_N) o = SC2_MO_AUTRE;
    g_mo_viv[o] = g_mo_viv[o] >= cap ? g_mo_viv[o] - cap : 0; if (g_mo_nviv[o]) g_mo_nviv[o]--;
}
static unsigned origine_de(uint32_t va)              /* origine d'un bloc servi (recyclage actif), sinon AUTRE */
{
    if (g_recycle != 1 || !dans_arene(va)) return SC2_MO_AUTRE;
    const uint32_t *e = (const uint32_t *)GPTR(va - 16u);
    unsigned o = (e[3] & MO_MASQUE) >> MO_DECALE;
    return e[2] == RECYCLE_ALLOUE && o < SC2_MO_N ? o : SC2_MO_AUTRE;
}
static const char *const MO_NOMS[SC2_MO_N] = { "autre", "malloc", "tas", "virt", "section", "d3d-obj", "d3d-tex", "d3d-cube",
                                               "d3d-vol", "d3d-surf", "d3d-vb", "d3d-ib", "d3d-shader" };
/* Lignes [MEM] (V1.2) : occupation de l'arène (principale : fronts bas et haut ; débordement), vivants, mémoire libre
 * réutilisable (étendues, listes par taille, quarantaine), puis vivants et servis par origine, puis objets D3D9
 * (sc2_d3d9_mem). Verrou ESSAYÉ : appelée depuis sc2_stop (faute possible sous g_arena_mu), elle n'attend pas — sans
 * verrou, les compteurs sont lus tels quels et les listes libres ne sont pas parcourues. */
void sc2_mem_ligne(const char *motif)
{
    int verrou = 0;
    for (int k = 0; k < 200 && !(verrou = !pthread_mutex_trylock(&g_arena_mu)); k++) usleep(1000);
    uint64_t o_ext = 0, o_lis = 0, n_lis = 0;
    for (int i = 0; i < g_next && i < EXT_MAX; i++) o_ext += g_ext[i].n;
    if (verrou)                                         /* listes par taille : chaînes bornées, maillons vérifiés */
        for (int l = 0; l < 258 + 33; l++)
            for (uint32_t h = l < 258 ? g_libres_petits[l] : g_libres_grands[l - 258], n = 0; h && n < 4000000u; n++) {
                if (!dans_arene(h + 16u)) break;
                const uint32_t *e = (const uint32_t *)GPTR(h);
                o_lis += e[1]; n_lis++; h = e[3];
            }
    uint64_t viv[SC2_MO_N], nviv[SC2_MO_N], tot[SC2_MO_N], v = 0, nv = 0;
    memcpy(viv, g_mo_viv, sizeof viv); memcpy(nviv, g_mo_nviv, sizeof nviv); memcpy(tot, g_mo_tot, sizeof tot);
    uint32_t bas = g_arena_top - ARENA_BASE, haut = ARENA_BASE + ARENA_SIZE - g_arena_haut;
    uint32_t a2 = g_arena2_top ? g_arena2_top - ARENA2_BASE : 0u, quar = g_quar_o;
    int next = g_next;
    if (verrou) pthread_mutex_unlock(&g_arena_mu);
    for (int o = 0; o < SC2_MO_N; o++) { v += viv[o]; nv += nviv[o]; }
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    double s = (double)(t.tv_sec - g_mem_t0.tv_sec) + (double)(t.tv_nsec - g_mem_t0.tv_nsec) / 1e9;
    sc2_log("[MEM] %s t=%.1f s : arène principale %u Mo / 768 (bas %u, haut %u), débordement %u Mo ; vivants %.1f Mo en %llu blocs ; "
            "libres réutilisables : étendues %.1f Mo (%d), listes %.1f Mo (%llu blocs%s), quarantaine %.1f Mo%s", motif, s,
            (bas + haut) >> 20, bas >> 20, haut >> 20, a2 >> 20, (double)v / 1048576.0, (unsigned long long)nv,
            (double)o_ext / 1048576.0, next, (double)o_lis / 1048576.0, (unsigned long long)n_lis, verrou ? "" : ", non parcourues",
            (double)quar / 1048576.0, g_recycle == 1 ? "" : " ; recyclage INACTIF (rien n'est rendu)");
    char l1[1024], l2[1024]; size_t p1 = 0, p2 = 0;
    for (int o = 0; o < SC2_MO_N; o++) {
        if (!tot[o]) continue;
        p1 += (size_t)snprintf(l1 + p1, p1 < sizeof l1 ? sizeof l1 - p1 : 0, "%s%s %.1f (%llu)", p1 ? ", " : "", MO_NOMS[o],
                               (double)viv[o] / 1048576.0, (unsigned long long)nviv[o]);
        p2 += (size_t)snprintf(l2 + p2, p2 < sizeof l2 ? sizeof l2 - p2 : 0, "%s%s %.1f", p2 ? ", " : "", MO_NOMS[o],
                               (double)tot[o] / 1048576.0);
        if (p1 >= sizeof l1) p1 = sizeof l1 - 1;
        if (p2 >= sizeof l2) p2 = sizeof l2 - 1;
    }
    sc2_log("[MEM]   vivants par origine, Mo (blocs) : %s", p1 ? l1 : "—");
    sc2_log("[MEM]   servis au total par origine, Mo : %s", p2 ? l2 : "—");
    if (sc2_d3d9_mem) { char l3[1024]; l3[0] = 0; sc2_d3d9_mem(l3, sizeof l3); if (l3[0]) sc2_log("[MEM]   D3D9 : %s", l3); }
    if (sc2_virt_mem && sc2_mem_liberer()) { char l4[400]; l4[0] = 0; sc2_virt_mem(l4, sizeof l4); if (l4[0]) sc2_log("[MEM]   %s", l4); }
}

int sc2_mem_liberer(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("SC2_MEM_LIBERER");
        int r = !(e && *e == '0') && recycle_actif();
        sc2_log("[MEM] V5.1 : mémoire rendue par le jeu %s", r ? "RETOURNÉE à l'arène (ressources D3D9 au dernier Release, "
                "tableaux remplacés des UnicodeString ; SC2_MEM_LIBERER=0 pour couper)"
                : e && *e == '0' ? "gardée (SC2_MEM_LIBERER=0, comportement d'avant)" : "gardée (recyclage d'arène inactif)");
        __atomic_store_n(&v, r, __ATOMIC_RELEASE);
    }
    return v;
}
uint32_t sc2_alloc(uint32_t size) { return sc2_alloc_de(size, SC2_MO_AUTRE); }
uint32_t sc2_alloc_de(uint32_t size, unsigned origine)
{
    arena_init();
    if (origine >= SC2_MO_N) origine = SC2_MO_AUTRE;
    uint32_t need = ((size + 15u) & ~15u) + 16u;
    if (recycle_actif() && size <= ARENA_SIZE) {
        pthread_mutex_lock(&g_arena_mu);
        uint32_t h = need >= GRAND_SEUIL ? ext_prendre(need) : 0;
        if (!h) h = recycle_prendre(need);
        if (h) {
            uint32_t cap = *(uint32_t *)GPTR(h + 4u);
            g_n_recycles++; g_o_recycles += cap;
            mo_servi(origine, cap);                     /* V5.1 */
            pthread_mutex_unlock(&g_arena_mu);
            memset(GPTR(h + 16u), 0, cap - 16u);
            uint32_t *e = (uint32_t *)GPTR(h); e[0] = size; e[2] = RECYCLE_ALLOUE; e[3] = origine << MO_DECALE;
            if (jrn_actif()) { pthread_mutex_lock(&g_arena_mu); jrn_alloc(h, need); pthread_mutex_unlock(&g_arena_mu); }   /* passe 10j */
            return h + 16u;
        }
        pthread_mutex_unlock(&g_arena_mu);
    }
    pthread_mutex_lock(&g_arena_mu);
    uint32_t hdr, z0 = 0, z1 = 0;                       /* passe 10l : [z0, z1) déjà servi une fois → à remettre à zéro */
    if (g_recycle == 1 && need >= HAUT_SEUIL && size <= ARENA_SIZE && g_arena_haut - g_arena_top >= need) {
        g_arena_haut -= need;                           /* passe 10j : grand bloc servi depuis le front haut */
        hdr = g_arena_haut;
        if (hdr + need > g_haut_min) { z0 = hdr > g_haut_min ? hdr : g_haut_min; z1 = hdr + need; }
        if (hdr < g_haut_min) g_haut_min = hdr;
    } else if (size <= ARENA_SIZE && g_arena_haut - g_arena_top >= need) {   /* passe 10j : limite = front haut */
        hdr = g_arena_top;
        g_arena_top += need;
        if (hdr < g_top_max) { z0 = hdr; z1 = g_arena_top < g_top_max ? g_arena_top : g_top_max; }
        if (g_arena_top > g_top_max) g_top_max = g_arena_top;
    } else if (size <= ARENA2_SIZE && arena2_init() && g_arena2_top - ARENA2_BASE + need <= ARENA2_SIZE) {
        hdr = g_arena2_top;                             /* passe 7h : débordement (SC2.exe est LAA) */
        g_arena2_top += need;
        if (hdr < g_a2_max) { z0 = hdr; z1 = g_arena2_top < g_a2_max ? g_arena2_top : g_a2_max; }
        if (g_arena2_top > g_a2_max) g_a2_max = g_arena2_top;
    } else {
        pthread_mutex_unlock(&g_arena_mu);
        /* passe 10l : l'appelant — import invité en cours (sc2_appel_ret, 0 hors import) et adresse hôte (addr2line) */
        sc2_stop(5, "arène invitée épuisée (demande %u octets, %u déjà servis + %u en débordement) ; import invité en cours "
                 "revenant en 0x%08X, appelant hôte %p", size,
                 (g_arena_top - ARENA_BASE) + (ARENA_BASE + ARENA_SIZE - g_arena_haut), g_arena2_top ? g_arena2_top - ARENA2_BASE : 0u,
                 sc2_appel_ret, __builtin_return_address(0));
    }
    mo_servi(origine, need);                            /* V5.1 */
    pthread_mutex_unlock(&g_arena_mu);
    if (z1 > z0) { memset(GPTR(z0), 0, z1 - z0); g_n_rezero++; }   /* passe 10l : voir g_top_max */
    *(uint32_t *)GPTR(hdr) = size;                      /* en-tête : taille demandée */
    if (g_recycle == 1) {                               /* passe 7h : capacité + marqueur (recyclage seulement) */
        *(uint32_t *)GPTR(hdr + 4u) = need; *(uint32_t *)GPTR(hdr + 8u) = RECYCLE_ALLOUE;
        *(uint32_t *)GPTR(hdr + 12u) = origine << MO_DECALE;   /* V5.1 : neuf ou remis à zéro (10l) — origine seule */
    }
    if (jrn_actif()) { pthread_mutex_lock(&g_arena_mu); jrn_alloc(hdr, need); pthread_mutex_unlock(&g_arena_mu); }   /* passe 10j */
    return hdr + 16u;                                   /* zéro : mémoire anonyme neuve */
}

uint32_t sc2_alloc_size(uint32_t va) { return va ? *(uint32_t *)GPTR(va - 16u) : 0; }

uint32_t sc2_realloc(uint32_t va, uint32_t size)
{
    if (!va) return sc2_alloc(size);
    uint32_t old = sc2_alloc_size(va);
    if (size <= old) { *(uint32_t *)GPTR(va - 16u) = size; return va; }
    uint32_t n = sc2_alloc_de(size, origine_de(va));    /* V5.1 : même origine */
    memcpy(GPTR(n), GPTR(va), old);
    sc2_free(va);
    return n;
}

void sc2_free(uint32_t va)
{
    if (!va) return;
    g_freed++;
    if (g_jrn == 1 && dans_arene(va) && (g_recycle != 1 || ((uint32_t *)GPTR(va - 16u))[2] == RECYCLE_ALLOUE)) {   /* passe 10j : journal */
        pthread_mutex_lock(&g_arena_mu); jrn_libere(va - 16u); pthread_mutex_unlock(&g_arena_mu);
    }
    if (!g_recycle) return;
    if (!dans_arene(va)) return;
    uint32_t h = va - 16u, *e = (uint32_t *)GPTR(h);
    if (e[2] != RECYCLE_ALLOUE || e[1] < 32u || (e[1] & 15u)) return;   /* double libération / pas un début de bloc */
    pthread_mutex_lock(&g_arena_mu);
    e[2] = RECYCLE_LIBRE;
    mo_rendu(e[3], e[1]);                               /* V5.1 : avant que [12] ne serve de chaînage */
    if (e[1] < GRAND_SEUIL) {
        /* passe 10j — QUARANTAINE : un petit bloc libéré n'est réutilisé (donc remis à zéro) qu'après QUAR_N autres
         * libérations, ou QUAR_OCTETS octets. Un lecteur en retard sur un objet libéré (fil de chargement : saut null
         * 0x009DB0AA, vcall n°2 sur [entrée+0x40], scénario 1 de la passe 10h) relit l'ancien contenu au lieu de zéros,
         * comme sous Windows où le tas ne remet pas la mémoire libérée à zéro. */
        if (g_quar_n == QUAR_N || g_quar_o + e[1] > QUAR_OCTETS) {
            uint32_t v = g_quar[g_quar_t], *ve = (uint32_t *)GPTR(v), *l = liste_de(ve[1]);
            g_quar_t = (g_quar_t + 1) % QUAR_N; g_quar_n--; g_quar_o -= ve[1];
            ve[3] = *l; *l = v;
        }
        g_quar[(g_quar_t + g_quar_n) % QUAR_N] = h; g_quar_n++; g_quar_o += e[1];
    } else if (!ext_rendre(h, e[1])) { uint32_t *l = liste_de(e[1]); e[3] = *l; *l = h; }
    pthread_mutex_unlock(&g_arena_mu);
}

uint32_t sc2_reserve_stack(uint32_t size, uint32_t *limit)
{
    uint32_t base = sc2_alloc(size + 64u);
    if (limit) *limit = base;
    return (base + size) & ~15u;
}

/* ========================================================================== */
/* Image                                                                      */
/* ========================================================================== */

uintptr_t g_hote_delta = 0;  /* 0 sur Linux (mmap fixes) ; base hote - 0x00800000 sur macOS */

#ifdef __APPLE__
/* Passe 7u : la zone couvre aussi l'arène de débordement LAA 0x80000000..0xF0000000 (passe 7h) ; avec l'ancienne
 * fin 0x38000000, l'arène tombait hors zone et TRaynor01 mourait à la frame 466 (« arène invitée épuisée »).
 * Réservation MAP_NORESERVE : rien n'est engagé tant que l'invité n'y écrit pas.
 * Banc natif du 19/10 : g_hote_delta doit être ALIGNÉ SUR 4 Go, comme sous Android (sc2_android_hooks.c). Les stubs
 * rendent des pointeurs hôtes par GVA_OF(p) = (uint32_t)p (memchr, strchr, strstr…), exact seulement si les 32 bits
 * bas de l'adresse hôte sont la VA invitée. L'ancienne zone prise au hasard (delta +0x6FFF800000) rendait au décodeur
 * Ogg du jeu un memchr faux, puis NULL + 0xF dans L_0107F9F0 à la 40e frame. 4 Go réservés en PROT_NONE (toute VA
 * invitée hors zone faute, comme sous Linux), la zone [0x00800000, 0xF0000000) ouverte en lecture-écriture. */
static void zone_hote_init(void)
{
    const uintptr_t ZONE_DEBUT = 0x00800000u, ZONE_FIN = 0xF0000000u, QUATRE_GO = (uintptr_t)1 << 32;
    const int fl = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    uintptr_t base = 0;
    for (uintptr_t h = 0x1000000000ull; h <= 0x7000000000ull && !base; h += QUATRE_GO) {   /* 64 Go .. 448 Go */
        void *z = mmap((void *)h, QUATRE_GO, PROT_NONE, fl, -1, 0);
        if (z == MAP_FAILED) continue;
        if ((uintptr_t)z == h) base = h; else munmap(z, QUATRE_GO);
    }
    if (!base) {                                       /* à défaut : 8 Go réservés puis rognés à 4 Go alignés */
        void *z = mmap(NULL, 2 * QUATRE_GO, PROT_NONE, fl, -1, 0);
        if (z == MAP_FAILED) { perror("zone hote"); exit(2); }
        uintptr_t b = (uintptr_t)z, a = (b + QUATRE_GO - 1) & ~(QUATRE_GO - 1);
        if (a > b) munmap(z, a - b);
        if (b + 2 * QUATRE_GO > a + QUATRE_GO) munmap((void *)(a + QUATRE_GO), b + 2 * QUATRE_GO - (a + QUATRE_GO));
        base = a;
    }
    g_zone_hote = base + ZONE_DEBUT; g_zone_taille = ZONE_FIN - ZONE_DEBUT;   /* avant le mmap FIXED (mmap_zone) */
    void *z = mmap((void *)g_zone_hote, g_zone_taille, PROT_READ | PROT_WRITE, fl | MAP_FIXED, -1, 0);
    if (z != (void *)g_zone_hote) { perror("zone hote (ouverture)"); exit(2); }
    g_hote_delta = base;
    sc2_log("[ZONE] hote %p..%p, delta +0x%llX", z, (void *)(g_zone_hote + g_zone_taille),
            (unsigned long long)g_hote_delta);
}
#endif

int sc2_load_image(const char *path)
{
#ifdef __APPLE__
    zone_hote_init();
#endif
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    void *img = mmap(GPTR(SC2_IMAGE_BASE), SC2_SIZE_OF_IMAGE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (img != GPTR(SC2_IMAGE_BASE)) {
        fprintf(stderr, "sc2: impossible de mapper l'image à 0x%08X\n", SC2_IMAGE_BASE);
        close(fd);
        return -1;
    }
    if (pread(fd, img, SC2_SIZE_OF_HEADERS, 0) != (ssize_t)SC2_SIZE_OF_HEADERS) goto court;
#define SEC(name, va, vsize, off, rsize)                                              \
    if ((rsize) && pread(fd, GPTR(va), (rsize) < (vsize) || !(vsize) ? (rsize) : (vsize), \
                         (off)) <= 0) goto court;                                     \
    sc2_log("[IMAGE] %-7s 0x%08X  virt 0x%08X  fichier 0x%08X", name, (unsigned)(va), \
            (unsigned)(vsize), (unsigned)(rsize));
    SC2_SECTIONS(SEC)
#undef SEC
    close(fd);
    g_image_delta = 0;
    arena_init();
    return 0;
court:
    fprintf(stderr, "sc2: lecture incomplète de %s\n", path);
    close(fd);
    return -1;
}

/* TEB/PEB simulés (x86, NT) : ce que lit le code MSVC par fs: et les TLS implicites. */
uint32_t sc2_new_teb(uint32_t stack_top, uint32_t stack_limit)
{
    if (!g_sc2_peb) {
        g_sc2_peb = sc2_alloc(0x1000);
        wr8 (g_sc2_peb + 0x02, 1);                      /* BeingDebugged = cohérent avec IsDebuggerPresent */
        wr32(g_sc2_peb + 0x08, SC2_IMAGE_BASE);         /* ImageBaseAddress */
        wr32(g_sc2_peb + 0x18, 0x00150000u);            /* ProcessHeap (handle fictif) */
    }
    uint32_t teb = sc2_alloc(0x1000);
    wr32(teb + 0x00, 0xFFFFFFFFu);                      /* ExceptionList : fin de chaîne SEH */
    wr32(teb + 0x04, stack_top);                        /* StackBase */
    wr32(teb + 0x08, stack_limit);                      /* StackLimit */
    wr32(teb + 0x18, teb);                              /* Self */
    wr32(teb + 0x20, (uint32_t)getpid());               /* ClientId.UniqueProcess */
    wr32(teb + 0x24, (uint32_t)gettid());               /* ClientId.UniqueThread */
    wr32(teb + 0x30, g_sc2_peb);
#ifdef SC2_TLS_START
    /* TLS implicite (__declspec(thread)) : fs:[0x2C] → tableau ; [index] → bloc. */
    uint32_t tls_len = SC2_TLS_END - SC2_TLS_START;
    uint32_t blk = sc2_alloc(tls_len + SC2_TLS_ZERO_FILL + 16u);
    memcpy(GPTR(blk), GPTR(SC2_TLS_START), tls_len);
    uint32_t arr = sc2_alloc(64 * 4);
    uint32_t idx = rd32(SC2_TLS_INDEX_VA);
    wr32(arr + 4u * (idx & 63u), blk);
    wr32(teb + 0x2C, arr);
#endif
    return teb;
}

/* ========================================================================== */
/* Fautes matérielles                                                         */
/* ========================================================================== */

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    sc2_shadow *ss = g_sc2_ss;
    uint32_t fn = ss->n > 0 && ss->n <= (int)(sizeof ss->va / sizeof ss->va[0]) ? ss->va[ss->n - 1] : 0;
    const char *nom = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGFPE ? "SIGFPE" : "SIGILL";
    if (g_sc2_recover) {
        printf("[TOLÉRANT] %s (adresse 0x%lX) dans la fonction liftée 0x%08X, initialiseur 0x%08X : abandonné\n",
               nom, (unsigned long)(uintptr_t)si->si_addr, fn, g_sc2_recover_va);
        siglongjmp(*g_sc2_recover, 1);
    }
    /* RIP hôte (passe 4) : `addr2line -e recomp-sc2 <rip>` donne la ligne C liftée,
     * dont le commentaire nomme l'instruction invitée fautive. */
#if defined(__APPLE__) && defined(__aarch64__)
    unsigned long rip = uc ? (unsigned long)((ucontext_t *)uc)->uc_mcontext->__ss.__pc : 0;   /* Darwin M4 */
#elif defined(__x86_64__)
    unsigned long rip = uc ? (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP] : 0;
    /* Passe 7j : faute dans un accesseur non intégré (rd32/wr32… d'un build sans optimisation) — l'adresse de
     * retour hôte ([rbp+8], cadre standard de l'accesseur) situe l'appel dans la fonction liftée : `objdump -d`
     * autour d'elle, puis l'ordre des appels d'accesseurs suit celui des lignes C, donc des instructions invitées. */
    if (uc) {
        unsigned long rbp = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RBP];
        unsigned long rsp = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RSP];
        if (rbp && rbp >= rsp && rbp - rsp < 0x1000)
            printf("[ARRÊT] faute dans un accesseur : retour hôte [rbp+8] = 0x%lX\n", *(unsigned long *)(rbp + 8));
    }
#elif defined(__aarch64__)
    unsigned long rip = uc ? (unsigned long)((ucontext_t *)uc)->uc_mcontext.pc : 0;   /* Linux ARM64 (passe 5a) */
#else
    unsigned long rip = 0;
#endif
    /* Passe 7w8 : (1) faute dans la pile hôte du fil ou sa page de garde → DÉBORDEMENT DE PILE HÔTE ; (2) base de
     * l'image hôte et décalage du RIP : une .so Android est chargée à une base aléatoire, sans elle le RIP ne se
     * symbolise pas (`llvm-symbolizer --obj=<.so non strippée> <décalage>`). */
    unsigned long sp = 0;
#if defined(__APPLE__) && defined(__aarch64__)
    if (uc) sp = (unsigned long)((ucontext_t *)uc)->uc_mcontext->__ss.__sp;
#elif defined(__x86_64__)
    if (uc) sp = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
    if (uc) sp = (unsigned long)((ucontext_t *)uc)->uc_mcontext.sp;
#endif
    uintptr_t fa = (uintptr_t)si->si_addr, plo, phi;
    pile_courante(&plo, &phi);
    const char *quelle = GetCurrentFiber() && t_fib_hi ? "de la FIBRE" : "du fil";
    if (phi && fa + 0x10000u >= plo && fa < phi)
        printf("[ARRÊT] DÉBORDEMENT DE PILE HÔTE : faute 0x%lX dans la pile %s [0x%lX, 0x%lX) (%lu Kio) ou sa page de "
               "garde, sp 0x%lX (%ld o sous la base) ; profondeur max mesurée %lu Kio\n", (unsigned long)fa, quelle,
               (unsigned long)plo, (unsigned long)phi, (unsigned long)((phi - plo) >> 10), sp, (long)plo - (long)sp,
               (unsigned long)(g_pile_max >> 10));
    else
        printf("[ARRÊT] faute hors de la pile hôte %s [0x%lX, 0x%lX) ; sp 0x%lX\n", quelle, (unsigned long)plo,
               (unsigned long)phi, sp);
    Dl_info di;
    if (rip && dladdr((void *)rip, &di) && di.dli_fbase)
        printf("[ARRÊT] image hôte %s, base 0x%lX : RIP − base = 0x%lX (symbole le plus proche %s)\n",
               di.dli_fname ? di.dli_fname : "?", (unsigned long)(uintptr_t)di.dli_fbase,
               rip - (unsigned long)(uintptr_t)di.dli_fbase, di.dli_sname ? di.dli_sname : "?");
    sc2_stop(128 + sig, "%s : accès fautif à 0x%lX pendant la fonction liftée 0x%08X (RIP hôte 0x%lX)", nom,
             (unsigned long)(uintptr_t)si->si_addr, fn, rip);
}

void sc2_thread_fault_stack(void)
{
#ifdef __APPLE__                                        /* passe 7w8 : bornes de la pile hôte de ce fil */
    t_pile_hi = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    t_pile_lo = t_pile_hi - pthread_get_stacksize_np(pthread_self());
#else
    pthread_attr_t pa;
    if (!pthread_getattr_np(pthread_self(), &pa)) {
        void *lo = NULL; size_t n = 0;
        if (!pthread_attr_getstack(&pa, &lo, &n) && lo) { t_pile_lo = (uintptr_t)lo; t_pile_hi = (uintptr_t)lo + n; }
        pthread_attr_destroy(&pa);
    }
#endif
    size_t n = 256 * 1024;
    stack_t st = { .ss_sp = malloc(n), .ss_size = n, .ss_flags = 0 };
    if (st.ss_sp) sigaltstack(&st, NULL);
}

void sc2_install_fault_handler(void)
{
    sc2_thread_fault_stack();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);  sigaction(SIGILL, &sa, NULL);
    lift_sante();                                       /* passe 7w8 */
    atexit(pile_bilan);
}
