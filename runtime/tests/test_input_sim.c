/* test_input_sim.c — M1 (V3, 22/10) : API d'entrées de bureau du runtime (sc2_input_sim.c).
 *
 * Premier test hôte de sc2_input_sim.c lui-même : le module est INCLUS (ses états static, I et ACT, et ses imports
 * i_PeekMessageW / i_GetMessageW / i_TranslateMessage / i_DispatchMessageW vivent dans cette unité), donc
 * sc2_entrees_banc.c aussi (inclus par lui, inerte sans SC2_BANC_ENTREES). Mémoire invitée : un tampon hôte de 1 Mio
 * en VA 0x01600000 (GPTR = g_hote_delta + va) qui couvre le MSG de la pompe, sa pile et le drapeau d'activation du jeu
 * [0x0169A62C]. La pompe du jeu (recomp_06_0.c, inventaire v3-entrees §1.2) est rejouée telle quelle : PeekMessageW
 * (PM_NOREMOVE) depuis 0x0099A3CC (un « tour »), puis tant que la file n'est pas vide GetMessageW → TranslateMessage →
 * DispatchMessageW → PeekMessageW. La wndproc du jeu est un bouchon de sc2_call_guest qui journalise les messages livrés
 * et, comme L_00804490, recopie wParam de WM_ACTIVATEAPP dans [0x0169A62C].
 * Bouchons : sc2_log, sc2_stop, sc2_call_guest, sc2_glue_pompe, sc2_iat_p4_find (GetActiveWindow, GetWindowLongA),
 * d3dv_actif, d3dv_lire, g_hote_delta. Après la fusion de V1.1 (sc2_entrees_banc.c étendu) : ses références FAIBLES
 * (sc2_d3d9_taille, sc2_glue_masque, sc2_frames_presentees) restent indéfinies — -U sur Mach-O dans le fragment
 * tests-hote/v3/entrees_runtime.cmake — et il faut Threads (déjà lié).
 *
 *   cc -std=gnu17 -Wall -Wextra -I recomp-sc2/runtime recomp-sc2/tests/test_input_sim.c -lpthread -o /tmp/t && /tmp/t
 * Sortie : une ligne PASS/FAIL par assertion, puis « RÉSULTAT : p PASS, f FAIL » ; code 0 sans échec.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sc2_input_sim.c"

/* ---- mémoire invitée ----------------------------------------------------------------------------------------- */
#define ZONE_VA     0x01600000u
#define ZONE_TAILLE 0x00100000u                       /* 1 Mio : ACTIF_VA (0x0169A62C) en fait partie */
#define MSG_VA      0x01600100u                       /* MSG (28 octets) de la pompe */
#define PILE_VA     0x01601000u                       /* sommet de pile : adresse de retour de l'import */
#define BOUCLE_VA   0x0099A48Bu                       /* retour du PeekMessageW de la boucle (≠ POMPE_VA) */
#define HWND_TEST   0x00C0FFEEu
#define WNDPROC_VA  0x0099A600u
enum { WM_ACTIVATE_ = 0x06, WM_ACTIVATEAPP_ = 0x1C };
uintptr_t g_hote_delta;

static int g_pass, g_fail, g_inattendus;
#define CHECK(cond, msg) do { if (cond) { g_pass++; printf("PASS  %s\n", msg); } \
                              else { g_fail++; printf("FAIL  %s\n", msg); } } while (0)

/* ---- bouchons du runtime --------------------------------------------------------------------------------------- */
static char g_dern_activation[512], g_dern_relache[512];
void sc2_log(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (!strncmp(b, "[ACTIVATION]", 12)) snprintf(g_dern_activation, sizeof g_dern_activation, "%s", b);
    if (strstr(b, "tout relâché")) snprintf(g_dern_relache, sizeof g_dern_relache, "%s", b);
    printf("[journal-test] %s\n", b);
}
void sc2_stop(int code, const char *fmt, ...)
{
    va_list ap;
    printf("FAIL  sc2_stop(%d) inattendu : ", code);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n');
    exit(1);
}
void sc2_glue_pompe(CPU *c) { (void)c; }
int d3dv_actif(void) { return 0; }
int d3dv_lire(int id, uint8_t *rgba) { (void)id; (void)rgba; return 1; }

/* imports d'origine (sc2_iat_p4.c) consultés par le module : fenêtre active et wndproc de la fenêtre */
static uint32_t b_GetActiveWindow(CPU *c, const uint32_t *a) { (void)c; (void)a; return HWND_TEST; }
static uint32_t b_GetWindowLongA(CPU *c, const uint32_t *a)
{ (void)c; return a[0] == HWND_TEST && a[1] == 0xFFFFFFFCu ? WNDPROC_VA : 0; }
static const imp_def g_p4[] = { { "GetActiveWindow", 0, STD, b_GetActiveWindow }, { "GetWindowLongA", 2, STD, b_GetWindowLongA } };
const imp_def *sc2_iat_p4_find(const char *nom)
{
    for (unsigned k = 0; k < sizeof g_p4 / sizeof g_p4[0]; k++) if (!strcmp(g_p4[k].nom, nom)) return &g_p4[k];
    return NULL;
}

/* wndproc du jeu : messages livrés, dans l'ordre ; WM_ACTIVATEAPP écrit le drapeau d'activation (si g_jeu_suit) */
typedef struct { uint32_t msg, wp, lp; } livre_t;
static livre_t L[4096];
static int g_nl, g_jeu_suit = 1;
uint32_t sc2_call_guest(CPU *c, uint32_t va, int nargs, const uint32_t *args, int cdecl)
{
    (void)c;
    if (va != WNDPROC_VA || nargs != 4 || cdecl || args[0] != HWND_TEST) { g_inattendus++; return 0; }
    if (g_nl < (int)(sizeof L / sizeof L[0])) L[g_nl++] = (livre_t){ args[1], args[2], args[3] };
    if (args[1] == WM_ACTIVATEAPP_ && g_jeu_suit) wr32(ACTIF_VA, args[2] ? 1u : 0u);
    return 0;
}

/* ---- la pompe du jeu ------------------------------------------------------------------------------------------- */
static CPU g_cpu;
static int g_n_traduits;                              /* TranslateMessage qui ont rendu ≠ 0 (WM_CHAR synthétisé) */
static void tour(void)
{
    uint32_t pk[5] = { MSG_VA, 0, 0, 0, 0 }, gm[4] = { MSG_VA, 0, 0, 0 }, tm[1] = { MSG_VA };
    g_cpu.esp = PILE_VA;
    wr32(PILE_VA, POMPE_VA);
    uint32_t r = i_PeekMessageW(&g_cpu, pk);
    wr32(PILE_VA, BOUCLE_VA);
    for (int garde = 0; r && garde < 2048; garde++) {
        i_GetMessageW(&g_cpu, gm);
        if (i_TranslateMessage(&g_cpu, tm)) g_n_traduits++;
        i_DispatchMessageW(&g_cpu, tm);
        r = i_PeekMessageW(&g_cpu, pk);
    }
}
static void tours(int n) { while (n-- > 0) tour(); }
static int compter(uint32_t msg, int64_t wp)          /* wp < 0 : tout wParam */
{
    int n = 0;
    for (int k = 0; k < g_nl; k++) if (L[k].msg == msg && (wp < 0 || L[k].wp == (uint32_t)wp)) n++;
    return n;
}
static int est(int k, uint32_t msg, uint32_t wp) { return k < g_nl && L[k].msg == msg && L[k].wp == wp; }
static void reinit(void)
{
    pthread_mutex_lock(&I.mu);
    I.fdeb = I.flen = 0; I.boutons = 0; I.x = 512; I.y = 384;
    memset(I.bas, 0, sizeof I.bas); memset(I.bascule, 0, sizeof I.bascule);
    I.dernier_fourni = 0; I.dernier_lp = 0;
    pthread_mutex_unlock(&I.mu);
    memset(&ACT, 0, sizeof ACT); ACT.mode = -1;
    wr32(ACTIF_VA, 1);                                 /* jeu actif : aucune auto-activation parasite */
    g_nl = 0; g_jeu_suit = 1; g_n_traduits = 0;
    g_dern_activation[0] = g_dern_relache[0] = 0;
}

/* ---- cas ------------------------------------------------------------------------------------------------------- */
static void cas_module_inactif(void)                  /* en premier : le module n'est pas encore actif */
{
    printf("\n== module inactif : état mémorisé, rien de posté\n");
    CHECK(!sc2_input_actif() && !sc2_input_find("PeekMessageW"), "module inactif au départ : sc2_input_find rend NULL (imports d'avant)");
    wr32(ACTIF_VA, 0);
    sc2_input_activation(0);
    CHECK(I.flen == 0 && ACT.hote_inactif == 1 && strstr(g_dern_activation, "[ACTIVATION] hôte : inactif")
          && strstr(g_dern_activation, "état mémorisé"), "module inactif : activation(0) mémorisée, rien de posté, journal [ACTIVATION]");
    sc2_input_activer();
    CHECK(sc2_input_find("PeekMessageW") && sc2_input_find("TranslateMessage") && sc2_input_find("GetMessageW"),
          "sc2_input_activer : PeekMessageW, GetMessageW et TranslateMessage servis");
    tours(200);
    CHECK(compter(WM_ACTIVATEAPP_, -1) == 0 && rd32(ACTIF_VA) == 0,
          "état inactif mémorisé puis module actif : 200 tours, aucun WM_ACTIVATEAPP (auto-activation suspendue)");
    sc2_input_activation(1);
    tour();
    CHECK(g_nl == 2 && est(0, WM_ACTIVATEAPP_, 1) && est(1, WM_ACTIVATE_, 1) && rd32(ACTIF_VA) == 1,
          "puis activation(1) : WM_ACTIVATEAPP(1) + WM_ACTIVATE(WA_ACTIVE) au tour suivant, jeu actif");
}

static void cas_translate(void)
{
    uint32_t gm[4] = { MSG_VA, 0, 0, 0 }, tm[1] = { MSG_VA };
    printf("\n== TranslateMessage : coupée pour les seuls messages marqués\n");
    reinit();
    sc2_input_touche_plateforme(0x41, 1);
    i_GetMessageW(&g_cpu, gm);
    int f0 = I.flen;
    uint32_t r = i_TranslateMessage(&g_cpu, tm);
    CHECK(rd32(MSG_VA + 4) == WM_KEYDOWN && rd32(MSG_VA + 8) == 0x41 && I.dernier_fourni == 1,
          "touche_plateforme(0x41, 1) : WM_KEYDOWN 0x41 pris, marqué");
    CHECK(r == 0 && I.flen == f0, "TranslateMessage rend 0 pour un message marqué : aucun WM_CHAR synthétisé");
    sc2_input_touche_plateforme(0x41, 0);
    i_GetMessageW(&g_cpu, gm);
    reinit();
    sc2_input_touche(0x41, 1);
    i_GetMessageW(&g_cpu, gm);
    uint32_t lp_bas = rd32(MSG_VA + 12);
    r = i_TranslateMessage(&g_cpu, tm);
    const msg_t *m = &I.file[I.fdeb % 256];
    CHECK(I.dernier_fourni == 0 && r == 1 && I.flen == 1 && m->msg == WM_CHAR && m->wp == 'a' && m->lp == lp_bas,
          "sc2_input_touche(0x41, 1) (banc, Android) : WM_CHAR 'a' toujours synthétisé, lParam du KEYDOWN");
    reinit();
    sc2_input_touche(0x41, 1); sc2_input_touche(0x41, 0);
    tour();
    CHECK(g_nl == 3 && est(0, WM_KEYDOWN, 0x41) && est(1, WM_KEYUP, 0x41) && est(2, WM_CHAR, 'a') && g_n_traduits == 1,
          "banc : KEY:0x41 en un tour → KEYDOWN, KEYUP, CHAR 'a' (ordre d'avant, inchangé)");
    reinit();
    sc2_input_touche_plateforme(0x32, 1); sc2_input_caractere(0xE9); sc2_input_touche_plateforme(0x32, 0);
    tour();
    CHECK(g_nl == 3 && est(1, WM_CHAR, 0xE9) && compter(WM_CHAR, -1) == 1 && g_n_traduits == 0,
          "AZERTY : VK 0x32 + « é » fourni → WM_CHAR 0xE9 seul (la synthèse US aurait donné '2')");
}

static void cas_ordre(void)
{
    printf("\n== ordre KEYDOWN, CHAR, KEYUP\n");
    reinit();
    sc2_input_touche_plateforme(0x41, 1); sc2_input_caractere('a'); sc2_input_touche_plateforme(0x41, 0);
    tour();
    CHECK(g_nl == 3 && est(0, WM_KEYDOWN, 0x41) && est(1, WM_CHAR, 'a') && est(2, WM_KEYUP, 0x41) && g_n_traduits == 0,
          "plate-forme : KEYDOWN 0x41, CHAR 'a', KEYUP 0x41 en un tour, aucun WM_CHAR en plus");
    CHECK(g_nl == 3 && L[1].lp == L[0].lp && ((L[0].lp >> 16) & 0xFF) == 0x1E && (L[0].lp & 0xFFFF) == 1,
          "lParam du WM_CHAR = celui du KEYDOWN (répétition 1, scan 0x1E)");
    reinit();
    sc2_input_caractere('z');
    tour();
    CHECK(g_nl == 1 && est(0, WM_CHAR, 'z') && L[0].lp == 1, "caractère sans KEYDOWN de la plate-forme : lParam 1");
}

static void cas_substitution(void)
{
    printf("\n== hors BMP : paire de substitution\n");
    reinit();
    sc2_input_caractere(0x1F600);
    tour();
    CHECK(g_nl == 2 && est(0, WM_CHAR, 0xD83D) && est(1, WM_CHAR, 0xDE00), "U+1F600 → WM_CHAR 0xD83D puis 0xDE00");
    reinit();
    sc2_input_caractere(0xD83D); sc2_input_caractere(0xDE00);
    tour();
    CHECK(g_nl == 2 && est(0, WM_CHAR, 0xD83D) && est(1, WM_CHAR, 0xDE00), "unités UTF-16 isolées (NSString) : passées telles quelles");
    reinit();
    sc2_input_caractere(0); sc2_input_caractere(0x110000);
    tour();
    CHECK(g_nl == 0, "0 et > U+10FFFF : ignorés");
}

static void cas_alt(void)
{
    printf("\n== Alt tenue : WM_SYSCHAR\n");
    reinit();
    sc2_input_touche_plateforme(0xA4, 1); sc2_input_touche_plateforme(0x41, 1); sc2_input_caractere(0xE5);
    sc2_input_touche_plateforme(0x41, 0); sc2_input_touche_plateforme(0xA4, 0);
    tour();
    CHECK(g_nl == 5 && est(1, WM_SYSKEYDOWN, 0x41) && est(2, WM_SYSCHAR, 0xE5) && (L[2].lp & (1u << 29)) && L[2].lp == L[1].lp,
          "Alt tenue : WM_SYSKEYDOWN 0x41 puis WM_SYSCHAR 0xE5 (lParam du KEYDOWN, bit 29)");
    CHECK(compter(WM_SYSCHAR, -1) == 1 && compter(WM_CHAR, -1) == 0 && g_n_traduits == 0, "Alt : aucun WM_(SYS)CHAR synthétisé en plus");
}

static void cas_activation(void)
{
    printf("\n== activation : auto-activation suspendue par l'hôte\n");
    reinit();
    wr32(ACTIF_VA, 0);
    tour();
    CHECK(compter(WM_ACTIVATEAPP_, 1) == 1 && rd32(ACTIF_VA) == 1, "témoin : drapeau à 0, l'auto-activation poste WM_ACTIVATEAPP(1) au 1er tour");
    g_nl = 0;
    sc2_input_activation(0);
    CHECK(I.flen == 2 && strstr(g_dern_activation, "[ACTIVATION] hôte : inactif (tour "),
          "activation(0) : WM_ACTIVATEAPP(0) + WM_ACTIVATE(WA_INACTIVE) postés tout de suite, journal [ACTIVATION]");
    tours(600);
    CHECK(est(0, WM_ACTIVATEAPP_, 0) && est(1, WM_ACTIVATE_, 0) && rd32(ACTIF_VA) == 0,
          "livrés au tour suivant : WM_ACTIVATEAPP(0), WM_ACTIVATE(WA_INACTIVE) ; drapeau [0x0169A62C] à 0");
    CHECK(compter(WM_ACTIVATEAPP_, 1) == 0 && g_nl == 2, "drapeau à 0 et activation(0) : 600 tours de pompe, aucun WM_ACTIVATEAPP(1)");
    sc2_input_activation(0);
    CHECK(I.flen == 0 && strstr(g_dern_activation, "inchangé"), "activation(0) répétée : rien de posté");
    g_nl = 0;
    sc2_input_activation(1);
    tour();
    CHECK(g_nl == 2 && est(0, WM_ACTIVATEAPP_, 1) && est(1, WM_ACTIVATE_, 1) && rd32(ACTIF_VA) == 1,
          "activation(1) : WM_ACTIVATEAPP(1) + WM_ACTIVATE(WA_ACTIVE) au tour suivant, jeu actif");
    tours(300);
    CHECK(g_nl == 2, "jeu actif : 300 tours de plus, rien d'autre (aucun doublon)");

    printf("\n== activation : jeu sourd à activation(1) → l'auto-activation reprend 60 tours plus tard\n");
    sc2_input_activation(0); tour();
    g_nl = 0; g_jeu_suit = 0;
    sc2_input_activation(1);
    tours(59);
    int n59 = compter(WM_ACTIVATEAPP_, 1);
    tour();
    int n60 = compter(WM_ACTIVATEAPP_, 1);
    CHECK(n59 == 1 && n60 == 2, "envoi de l'hôte, puis l'auto-activation au 60e tour seulement (aucun doublon au même tour)");

    printf("\n== activation : SC2_ACTIVATION=0\n");
    reinit();
    setenv("SC2_ACTIVATION", "0", 1);
    wr32(ACTIF_VA, 0);
    tours(200); sc2_input_activation(0); tours(10); sc2_input_activation(1); tours(10);
    CHECK(g_nl == 0 && I.flen == 0 && strstr(g_dern_activation, "SC2_ACTIVATION=0"),
          "SC2_ACTIVATION=0 : ni l'auto-activation ni activation(0)/(1) ne postent quoi que ce soit");
    unsetenv("SC2_ACTIVATION");
}

static void cas_tout_relacher(void)
{
    printf("\n== tout_relacher\n");
    reinit();
    sc2_input_touche_plateforme(0x41, 1); sc2_input_touche_plateforme(0xA0, 1); sc2_input_touche_plateforme(0xA3, 1);
    sc2_input_touche(0x14, 1); sc2_input_touche(0x14, 0);                     /* Verr. maj : bascule à 1, relâchée */
    sc2_input_souris(100, 200); sc2_input_bouton(0, 1); sc2_input_bouton(2, 1);
    tour();
    g_nl = 0;
    sc2_input_tout_relacher();
    tour();
    CHECK(g_nl == 5 && est(0, WM_KEYUP, 0x41) && est(1, WM_KEYUP, 0x10) && est(2, WM_KEYUP, 0x11)
          && est(3, WM_LBUTTONUP, 0x02 /* MK_RBUTTON encore tenu */) && est(4, WM_RBUTTONUP, 0),
          "un KEYUP par touche tenue (0x41, Maj g. → 0x10, Ctrl d. → 0x11), puis WM_LBUTTONUP, WM_RBUTTONUP");
    int tout = 1;
    for (int k = 0; k < 256; k++) if (I.bas[k] || sc2_input_async((uint32_t)k)) tout = 0;
    CHECK(tout && I.boutons == 0 && (L[0].lp & 0xC0000000u) == 0xC0000000u && L[3].lp == ((200u << 16) | 100u),
          "tout relâché : GetAsyncKeyState à 0 partout, boutons à 0 ; KEYUP bits 30-31, clic à la position du curseur");
    CHECK(I.bascule[0x14] == 1 && (sc2_input_etat(0x14) & 1) && strstr(g_dern_relache, "3 touche(s), 2 bouton(s)"),
          "état bascule intact (Verr. maj) ; journal du relâché");
    g_nl = 0;
    sc2_input_tout_relacher();
    tour();
    CHECK(g_nl == 0, "rien de tenu : rien de posté");
    reinit();
    sc2_input_touche_plateforme(0xA4, 1); sc2_input_touche_plateforme(0x41, 1); sc2_input_touche(0x10, 1);
    tour();
    g_nl = 0;
    sc2_input_tout_relacher();
    tour();
    CHECK(g_nl == 3 && est(0, WM_SYSKEYUP, 0x41) && est(1, WM_SYSKEYUP, 0x10) && est(2, WM_KEYUP, 0x12),
          "Alt tenue : SYSKEYUP 0x41, SYSKEYUP 0x10 (Maj générique → variante gauche, un seul message), puis KEYUP 0x12");
}

int main(void)
{
    unsetenv("SC2_INPUT_SIM"); unsetenv("SC2_BANC_ENTREES"); unsetenv("SC2_ACTIVATION");
    void *zone = calloc(1, ZONE_TAILLE);
    if (!zone) { printf("FAIL  mémoire invitée\n"); return 1; }
    g_hote_delta = (uintptr_t)zone - ZONE_VA;
    cas_module_inactif();
    cas_translate();
    cas_ordre();
    cas_substitution();
    cas_alt();
    cas_activation();
    cas_tout_relacher();
    CHECK(g_inattendus == 0, "aucun appel invité inattendu (wndproc, fenêtre)");
    printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
