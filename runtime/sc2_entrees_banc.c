/* sc2_entrees_banc.c — entrées simulées du banc, horloge en SECONDES (passe 10i).
 *
 * Complète SC2_INPUT_SIM (sc2_input_sim.c, horloge en tours de pompe) pour les runs de régression à travers les
 * écrans et dialogues : une seule variable d'environnement, sans fichier, et des instants en secondes depuis le
 * démarrage du run (l'activation du module d'entrées, au premier import résolu, ~0,1 s après le lancement).
 *
 *   SC2_BANC_ENTREES="<instant>:<action>[:<x>,<y>];…"       ou   SC2_BANC_ENTREES=@<fichier> (mêmes éléments,
 *                                                                séparés par « ; » ou par des fins de ligne, # = commentaire)
 *     <instant>  secondes (« 30 », « 12.5 ») ou tours de pompe préfixés de f (« f700 », comme SC2_INPUT_SIM)
 *     DOWN:x,y   WM_LBUTTONDOWN aux coordonnées CLIENT (x, y) — le curseur y est posé d'abord
 *     UP:x,y     WM_LBUTTONUP
 *     CLIC:x,y   DOWN puis UP au tour de pompe suivant
 *     MOVE:x,y   déplacement seul (WM_MOUSEMOVE)
 *     ESC        touche Échap (VK 0x1B) enfoncée puis relâchée au tour suivant
 *     KEY:vk     touche vk (décimal ou 0x…) enfoncée puis relâchée au tour suivant
 *     KEYDOWN:vk / KEYUP:vk   touche maintenue / relâchée (combinaisons : « 5:KEYDOWN:0x12;5.1:KEY:0x73;5.2:KEYUP:0x12 » = Alt+F4)
 *   ex. SC2_BANC_ENTREES="30:CLIC:640,360;f700:ESC;200:KEY:0x79"
 *
 * Chaque événement appliqué est journalisé « [INPUT] banc t=…s tour … : … », puis les lignes [INPUT] habituelles de
 * sc2_input_sim.c suivent la livraison à la wndproc du jeu. Bilan à la sortie : « [BILAN] [INPUT] banc … ».
 * Sans la variable : aucun effet (le module d'entrées reste inactif, bit à bit comme avant).
 *
 * Inclus par sc2_input_sim.c (branchement 10i : #include + 2 appels), donc compilé partout où il l'est, APK compris ;
 * appelé sur le fil du jeu, à chaque tour de la pompe, HORS du verrou du module (sc2_input_* le prennent).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define BANC_MAX 256
typedef struct { double t; uint32_t tour; int par_tour; int action; int32_t x, y; uint32_t vk; } banc_evt;
enum { B_DOWN, B_UP, B_CLIC, B_MOVE, B_TOUCHE, B_TBAS, B_THAUT };
static struct {
    int pret, n, i;
    banc_evt ev[BANC_MAX];
    struct timespec t0;
    uint32_t tour;
    int relacher_bouton; uint32_t relacher_vk;     /* CLIC / ESC / KEY : relâchement au tour suivant */
    int appliques, rejetes;
} B;

static double banc_secondes(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - B.t0.tv_sec) + (double)(t.tv_nsec - B.t0.tv_nsec) / 1e9;
}

static void banc_element(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    for (size_t n = strlen(s); n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'); ) s[--n] = 0;
    if (!*s || *s == '#') return;
    banc_evt e = { 0 };
    char *p = strchr(s, ':');
    if (!p || B.n >= BANC_MAX) { B.rejetes++; sc2_log("[INPUT] banc : élément rejeté « %s »", s); return; }
    *p++ = 0;
    if (*s == 'f' || *s == 'F') { e.par_tour = 1; e.tour = (uint32_t)strtoul(s + 1, NULL, 10); }
    else e.t = strtod(s, NULL);
    char *q = strchr(p, ':'); if (q) *q++ = 0;
    int xy = q && sscanf(q, "%d,%d", &e.x, &e.y) == 2;
    if (!strcasecmp(p, "DOWN") && xy) e.action = B_DOWN;
    else if (!strcasecmp(p, "UP") && xy) e.action = B_UP;
    else if (!strcasecmp(p, "CLIC") && xy) e.action = B_CLIC;
    else if (!strcasecmp(p, "MOVE") && xy) e.action = B_MOVE;
    else if (!strcasecmp(p, "ESC")) { e.action = B_TOUCHE; e.vk = 0x1B; }
    else if (!strcasecmp(p, "KEY") && q) { e.action = B_TOUCHE; e.vk = (uint32_t)strtoul(q, NULL, 0) & 0xFF; }
    else if (!strcasecmp(p, "KEYDOWN") && q) { e.action = B_TBAS; e.vk = (uint32_t)strtoul(q, NULL, 0) & 0xFF; }
    else if (!strcasecmp(p, "KEYUP") && q) { e.action = B_THAUT; e.vk = (uint32_t)strtoul(q, NULL, 0) & 0xFF; }
    else { B.rejetes++; sc2_log("[INPUT] banc : action rejetée « %s%s%s »", p, q ? ":" : "", q ? q : ""); return; }
    B.ev[B.n++] = e;
}

static void banc_bilan(void)
{
    printf("[BILAN] [INPUT] banc (SC2_BANC_ENTREES) : %d/%d événement(s) appliqué(s), %d rejeté(s), %u tours de pompe, %.1f s\n",
           B.appliques, B.n, B.rejetes, B.tour, banc_secondes());
}

/* Au premier import résolu (sc2_input_find) : lecture de la variable, activation du module d'entrées. */
static void sc2_banc_entrees_init(void)
{
    const char *v = getenv("SC2_BANC_ENTREES");
    if (!v || !*v || B.pret) return;
    B.pret = 1;
    clock_gettime(CLOCK_MONOTONIC, &B.t0);
    static char buf[16384];
    if (*v == '@') {
        FILE *f = fopen(v + 1, "r"); size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        if (!f) sc2_log("[INPUT] banc : SC2_BANC_ENTREES=%s illisible", v);
        buf[n] = 0;
    } else snprintf(buf, sizeof buf, "%s", v);
    for (char *s = buf, *fin; s && *s; s = fin) {
        fin = s + strcspn(s, ";\n");
        if (*fin) *fin++ = 0; else fin = NULL;
        char *c = strchr(s, '#'); if (c && c != s) *c = 0;      /* commentaire en fin d'élément */
        banc_element(s);
    }
    /* ordre d'application : instants croissants (les éléments « f » gardent leur rang relatif) */
    for (int a = 1; a < B.n; a++)
        for (int b = a; b > 0 && !B.ev[b].par_tour && !B.ev[b - 1].par_tour && B.ev[b].t < B.ev[b - 1].t; b--) {
            banc_evt t = B.ev[b]; B.ev[b] = B.ev[b - 1]; B.ev[b - 1] = t;
        }
    sc2_input_activer();
    atexit(banc_bilan);
    sc2_log("[INPUT] banc : SC2_BANC_ENTREES = %d événement(s), %d rejeté(s) ; horloge = secondes depuis l'activation (f<n> : tours de pompe)",
            B.n, B.rejetes);
}

/* À chaque tour de la pompe du jeu, hors verrou : relâchements en attente, puis événements échus. */
static void sc2_banc_entrees_tour(void)
{
    if (!B.pret) return;
    B.tour++;
    double t = banc_secondes();
    if (B.relacher_bouton) { sc2_input_bouton(0, 0); B.relacher_bouton = 0; sc2_log("[INPUT] banc t=%.2fs tour %u : bouton gauche relâché (fin de CLIC)", t, B.tour); }
    if (B.relacher_vk) { sc2_input_touche(B.relacher_vk, 0); sc2_log("[INPUT] banc t=%.2fs tour %u : VK 0x%02X relâchée", t, B.tour, B.relacher_vk); B.relacher_vk = 0; }
    while (B.i < B.n) {
        banc_evt *e = &B.ev[B.i];
        if (e->par_tour ? B.tour < e->tour : t < e->t) break;
        B.i++; B.appliques++;
        switch (e->action) {
        case B_DOWN: case B_UP: case B_CLIC: case B_MOVE:
            sc2_input_souris(e->x, e->y);
            if (e->action != B_MOVE) sc2_input_bouton(0, e->action != B_UP);
            if (e->action == B_CLIC) B.relacher_bouton = 1;
            sc2_log("[INPUT] banc t=%.2fs tour %u : %s (%d, %d)", t, B.tour,
                    e->action == B_DOWN ? "DOWN" : e->action == B_UP ? "UP" : e->action == B_CLIC ? "CLIC" : "MOVE", e->x, e->y);
            break;
        case B_TOUCHE:
            sc2_input_touche(e->vk, 1); B.relacher_vk = e->vk;
            sc2_log("[INPUT] banc t=%.2fs tour %u : touche VK 0x%02X enfoncée", t, B.tour, e->vk);
            return;                                    /* une touche à la fois : relâchée au tour suivant */
        case B_TBAS: case B_THAUT:
            sc2_input_touche(e->vk, e->action == B_TBAS);
            sc2_log("[INPUT] banc t=%.2fs tour %u : touche VK 0x%02X %s", t, B.tour, e->vk, e->action == B_TBAS ? "maintenue" : "relâchée");
            return;                                    /* un changement d'état de touche par tour */
        }
        if (B.relacher_bouton) return;                 /* le relâchement du CLIC passe avant l'événement suivant */
    }
}
