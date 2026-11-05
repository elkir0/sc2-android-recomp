/* sc2_input_sim.c — état d'entrée injectable et file de messages Win32 du jeu (passe 5f).
 *
 * Imports servis quand le module est ACTIF (SC2_INPUT_SIM=<fichier>, ou sc2_input_activer()) :
 *   PeekMessageW, GetMessageW, TranslateMessage, DispatchMessageW, GetCursorPos, GetKeyboardState
 *   (via sc2_input_find, consulté en tête de sc2_iat_p4_find) ; GetAsyncKeyState et GetKeyState
 *   restent dans g_defs (sc2_iat.c) et lisent sc2_input_async / sc2_input_etat.
 * Avant ce module : PeekMessageW rendait toujours « file vide », DispatchMessageW n'appelait pas la
 * wndproc, GetCursorPos rendait (512, 384) et GetMessageW n'existait pas côté runtime (servi par la
 * glue Android, par --wrap, qui garde la priorité). Inactif : sc2_input_find rend NULL → inchangé.
 *
 * Horloge des scripts : « frame n » = n-ième appel de la pompe du jeu (PeekMessageW dont l'adresse de
 * retour est 0x0099A3CC, une fois par frame au boot) — pas d'accès au compteur de Present (sc2_d3d9).
 * Chaque message injecté qui atteint la wndproc du jeu est tracé [INPUT] avec la valeur rendue.
 *
 * V3 (M1, 22/10) — plate-forme de bureau qui fournit ses CARACTÈRES (Mac : NSEvent.characters) : un message clavier posté
 * par sc2_input_touche_plateforme est MARQUÉ (msg_t.fourni) ; prendre(retirer) recopie la marque dans I.dernier_fourni et
 * i_TranslateMessage n'en synthétise aucun WM_CHAR (la pompe enchaîne GetMessageW → TranslateMessage → DispatchMessageW
 * sans autre prise entre deux) ; les caractères arrivent par sc2_input_caractere. Focus : sc2_input_activation suspend
 * l'auto-activation (activation()) tant que l'hôte dit « inactif », sc2_input_tout_relacher relâche touches et boutons.
 * Sans ces appels (banc, SC2_INPUT_SIM, SC2_BANC_ENTREES, Android) : comportement d'avant, synthèse US comprise.
 */
#define _GNU_SOURCE
#include "sc2_input.h"
#include "sc2_host.h"
#include "sc2_horloge.h"                              /* passe 9j : MSG.time, GetMessageTime */
#include "sc2_d3d9v.h"                                  /* passe 7u : d3dv_lire (lecture seule de l'écran) */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sc2_entrees_banc.c"                         /* passe 10i : SC2_BANC_ENTREES (secondes) */

#define A(i) (a[(i)])
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP                  /* glibc, bionic ; macOS : sans _NP */
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif
#define POMPE_VA 0x0099A3CCu                      /* retour de l'appel PeekMessageW de la pompe du jeu */
enum { WM_NULL = 0, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_MOUSEMOVE = 0x200,
       WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205,
       WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208,
       WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105, WM_SYSCHAR = 0x106, WM_MOUSEWHEEL = 0x20A, WM_MOUSEHWHEEL = 0x20E };   /* 8h */

typedef struct { uint32_t frame; int genre; uint32_t vk; int32_t x, y; int bouton, bas, deplace;   /* genre 2 : molette (8h), vk = delta */
                 int quand; uint32_t max;            /* genre 0 touche, 1 souris, 2 molette ; quand/max : passe 7u (0 = sans condition) */
                 int q_texte, q_pv, q_seuil;         /* passe 8e : quand:texte= (motif + 1) / quand:pv= (max PV, seuil) */
                 int c_sens, c_texte, c_pv, c_seuil; /* 8e : sauf: (1) / si: (2) sur un texte ou un relevé de PV */
                 int saute, bloc, repere;            /* 8e : sinon:saute, bloc:<n>, repere */
                 uint32_t debut; uint64_t seq0; } evt_t;  /* 8e : début de l'attente (tour, n° de texte lu) */
typedef struct { uint32_t hwnd, msg, wp, lp; int32_t x, y; uint8_t fourni; } msg_t;   /* M1 : fourni = caractères par la plate-forme */

static struct {
    int actif, essaye, contourne;
    pthread_mutex_t mu;
    uint8_t bas[256], bascule[256];
    int32_t x, y;
    int boutons;                                  /* bits : 1 gauche, 2 droit, 4 milieu (MK_*) */
    evt_t *ev; int nev, iev;
    msg_t file[256]; int fdeb, flen;
    uint32_t frame, temps;                   /* temps : heure (ms, sc2_tick_ms) du dernier message pris (passe 9j) */
    uint64_t n_injectes, n_livres, n_dispatches, n_wndproc_rendus_0;
    uint32_t decalage;                            /* passe 7u : retard cumulé des événements conditionnels */
    uint64_t n_decl[3];                           /* 7u : déclencheurs observés / partis au délai max ; 8e : sautés au délai max */
    uint64_t n_sautes;                            /* 8e : événements sautés (sauf:/si:/sinon:saute + leur bloc) */
    int bloc_saute;                               /* 8e : bloc en cours de saut (0 : aucun) */
    int textes;                                   /* 8e : le script utilise des conditions de texte ou de PV */
    uint8_t dernier_fourni;                       /* M1 : marque du dernier message RETIRÉ (lue par i_TranslateMessage) */
    uint32_t dernier_lp;                          /* M1 : lParam du dernier KEYDOWN de la plate-forme (WM_CHAR) */
    char script[512];
} I = { .mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP, .x = 512, .y = 384 };   /* récursif : appliquer() → sc2_input_*() */

/* ---- imports d'origine (sc2_iat_p4.c), pour les messages non-entrée ------------- */
static const imp_def *origine(const char *nom)
{
    I.contourne = 1;
    const imp_def *d = sc2_iat_p4_find(nom);
    I.contourne = 0;
    return d;
}
static uint32_t appel_origine(const char *nom, CPU *c, uint32_t *args)
{
    const imp_def *d = origine(nom);
    return d && d->fn ? d->fn(c, args) : 0;
}

/* ---- état ------------------------------------------------------------------------- */
static void poster_f(uint32_t msg, uint32_t wp, uint32_t lp, int fourni)   /* M1 : fourni = message marqué */
{
    if (I.flen >= 256) return;
    msg_t *m = &I.file[(I.fdeb + I.flen++) % 256];
    m->hwnd = 0; m->msg = msg; m->wp = wp; m->lp = lp; m->x = I.x; m->y = I.y; m->fourni = (uint8_t)(fourni != 0);
    I.n_injectes++;
}
static void poster(uint32_t msg, uint32_t wp, uint32_t lp) { poster_f(msg, wp, lp, 0); }
static uint32_t lp_souris(void) { return ((uint32_t)(uint16_t)I.y << 16) | (uint16_t)I.x; }
/* Passe 8h (clavier Bluetooth) : code de balayage « set 1 » d'un VK (clavier US 101/102 touches), bit 8 = touche
 * étendue (bit 24 du lParam). Ce que TranslateMessage / le jeu lisent dans lParam : bits 16–23 scan, 24 étendue, 29 Alt
 * (WM_SYSKEY*), 30 état précédent (répétition), 31 relâchée. 0 = inconnu (lParam d'avant : scan 0). */
static uint32_t scan_de(uint32_t vk)
{
    static const uint8_t lettres[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                         0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    static const uint16_t pave[10] = { 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49 };
    static const struct { uint8_t vk; uint16_t sc; } t[] = {
        { 0x08, 0x0E }, { 0x09, 0x0F }, { 0x0D, 0x1C }, { 0x10, 0x2A }, { 0x11, 0x1D }, { 0x12, 0x38 }, { 0x13, 0x45 },
        { 0x14, 0x3A }, { 0x1B, 0x01 }, { 0x20, 0x39 }, { 0x21, 0x149 }, { 0x22, 0x151 }, { 0x23, 0x14F }, { 0x24, 0x147 },
        { 0x25, 0x14B }, { 0x26, 0x148 }, { 0x27, 0x14D }, { 0x28, 0x150 }, { 0x2C, 0x137 }, { 0x2D, 0x152 }, { 0x2E, 0x153 },
        { 0x5B, 0x15B }, { 0x5C, 0x15C }, { 0x5D, 0x15D }, { 0x6A, 0x37 }, { 0x6B, 0x4E }, { 0x6D, 0x4A }, { 0x6E, 0x53 },
        { 0x6F, 0x135 }, { 0x90, 0x45 }, { 0x91, 0x46 }, { 0xA0, 0x2A }, { 0xA1, 0x36 }, { 0xA2, 0x1D }, { 0xA3, 0x11D },
        { 0xA4, 0x38 }, { 0xA5, 0x138 }, { 0xBA, 0x27 }, { 0xBB, 0x0D }, { 0xBC, 0x33 }, { 0xBD, 0x0C }, { 0xBE, 0x34 },
        { 0xBF, 0x35 }, { 0xC0, 0x29 }, { 0xDB, 0x1A }, { 0xDC, 0x2B }, { 0xDD, 0x1B }, { 0xDE, 0x28 }, { 0xE2, 0x56 } };
    if (vk >= 0x41 && vk <= 0x5A) return lettres[vk - 0x41];
    if (vk >= 0x31 && vk <= 0x39) return 0x02 + (vk - 0x31);
    if (vk == 0x30) return 0x0B;
    if (vk >= 0x60 && vk <= 0x69) return pave[vk - 0x60];
    if (vk >= 0x70 && vk <= 0x79) return 0x3B + (vk - 0x70);         /* F1–F10 */
    if (vk == 0x7A || vk == 0x7B) return 0x57 + (vk - 0x7A);          /* F11, F12 */
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) if (t[k].vk == vk) return t[k].sc;
    return 0;
}
static void touche(uint32_t vk, int bas, int fourni)       /* sous I.mu ; M1 : fourni = sc2_input_touche_plateforme */
{
    vk &= 0xFF;
    uint32_t msg_vk = vk, sc = scan_de(vk);
    int prec = I.bas[vk];                                /* passe 8h : répétition = enfoncée alors qu'elle l'était */
    if (bas && !I.bas[vk]) I.bascule[vk] ^= 1;
    I.bas[vk] = bas ? 1 : 0;
    if (vk >= 0xA0 && vk <= 0xA5) {                      /* 8h : modificateur gauche/droit → VK générique (comme Win32) */
        uint32_t g = vk & ~1u, gen = vk <= 0xA1 ? 0x10 : vk <= 0xA3 ? 0x11 : 0x12;
        prec = I.bas[gen];
        if (bas && !I.bas[gen]) I.bascule[gen] ^= 1;
        I.bas[gen] = I.bas[g] || I.bas[g + 1];
        msg_vk = gen;
    } else if (vk >= 0x10 && vk <= 0x12) {               /* générique injecté seul (scripts d'avant) : la variante gauche suit */
        I.bas[vk == 0x10 ? 0xA0 : vk == 0x11 ? 0xA2 : 0xA4] = bas ? 1 : 0;
    }
    int alt = I.bas[0x12] || msg_vk == 0x79;             /* Alt maintenue (ou F10) → WM_SYSKEY*, bit 29 */
    uint32_t lp = 1u | (sc & 0xFF) << 16 | (sc & 0x100 ? 1u << 24 : 0) | (alt && I.bas[0x12] ? 1u << 29 : 0)
                     | (bas ? (prec ? 1u << 30 : 0) : 0xC0000000u);
    poster_f(alt ? (bas ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (bas ? WM_KEYDOWN : WM_KEYUP), msg_vk, lp, fourni);
    if (fourni && bas) I.dernier_lp = lp;                /* M1 : lParam des WM_CHAR de sc2_input_caractere */
}
void sc2_input_touche(uint32_t vk, int bas)
{
    pthread_mutex_lock(&I.mu);
    touche(vk, bas, 0);
    pthread_mutex_unlock(&I.mu);
}
/* M1 (V3) : même message que sc2_input_touche, MARQUÉ : i_TranslateMessage n'en synthétise aucun WM_CHAR. */
void sc2_input_touche_plateforme(uint32_t vk, int bas)
{
    pthread_mutex_lock(&I.mu);
    touche(vk, bas, 1);
    pthread_mutex_unlock(&I.mu);
}
/* M1 (V3) : caractère fourni par la plate-forme (UTF-32) → WM_CHAR, ou WM_SYSCHAR si VK_MENU est tenue (le KEYDOWN était
 * alors un WM_SYSKEYDOWN) ; hors BMP, deux messages (paire de substitution UTF-16) ; une unité UTF-16 isolée (0xD800-
 * 0xDFFF, NSString vu unité par unité) passe telle quelle ; 0 et > 0x10FFFF ignorés. lParam = celui du dernier KEYDOWN
 * de la plate-forme (1 avant le premier). Appelé juste après sc2_input_touche_plateforme(vk, 1) : ordre Win32 KEYDOWN,
 * CHAR, KEYUP. */
void sc2_input_caractere(uint32_t cp)
{
    if (!cp || cp > 0x10FFFFu) return;
    pthread_mutex_lock(&I.mu);
    uint32_t m = I.bas[0x12] ? WM_SYSCHAR : WM_CHAR, lp = I.dernier_lp ? I.dernier_lp : 1u;
    if (cp > 0xFFFFu) {
        cp -= 0x10000u;
        poster(m, 0xD800u | (cp >> 10), lp);
        poster(m, 0xDC00u | (cp & 0x3FFu), lp);
    } else poster(m, cp, lp);
    pthread_mutex_unlock(&I.mu);
}
/* Passe 8h : molette. delta en unités Win32 (± 120 par cran ; > 0 = vers l'avant / la droite), position = curseur courant
 * (lParam en coordonnées ÉCRAN = client ici, fenêtre en (0, 0)) ; wParam = delta << 16 | boutons MK_*. */
void sc2_input_molette(int32_t delta, int horizontale)
{
    pthread_mutex_lock(&I.mu);
    poster(horizontale ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL, ((uint32_t)(uint16_t)(int16_t)delta << 16) | (uint32_t)I.boutons, lp_souris());
    pthread_mutex_unlock(&I.mu);
}
void sc2_input_souris(int32_t x, int32_t y)
{
    pthread_mutex_lock(&I.mu);
    I.x = x; I.y = y;
    poster(WM_MOUSEMOVE, (uint32_t)I.boutons, lp_souris());
    pthread_mutex_unlock(&I.mu);
}
void sc2_input_bouton(int b, int bas)
{
    static const uint32_t vk[3] = { 0x01, 0x04, 0x02 }, mk[3] = { 0x01, 0x10, 0x02 };
    static const uint32_t md[3] = { WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN }, mu[3] = { WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP };
    if (b < 0 || b > 2) return;
    pthread_mutex_lock(&I.mu);
    I.bas[vk[b]] = bas ? 1 : 0;
    I.boutons = bas ? (I.boutons | (int)mk[b]) : (I.boutons & ~(int)mk[b]);
    poster(bas ? md[b] : mu[b], (uint32_t)I.boutons, lp_souris());
    pthread_mutex_unlock(&I.mu);
}
uint32_t sc2_input_async(uint32_t vk) { return I.actif && I.bas[vk & 0xFF] ? 0x8000u : 0; }
uint32_t sc2_input_etat(uint32_t vk) { return I.actif ? (I.bas[vk & 0xFF] ? 0xFF80u : 0) | I.bascule[vk & 0xFF] : 0; }
int sc2_input_actif(void) { return I.actif; }
void sc2_input_activer(void) { I.actif = 1; }

/* ---- passe 7u : déclencheurs ÉVÉNEMENTIELS (état de l'écran observé) -----------------------------------------
 * Un événement du script peut porter « quand:<déclencheur> [max:<tours>] » : arrivé à sa frame, il attend que l'état
 * soit OBSERVÉ sur le backbuffer (relu par d3dv_lire, cible 0 = backbuffer créé au CreateDevice ; une lecture tous les
 * 4 tours tant qu'un événement attend), au plus max tours (défaut 1 200), puis part ; tous les événements suivants
 * sont décalés d'autant (les paires down/up restent ensemble). Sans « quand: » : comportement d'avant, à l'identique.
 * Zones en fractions de l'écran (mesurées en 1280×720 sur les runs 7q2, cf. TRACE-PASSE7U) :
 *   chargement     barre de chargement verte (395–885, 672–696) : V > 120 et V − R > 80
 *   fin-chargement chargement vu au moins une fois, puis plus vu, écran non noir
 *   aide           bouton « Close » du panneau d'aide (880–1005, 573–597) : V > 80 et V − R > 40
 *   partie         cadre métallique gris du HUD (5–230, 515–532) : R, V, B > 50 et |R − B| < 20, ni aide ni chargement
 *   selection      carte de commande (1030–1265, 555–712) : ≥ 15 % de pixels allumés (boutons d'une unité choisie) */
enum { D_AUCUN, D_AIDE, D_PARTIE, D_SELECTION, D_CHARGEMENT, D_FIN_CHARGEMENT, D_N };
static const char *NOMS_DECL[D_N] = { "", "aide", "partie", "selection", "chargement", "fin-chargement" };
static int declencheur_de(const char *n) { for (int k = 1; k < D_N; k++) if (!strcmp(n, NOMS_DECL[k])) return k; return 0; }
static uint32_t g_ew, g_eh;
static uint8_t *g_ecran;
static int g_vu_chargement;
static void ecran_dims(void)
{
    if (g_ew) return;
    g_ew = 1280; g_eh = 720;                     /* même règle que sc2_d3d9.c (ecran_init) */
    const char *e = getenv("SC2_ECRAN"); unsigned w, h;
    if (e && sscanf(e, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 8192 && h <= 8192) { g_ew = w; g_eh = h; }
}
/* moyenne R, V, B et % de pixels allumés (R+V+B > 60) d'une zone donnée en coordonnées 1280×720 */
static void zone(int x0, int y0, int x1, int y1, int m[4])
{
    double sx = g_ew / 1280.0, sy = g_eh / 720.0; long s[3] = { 0 }, n = 0, nn = 0;
    int X0 = (int)(x0 * sx), X1 = (int)(x1 * sx), Y0 = (int)(y0 * sy), Y1 = (int)(y1 * sy);
    for (int y = Y0; y < Y1 && y < (int)g_eh; y += 2)
        for (int x = X0; x < X1 && x < (int)g_ew; x += 2) {
            const uint8_t *p = g_ecran + 4 * ((size_t)y * g_ew + (size_t)x);
            s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; n++; if (p[0] + p[1] + p[2] > 60) nn++;
        }
    for (int k = 0; k < 3; k++) m[k] = n ? (int)(s[k] / n) : 0;
    m[3] = n ? (int)(100 * nn / n) : 0;
}
static int observer(int quel, uint32_t frame)
{
    if (!d3dv_actif()) return 0;
    ecran_dims();
    if (!g_ecran) g_ecran = malloc((size_t)g_ew * g_eh * 4);
    if (!g_ecran || d3dv_lire(0, g_ecran)) return 0;
    int b[4], c[4], h[4], cc[4];
    zone(395, 672, 885, 696, b); zone(880, 573, 1005, 597, c); zone(5, 515, 230, 532, h); zone(1030, 555, 1265, 712, cc);
    int chargement = b[1] > 120 && b[1] - b[0] > 80, aide = c[1] > 80 && c[1] - c[0] > 40;
    int hud = h[0] > 50 && h[1] > 50 && h[2] > 50 && abs(h[0] - h[2]) < 20;
    if (chargement) g_vu_chargement = 1;
    int r = quel == D_CHARGEMENT ? chargement : quel == D_FIN_CHARGEMENT ? g_vu_chargement && !chargement && h[3] > 20
          : quel == D_AIDE ? aide : quel == D_PARTIE ? hud && !aide && !chargement : quel == D_SELECTION ? cc[3] >= 15 : 0;
    static uint32_t n_obs;
    if (++n_obs <= 4 || n_obs % 50 == 0 || r)
        sc2_log("[INPUT] observation n°%u frame %u (« %s ») : barre (%d,%d,%d) close (%d,%d,%d) hud (%d,%d,%d %d%%) carte %d%% → %s",
                n_obs, frame, NOMS_DECL[quel], b[0], b[1], b[2], c[0], c[1], c[2], h[0], h[1], h[2], h[3], cc[3], r ? "VRAI" : "faux");
    return r;
}

/* ---- passe 8e : conditions sur le TEXTE et les PV observés (boucle fermée du pilote) -----------------------
 * Source : les lignes « [TEXTE] n°… BreakIterator::setText (… car.) « <texte> » » que sc2_d3d9.c journalise par
 * sc2_log sur la sortie standard. Ce module (seul fichier runtime de la passe) RELIT cette sortie par
 * /proc/self/fd/1 (descripteur à part, son propre curseur) une fois par tour de pompe, et seulement si le script
 * utilise ces conditions. Si la sortie n'est pas un fichier ordinaire (tube, logcat Android), aucun texte n'est lu :
 * quand: part au délai max (ou saute avec sinon:saute), sauf: ne saute jamais, si: saute toujours (le prudent).
 * Jetons (en plus de quand:<déclencheur 7u> et max:<tours>) :
 *   quand:texte=<motif>          attendre une NOUVELLE ligne [TEXTE] contenant <motif>, lue après le début de l'attente
 *   quand:pv=<unité|max>:<seuil> attendre un NOUVEAU relevé de PV « n / max » avec n ≤ seuil (raynor = 200)
 *   sauf:texte=<motif>           sauter l'événement si <motif> a été lu depuis le dernier « repere »
 *   si:texte=<motif>             le sauter si <motif> n'a PAS été lu depuis le dernier « repere »
 *   sauf:pv=<unité|max>:<seuil>  le sauter si le DERNIER relevé « n / max » a n ≤ seuil ; si:pv= : sauter sinon
 *   sinon:saute                  avec quand: — délai max sans observation → l'événement est SAUTÉ, pas envoyé
 *   bloc:<n>                     un événement sauté entraîne les suivants consécutifs du même bloc (A + clic ensemble)
 *   repere                       à l'application de l'événement, pose le repère des sauf:/si:texte
 * Dans <motif> : « _ » vaut une espace, « | » sépare des variantes (une suffit). */
enum { NMOTIF = 64, NPV = 8 };
static char *g_motif[NMOTIF]; static int g_nmotif;
static uint64_t g_motif_vu[NMOTIF];               /* n° du dernier texte lu qui contient le motif (0 : jamais) */
static int g_pv_max[NPV], g_pv_val[NPV], g_npv; static uint64_t g_pv_seq[NPV];
static uint64_t g_seq, g_repere;                  /* n° de texte lu ; repère des sauf:/si:texte */
static int g_fd = -2; static char g_ligne[2048]; static size_t g_llen;
static int motif_de(const char *m)
{
    char t[128]; size_t k = 0;
    for (; m[k] && k < sizeof t - 1; k++) t[k] = m[k] == '_' ? ' ' : m[k];
    t[k] = 0;
    for (int i = 0; i < g_nmotif; i++) if (!strcmp(g_motif[i], t)) return i + 1;
    if (g_nmotif >= NMOTIF) return 0;
    g_motif[g_nmotif++] = strdup(t);
    return g_nmotif;
}
static int contient(const char *t, const char *motif)       /* variantes séparées par « | » */
{
    for (const char *p = motif; *p; ) {
        const char *b = strchr(p, '|'); size_t n = b ? (size_t)(b - p) : strlen(p);
        char v[128]; if (n >= sizeof v) n = sizeof v - 1;
        memcpy(v, p, n); v[n] = 0;
        if (n && strstr(t, v)) return 1;
        if (!b) break;
        p = b + 1;
    }
    return 0;
}
static int pv_de(const char *u)                     /* « raynor:… » ou « 200:… » → PV max de l'unité */
{ return !strncasecmp(u, "raynor:", 7) ? 200 : atoi(u); }
static int pv_idx(int max)
{
    for (int i = 0; i < g_npv; i++) if (g_pv_max[i] == max) return i;
    if (g_npv >= NPV) return -1;
    g_pv_max[g_npv] = max; return g_npv++;
}
static void texte_lu(const char *l)
{
    if (strncmp(l, "[TEXTE] n°", 11)) return;
    const char *a = strstr(l, " car.) « ");      /* format de sc2_d3d9.c : « … (n car.) « texte »  ← 0x… » */
    if (!a) return;
    a += strlen(" car.) « ");
    const char *b = strstr(a, " »  ← ");
    if (!b) return;
    char t[512]; size_t n = (size_t)(b - a) < sizeof t - 1 ? (size_t)(b - a) : sizeof t - 1;
    memcpy(t, a, n); t[n] = 0;
    g_seq++;
    for (int i = 0; i < g_nmotif; i++) if (contient(t, g_motif[i])) g_motif_vu[i] = g_seq;
    int v, m;
    if (n < 20 && sscanf(t, "%d / %d", &v, &m) == 2 && m > 0) {
        int i = pv_idx(m);
        if (i >= 0) { g_pv_val[i] = v; g_pv_seq[i] = g_seq; }
    }
}
static void lire_textes(void)
{
    if (g_fd == -2) {
        struct stat st;
        g_fd = open("/proc/self/fd/1", O_RDONLY);
        if (g_fd >= 0 && (fstat(g_fd, &st) || !S_ISREG(st.st_mode))) { close(g_fd); g_fd = -1; }
        sc2_log("[INPUT] passe 8e : textes observés %s", g_fd >= 0 ? "relus sur la sortie standard (fichier ordinaire)"
                : "INDISPONIBLES (sortie standard non relisible) — quand:texte/pv partent au délai max, si: saute");
    }
    if (g_fd < 0) return;
    char b[65536]; ssize_t n;
    while ((n = read(g_fd, b, sizeof b)) > 0)
        for (ssize_t k = 0; k < n; k++) {
            if (b[k] == '\n') { g_ligne[g_llen] = 0; texte_lu(g_ligne); g_llen = 0; }
            else if (g_llen < sizeof g_ligne - 1) g_ligne[g_llen++] = b[k];
        }
}
static int pv_bas(int max, int seuil, uint64_t apres)      /* dernier relevé (lu après « apres ») ≤ seuil */
{
    int i = pv_idx(max);
    return i >= 0 && g_pv_seq[i] && g_pv_seq[i] > apres && g_pv_val[i] <= seuil;
}
static void decrire(const evt_t *e, char *d, size_t n)
{
    if (e->q_texte) snprintf(d, n, "texte=%s", g_motif[e->q_texte - 1]);
    else if (e->q_pv) snprintf(d, n, "pv=%d:%d", e->q_pv, e->q_seuil);
    else snprintf(d, n, "%s", NOMS_DECL[e->quand]);
}

/* ---- script SC2_INPUT_SIM ---------------------------------------------------------- */
static void charger_script(const char *chemin)
{
    FILE *f = fopen(chemin, "r");
    if (!f) { sc2_log("[INPUT] SC2_INPUT_SIM=%s : fichier illisible — entrées neutres", chemin); return; }
    char l[512]; int ln = 0, ko = 0;
    while (fgets(l, sizeof l, f)) {
        ln++;
        char *h = strchr(l, '#'); if (h) *h = 0;
        int quand = 0; uint32_t max = 0;                 /* passe 7u : jetons optionnels, retirés avant l'analyse */
        evt_t o = { 0 };                                 /* passe 8e : conditions de texte / PV, bloc, repère */
        char base[512] = "";
        for (char *tok = strtok(l, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
            if (!strncmp(tok, "quand:", 6) || !strncmp(tok, "sauf:", 5) || !strncmp(tok, "si:", 3)) {
                int sens = tok[1] == 'u' ? 0 : tok[1] == 'a' ? 1 : 2;   /* quand / sauf / si */
                char *v = strchr(tok, ':') + 1;
                if (!strncmp(v, "texte=", 6)) {
                    int m = motif_de(v + 6);
                    if (sens == 0) o.q_texte = m; else { o.c_sens = sens; o.c_texte = m; }
                    I.textes = 1;
                } else if (!strncmp(v, "pv=", 3) && strchr(v + 3, ':')) {
                    int mx = pv_de(v + 3), se = atoi(strchr(v + 3, ':') + 1);
                    if (sens == 0) { o.q_pv = mx; o.q_seuil = se; } else { o.c_sens = sens; o.c_pv = mx; o.c_seuil = se; }
                    I.textes = 1;
                } else if (sens == 0) {
                    quand = declencheur_de(v);
                    if (!quand) { sc2_log("[INPUT] %s:%d : déclencheur « %s » inconnu (aide, partie, selection, chargement, fin-chargement, texte=, pv=) — ignoré", chemin, ln, v); }
                } else sc2_log("[INPUT] %s:%d : condition « %s » inconnue (texte=, pv=) — ignorée", chemin, ln, tok);
            } else if (!strncmp(tok, "max:", 4)) max = (uint32_t)strtoul(tok + 4, NULL, 10);
            else if (!strcmp(tok, "sinon:saute")) o.saute = 1;
            else if (!strncmp(tok, "bloc:", 5)) o.bloc = atoi(tok + 5);
            else if (!strcmp(tok, "repere")) o.repere = 1;
            else { if (*base) strcat(base, " "); if (strlen(base) + strlen(tok) < sizeof base - 2) strcat(base, tok); }
        }
        strcpy(l, base);
        unsigned fr; char genre[16], a1[32] = "", a2[32] = "", a3[32] = "", a4[32] = "", a5[32] = "";
        int n = sscanf(l, "frame %u %15s %31s %31s %31s %31s %31s", &fr, genre, a1, a2, a3, a4, a5);
        if (n <= 0) continue;
        evt_t e = { .frame = fr };
        if (n >= 4 && !strcmp(genre, "touche") && (!strcmp(a2, "down") || !strcmp(a2, "up"))) {
            e.genre = 0; e.vk = (uint32_t)strtoul(a1, NULL, 0); e.bas = !strcmp(a2, "down");
        } else if (n >= 4 && !strcmp(genre, "souris")) {
            e.genre = 1; e.x = atoi(a1); e.y = atoi(a2); e.deplace = 1; e.bouton = -1;
            if (n >= 7 && !strcmp(a3, "bouton") && strchr("gmd", a4[0]) && a4[1] == 0 && (!strcmp(a5, "down") || !strcmp(a5, "up"))) {
                e.bouton = a4[0] == 'g' ? 0 : a4[0] == 'm' ? 1 : 2; e.bas = !strcmp(a5, "down");
            } else if (n != 4) { ko++; sc2_log("[INPUT] %s:%d : ligne « souris » invalide, ignorée", chemin, ln); continue; }
        } else if (n >= 3 && !strcmp(genre, "molette")) {   /* passe 8h : « frame n molette <delta> [x y] » (±120 = 1 cran) */
            e.genre = 2; e.vk = (uint32_t)(int32_t)strtol(a1, NULL, 0); e.bouton = -1;
            if (n >= 5) { e.x = atoi(a2); e.y = atoi(a3); e.deplace = 1; }
        } else { ko++; sc2_log("[INPUT] %s:%d : ligne invalide, ignorée", chemin, ln); continue; }
        e.q_texte = o.q_texte; e.q_pv = o.q_pv; e.q_seuil = o.q_seuil;
        e.c_sens = o.c_sens; e.c_texte = o.c_texte; e.c_pv = o.c_pv; e.c_seuil = o.c_seuil;
        e.saute = o.saute; e.bloc = o.bloc; e.repere = o.repere;
        int attend = quand || e.q_texte || e.q_pv;
        e.quand = quand; e.max = attend ? (max ? max : 1200u) : 0;   /* délai max par défaut : 1 200 tours */
        I.ev = realloc(I.ev, sizeof *I.ev * (size_t)(I.nev + 1));
        I.ev[I.nev++] = e;
    }
    fclose(f);
    for (int i = 1; i < I.nev; i++) for (int j = i; j > 0 && I.ev[j].frame < I.ev[j - 1].frame; j--) { evt_t t = I.ev[j]; I.ev[j] = I.ev[j - 1]; I.ev[j - 1] = t; }
    sc2_log("[INPUT] SC2_INPUT_SIM=%s : %d événement(s) chargé(s), %d ligne(s) rejetée(s) ; horloge = tours de la pompe (0x%08X)",
            chemin, I.nev, ko, POMPE_VA);
}
static void appliquer(uint32_t frame)
{
    if (I.textes) lire_textes();                         /* passe 8e : textes et PV journalisés depuis le tour d'avant */
    while (I.iev < I.nev && I.ev[I.iev].frame + I.decalage <= frame) {
        evt_t *e = &I.ev[I.iev];
        if (I.bloc_saute && e->bloc == I.bloc_saute) {   /* 8e : la suite d'un bloc sauté est sautée avec lui */
            I.iev++; I.n_sautes++; continue;
        }
        I.bloc_saute = 0;
        if (e->c_sens) {                                 /* 8e : sauf: / si: évalués à l'heure de l'événement */
            int v = e->c_texte ? g_motif_vu[e->c_texte - 1] > g_repere : pv_bas(e->c_pv, e->c_seuil, 0);
            if (e->c_sens == 1 ? v : !v) {
                char d[160];
                if (e->c_texte) snprintf(d, sizeof d, "texte=%s", g_motif[e->c_texte - 1]);
                else snprintf(d, sizeof d, "pv=%d:%d", e->c_pv, e->c_seuil);
                sc2_log("[INPUT] frame %u : %s:%s %s → événement SAUTÉ%s (prévu %u)", frame, e->c_sens == 1 ? "sauf" : "si", d,
                        v ? "vrai" : "faux", e->bloc ? " avec son bloc" : "", e->frame);
                I.bloc_saute = e->bloc; I.iev++; I.n_sautes++; continue;
            }
        }
        if (e->quand || e->q_texte || e->q_pv) {         /* passe 7u : attendre l'état observé (ou le délai max) */
            uint32_t du = e->frame + I.decalage, attente = frame - du;
            if (!e->debut) { e->debut = frame ? frame : 1; e->seq0 = g_seq; }
            int vrai = 0;
            if (e->q_texte) vrai = g_motif_vu[e->q_texte - 1] > e->seq0;          /* 8e : nouveau texte */
            else if (e->q_pv) vrai = pv_bas(e->q_pv, e->q_seuil, e->seq0);         /* 8e : nouveau relevé bas */
            else if (attente % 4 == 0 || attente >= e->max) vrai = observer(e->quand, frame);
            if (!vrai && attente < e->max) return;       /* on réessaie au prochain tour ; rien d'autre ne passe avant */
            char d[160]; decrire(e, d, sizeof d);
            int saut = !vrai && e->saute;
            sc2_log("[INPUT] frame %u : déclencheur « %s » %s après %u tour(s) d'attente (prévu %u) — la suite est décalée de %u",
                    frame, d, vrai ? "OBSERVÉ" : saut ? "NON observé : délai max atteint, événement SAUTÉ (sinon:saute)"
                    : "NON observé : délai max atteint, l'événement part quand même", attente, e->frame, I.decalage + attente);
            I.decalage += attente;
            I.n_decl[vrai ? 0 : saut ? 2 : 1]++;
            if (saut) { I.bloc_saute = e->bloc; I.iev++; I.n_sautes++; continue; }
        }
        if (e->repere) { g_repere = g_seq; sc2_log("[INPUT] frame %u : repère posé (texte lu n°%llu)", frame, (unsigned long long)g_seq); }
        I.iev++;
        if (e->genre == 0) {
            sc2_log("[INPUT] frame %u : touche VK 0x%02X %s (injectée)", frame, e->vk, e->bas ? "enfoncée" : "relâchée");
            sc2_input_touche(e->vk, e->bas);
        } else if (e->genre == 2) {
            if (e->deplace) sc2_input_souris(e->x, e->y);
            sc2_input_molette((int32_t)e->vk, 0);
            sc2_log("[INPUT] frame %u : molette %+d en (%d, %d) (injectée)", frame, (int32_t)e->vk, I.x, I.y);
        } else {
            if (e->deplace) sc2_input_souris(e->x, e->y);
            if (e->bouton >= 0) sc2_input_bouton(e->bouton, e->bas);
            sc2_log("[INPUT] frame %u : souris (%d, %d)%s%s%s (injectée)", frame, e->x, e->y,
                    e->bouton >= 0 ? " bouton " : "", e->bouton < 0 ? "" : e->bouton == 0 ? "gauche" : e->bouton == 1 ? "milieu" : "droit",
                    e->bouton < 0 ? "" : e->bas ? " enfoncé" : " relâché");
        }
    }
}

/* ---- imports --------------------------------------------------------------------------- */
static uint32_t fenetre(CPU *c)
{
    uint32_t args[1] = { 0 };
    return appel_origine("GetActiveWindow", c, args);
}
static int prendre(CPU *c, uint32_t pmsg, uint32_t min, uint32_t max, int retirer)
{
    for (int k = 0; k < I.flen; k++) {
        msg_t *m = &I.file[(I.fdeb + k) % 256];
        if ((min || max) && (m->msg < min || m->msg > max)) continue;
        if (!m->hwnd) m->hwnd = fenetre(c);
        wr32(pmsg + 0, m->hwnd); wr32(pmsg + 4, m->msg); wr32(pmsg + 8, m->wp); wr32(pmsg + 12, m->lp);
        I.temps = sc2_tick_ms();                         /* passe 9j : MSG.time en ms (était un compteur 1, 2, 3…) */
        wr32(pmsg + 16, I.temps); wr32(pmsg + 20, (uint32_t)m->x); wr32(pmsg + 24, (uint32_t)m->y);
        if (retirer) {                                   /* retrait (k = 0 au boot : file en ordre) */
            I.dernier_fourni = m->fourni;                /* M1 : lu par le TranslateMessage qui suit */
            for (int j = k; j > 0; j--) I.file[(I.fdeb + j) % 256] = I.file[(I.fdeb + j - 1) % 256];
            I.fdeb = (I.fdeb + 1) % 256; I.flen--;
            I.n_livres++;
        }
        return 1;
    }
    return 0;
}
/* 21/10 : ACTIVATION de l'application. Aucun WM_ACTIVATEAPP n'était jamais livré : le jeu se croyait en arrière-plan,
 * drapeau [0x0169A62C] à 0 (écrit par le gestionnaire d'activation L_00804490), et sa boucle principale (L_00807F20)
 * faisait « if (![0x0169A62C]) Sleep(10) » à CHAQUE tour : ≈ 10 ms d'attente par image (53 % du fil du jeu au banc).
 * Tant que le drapeau vaut 0, on poste WM_ACTIVATEAPP(TRUE) + WM_ACTIVATE(WA_ACTIVE) à la wndproc, au plus une fois
 * tous les 60 tours de pompe. SC2_ACTIVATION=0 : rien (comportement d'avant). */
#define ACTIF_VA 0x0169A62Cu
/* M1 (V3) : l'état de l'auto-activation (ex-statiques de activation()) est partagé avec sc2_input_activation. hote_inactif :
 * la plate-forme a perdu le focus → activation() ne reposte rien ; n_hote : envois de l'hôte (WM_ACTIVATEAPP(1) posté par
 * sc2_input_activation(1) : l'auto-activation attend ensuite 60 tours comme après l'un des siens, sans doublon). */
static struct { int mode, vu_actif, hote_inactif; uint32_t dernier; unsigned n, n_hote; } ACT = { .mode = -1 };
static int activation_mode(void)
{
    if (ACT.mode < 0) { const char *e = getenv("SC2_ACTIVATION"); ACT.mode = !(e && *e == '0'); }
    return ACT.mode;
}
static void activation(void)
{
    if (!activation_mode()) return;
    if (ACT.hote_inactif) return;                        /* M1 : perte du focus annoncée par l'hôte : rien n'est reposté */
    if (rd32(ACTIF_VA)) {
        if (!ACT.vu_actif) { ACT.vu_actif = 1; sc2_log("[INPUT] activation : jeu ACTIF au tour %u (%u envoi(s))", I.frame, ACT.n); }
        return;
    }
    if ((ACT.n || ACT.n_hote) && I.frame - ACT.dernier < 60) return;
    ACT.dernier = I.frame; ACT.n++;
    poster(0x1C /* WM_ACTIVATEAPP */, 1, 0); poster(0x06 /* WM_ACTIVATE */, 1 /* WA_ACTIVE */, 0);
    if (ACT.n <= 3 || !(ACT.n & (ACT.n - 1)))
        sc2_log("[INPUT] activation : WM_ACTIVATEAPP + WM_ACTIVATE postés (tour %u, envoi n°%u) — [0x%08X] = 0", I.frame, ACT.n,
                ACTIF_VA);
}
/* M1 (V3) : focus de la plate-forme, tous les fils. 0 (perte) : WM_ACTIVATEAPP(0) + WM_ACTIVATE(WA_INACTIVE) postés tout
 * de suite, auto-activation suspendue ; 1 (retour) : WM_ACTIVATEAPP(1) + WM_ACTIVATE(WA_ACTIVE) postés tout de suite (livrés
 * au tour de pompe suivant), auto-activation reprise. Rien n'est posté si l'état ne change pas, si le module est inactif
 * (état mémorisé : il vaudra à l'activation du module) ou sous SC2_ACTIVATION=0 (aucune activation, comme avant). */
void sc2_input_activation(int actif)
{
    pthread_mutex_lock(&I.mu);
    actif = actif != 0;
    int avant = !ACT.hote_inactif;
    ACT.hote_inactif = !actif;
    const char *pourquoi = actif == avant ? "inchangé, rien de posté"
                         : !I.actif ? "module d'entrées inactif : état mémorisé, rien de posté"
                         : !activation_mode() ? "SC2_ACTIVATION=0 : rien de posté" : NULL;
    if (!pourquoi) {
        poster(0x1C /* WM_ACTIVATEAPP */, (uint32_t)actif, 0);
        poster(0x06 /* WM_ACTIVATE */, (uint32_t)actif /* WA_ACTIVE : WA_INACTIVE */, 0);
        if (actif) { ACT.dernier = I.frame; ACT.n_hote++; }
        else ACT.vu_actif = 0;                           /* le retour à l'état actif sera de nouveau journalisé */
    }
    sc2_log("[ACTIVATION] hôte : %s (tour %u)%s%s", actif ? "actif" : "inactif", I.frame,
            pourquoi ? " — " : " — WM_ACTIVATEAPP + WM_ACTIVATE postés", pourquoi ? pourquoi : "");
    pthread_mutex_unlock(&I.mu);
}
/* M1 (V3) : relâche tout (perte du focus) : KEYUP (SYSKEYUP si Alt tenue) de chaque touche tenue, dans l'ordre des VK,
 * boutons de souris exclus (1, 2, 4) ; un VK générique 0x10-0x12 l'est par ses variantes gauche/droite tenues (un seul
 * message par touche physique), seul s'il n'en a aucune ; puis les boutons tenus (gauche, milieu, droit). L'état bascule
 * (Verr. maj) est intact : GetAsyncKeyState, relu par le jeu à l'activation, trouve tout relâché. */
void sc2_input_tout_relacher(void)
{
    static const uint32_t vk_bouton[3] = { 0x01, 0x04, 0x02 };
    pthread_mutex_lock(&I.mu);
    int nt = 0, nb = 0;
    for (uint32_t vk = 0; vk < 256; vk++) {
        if (!I.bas[vk] || vk == 0x01 || vk == 0x02 || vk == 0x04) continue;
        if (vk >= 0x10 && vk <= 0x12) {
            uint32_t g = vk == 0x10 ? 0xA0 : vk == 0x11 ? 0xA2 : 0xA4;
            if (I.bas[g] || I.bas[g + 1]) continue;
        }
        touche(vk, 0, 0); nt++;
    }
    for (int b = 0; b < 3; b++) if (I.bas[vk_bouton[b]]) { sc2_input_bouton(b, 0); nb++; }
    sc2_log("[INPUT] hôte : tout relâché (tour %u) — %d touche(s), %d bouton(s)", I.frame, nt, nb);
    pthread_mutex_unlock(&I.mu);
}
static uint32_t i_PeekMessageW(CPU *c, const uint32_t *a)        /* (msg*, hwnd, min, max, retrait) */
{
    if (rd32(c->esp) == POMPE_VA) sc2_banc_entrees_tour();   /* passe 10i : hors verrou (sc2_input_* le prennent) */
    pthread_mutex_lock(&I.mu);
    if (rd32(c->esp) == POMPE_VA) { appliquer(++I.frame); activation(); sc2_glue_pompe(c); }
    int r = prendre(c, A(0), A(2), A(3), A(4) & 1);
    pthread_mutex_unlock(&I.mu);
    return (uint32_t)r;                                  /* file vide : 0, comme avant */
}
static uint32_t i_GetMessageW(CPU *c, const uint32_t *a)         /* (msg*, hwnd, min, max) */
{
    pthread_mutex_lock(&I.mu);
    int r = prendre(c, A(0), A(2), A(3), 1);
    if (!r) {                                            /* la pompe ne l'appelle qu'après un Peek non vide */
        memset(GPTR(A(0)), 0, 28); wr32(A(0), fenetre(c)); wr32(A(0) + 4, WM_NULL);
        I.dernier_fourni = 0;                            /* M1 : WM_NULL n'est pas marqué */
    }
    pthread_mutex_unlock(&I.mu);
    return 1;                                            /* jamais WM_QUIT */
}
static uint32_t i_TranslateMessage(CPU *c, const uint32_t *a)    /* WM_KEYDOWN imprimable → WM_CHAR */
{
    (void)c;
    if (I.dernier_fourni) return 0;                      /* M1 : la plate-forme fournit ses caractères (sc2_input_caractere) */
    uint32_t m = rd32(A(0) + 4);
    if (m != WM_KEYDOWN && m != WM_SYSKEYDOWN) return 0;
    uint32_t vk = rd32(A(0) + 8), ch = 0;
    int maj = I.bas[0x10] || I.bascule[0x14], shift = I.bas[0x10];
    if (I.bas[0x11] && !I.bas[0x12]) {                    /* Ctrl+lettre → caractère de contrôle (Ctrl+A = 0x01), comme Win32 */
        if (vk >= 0x41 && vk <= 0x5A) ch = vk - 0x40;
    } else if (vk >= 0x41 && vk <= 0x5A) ch = maj ? vk : vk + 0x20;
    else if (vk >= 0x30 && vk <= 0x39) ch = shift ? (uint32_t)")!@#$%^&*("[vk - 0x30] : vk;   /* passe 8h : disposition US */
    else if (vk == 0x20 || vk == 0x0D || vk == 0x08 || vk == 0x1B || vk == 0x09) ch = vk;
    else if (vk >= 0x60 && vk <= 0x69) ch = '0' + (vk - 0x60);                             /* pavé numérique */
    else {
        static const struct { uint8_t vk; char n, s; } p[] = { { 0xBA, ';', ':' }, { 0xBB, '=', '+' }, { 0xBC, ',', '<' },
            { 0xBD, '-', '_' }, { 0xBE, '.', '>' }, { 0xBF, '/', '?' }, { 0xC0, '`', '~' }, { 0xDB, '[', '{' },
            { 0xDC, '\\', '|' }, { 0xDD, ']', '}' }, { 0xDE, '\'', '"' }, { 0x6A, '*', '*' }, { 0x6B, '+', '+' },
            { 0x6D, '-', '-' }, { 0x6E, '.', '.' }, { 0x6F, '/', '/' } };
        for (unsigned k = 0; k < sizeof p / sizeof p[0]; k++) if (p[k].vk == vk) ch = (uint8_t)(shift ? p[k].s : p[k].n);
    }
    if (!ch) return 0;
    pthread_mutex_lock(&I.mu);
    poster(m == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, ch, rd32(A(0) + 12));
    pthread_mutex_unlock(&I.mu);
    return 1;
}
static uint32_t i_DispatchMessageW(CPU *c, const uint32_t *a)    /* → wndproc de la fenêtre du message */
{
    uint32_t hwnd = rd32(A(0)), msg = rd32(A(0) + 4), wp = rd32(A(0) + 8), lp = rd32(A(0) + 12);
    uint32_t gw[2] = { hwnd, 0xFFFFFFFCu };              /* GWL_WNDPROC */
    uint32_t proc = appel_origine("GetWindowLongA", c, gw);   /* le jeu importe la variante A (sc2_iat_p4) */
    if (!proc || msg == WM_NULL) return 0;
    uint32_t args[4] = { hwnd, msg, wp, lp };
    uint32_t r = sc2_call_guest(c, proc, 4, args, 0);    /* WNDPROC : stdcall */
    I.n_dispatches++;
    if (!r) I.n_wndproc_rendus_0++;
    static const struct { uint32_t m; const char *n; } noms[] = { { WM_KEYDOWN, "WM_KEYDOWN" }, { WM_KEYUP, "WM_KEYUP" },
        { WM_CHAR, "WM_CHAR" }, { WM_MOUSEMOVE, "WM_MOUSEMOVE" }, { WM_LBUTTONDOWN, "WM_LBUTTONDOWN" },
        { WM_LBUTTONUP, "WM_LBUTTONUP" }, { WM_RBUTTONDOWN, "WM_RBUTTONDOWN" }, { WM_RBUTTONUP, "WM_RBUTTONUP" },
        { WM_MBUTTONDOWN, "WM_MBUTTONDOWN" }, { WM_MBUTTONUP, "WM_MBUTTONUP" }, { WM_SYSKEYDOWN, "WM_SYSKEYDOWN" },
        { WM_SYSKEYUP, "WM_SYSKEYUP" }, { WM_SYSCHAR, "WM_SYSCHAR" }, { WM_MOUSEWHEEL, "WM_MOUSEWHEEL" },
        { WM_MOUSEHWHEEL, "WM_MOUSEHWHEEL" } };
    const char *n = "?";
    for (unsigned k = 0; k < sizeof noms / sizeof noms[0]; k++) if (noms[k].m == msg) n = noms[k].n;
    sc2_log("[INPUT] frame %u : %s (0x%X, wParam 0x%X, lParam 0x%08X = (%d, %d)) → wndproc du jeu 0x%08X, rendu 0x%X",
            I.frame, n, msg, wp, lp, (int16_t)(lp & 0xFFFF), (int16_t)(lp >> 16), proc, r);
    return r;
}
static uint32_t i_GetCursorPos(CPU *c, const uint32_t *a)
{ (void)c; wr32(A(0), (uint32_t)I.x); wr32(A(0) + 4, (uint32_t)I.y); return 1; }   /* fenêtre en (0, 0) : écran = client */
static uint32_t i_GetKeyboardState(CPU *c, const uint32_t *a)
{ (void)c; for (int k = 0; k < 256; k++) wr8(A(0) + (uint32_t)k, (uint8_t)((I.bas[k] ? 0x80 : 0) | I.bascule[k])); return 1; }

/* GetMessageTime (passe 9j) : l'heure du dernier message pris par PeekMessageW / GetMessageW, comme Win32 ; avant le
 * premier message, l'heure courante. Était p_ret0 (toujours 0) : les événements clavier du moteur, horodatés par la
 * wndproc (0x0099A9A9 → [0x14197E4]), portaient tous 0. */
static uint32_t i_GetMessageTime(CPU *c, const uint32_t *a) { (void)c; (void)a; return I.temps ? I.temps : sc2_tick_ms(); }

static const imp_def g_defs_input[] = {
    { "PeekMessageW", 5, STD, i_PeekMessageW }, { "GetMessageW", 4, STD, i_GetMessageW },
    { "TranslateMessage", 1, STD, i_TranslateMessage }, { "DispatchMessageW", 1, STD, i_DispatchMessageW },
    { "GetCursorPos", 1, STD, i_GetCursorPos }, { "GetKeyboardState", 1, STD, i_GetKeyboardState },
    { "GetMessageTime", 0, STD, i_GetMessageTime },
};

static void bilan(void)
{
    printf("[BILAN] [INPUT] %u tours de pompe ; événements script %d/%d appliqués ; messages injectés %llu, livrés au jeu %llu, "
           "dispatchés à la wndproc %llu (rendu 0 : %llu)\n", I.frame, I.iev, I.nev, (unsigned long long)I.n_injectes,
           (unsigned long long)I.n_livres, (unsigned long long)I.n_dispatches, (unsigned long long)I.n_wndproc_rendus_0);
    if (I.n_decl[0] || I.n_decl[1] || I.n_decl[2])
        printf("[BILAN] [INPUT] déclencheurs (passe 7u) : %llu observés, %llu partis au délai max ; décalage total %u tour(s)\n",
               (unsigned long long)I.n_decl[0], (unsigned long long)I.n_decl[1], I.decalage);
    if (I.textes)
        printf("[BILAN] [INPUT] boucle fermée (passe 8e) : %llu textes lus (%s), %d motif(s) ; %llu attente(s) sautée(s) au délai max, "
               "%llu événement(s) sauté(s) au total\n", (unsigned long long)g_seq, g_fd >= 0 ? "sortie standard relue" : "INDISPONIBLES",
               g_nmotif, (unsigned long long)I.n_decl[2], (unsigned long long)I.n_sautes);
}

const imp_def *sc2_input_find(const char *nom)
{
    if (I.contourne) return NULL;
    if (!I.essaye) {
        I.essaye = 1;
        sc2_banc_entrees_init();                         /* passe 10i : active le module si SC2_BANC_ENTREES */
        const char *s = getenv("SC2_INPUT_SIM");
        if (s && *s) {
            snprintf(I.script, sizeof I.script, "%s", s);
            charger_script(s);
            I.actif = 1;
            atexit(bilan);
        }
    }
    if (!I.actif) return NULL;
    for (unsigned k = 0; k < sizeof g_defs_input / sizeof g_defs_input[0]; k++)
        if (!strcmp(g_defs_input[k].nom, nom)) return &g_defs_input[k];
    return NULL;
}
