/* test_entrees_banc.c — SC2_BANC_ENTREES : analyse, QUAND:VICTOIRE, détection de victoire, annulation (20/10) ;
 * format 2 piloté par événements (V1.1, 22/10) : identité du format 1, déclencheurs, échelle, plafonds, CAPTURE.
 *
 * Le module (runtime/sc2_entrees_banc.c) est inclus par sc2_input_sim.c ; ici il est compilé SEUL, avec des
 * bouchons des fonctions du runtime (sc2_input_*, sc2_stop, sc2_log ; taille du client, masque GlueUI, images
 * présentées) définis ci-dessous. L'état B du module est static mais vit dans la même unité de traduction : le test
 * le réinitialise et le manipule directement. Trois substitutions, le temps des deux inclusions seulement :
 *   - clock_gettime → horloge SIMULÉE (g_sim, en secondes) : les instants sont exacts et rejouables ;
 *   - atexit → mémorisé, jamais enregistré (le bilan est appelé par le test, sa sortie comparée) ;
 *   - printf → capturé dans un tampon pendant les bilans.
 * IDENTITÉ DU FORMAT 1 : l'ancien module (main 08853c3, l. 39-248 : analyse, tri, détection, tour) est recopié
 * VERBATIM plus bas, renommé par des macros (V1_…). Pour chaque scénario de scripts/entrees/ que l'ancien analyseur lit
 * sans rejet, les deux modules sont initialisés par leur vrai sc2_banc_entrees_init (SC2_BANC_ENTREES=@fichier) puis
 * rejoués sur la même horloge simulée (pas de 1/60 s, arrêts de 2 à 7 s, victoire écrite à t = 130 s) : mêmes
 * événements champ à champ, même suite d'appels sc2_input_* / sc2_stop par tour, même journal (hors la ligne
 * « analyseur format 2 »), même bilan. Dépôt : SC2_DEPOT_TEST, sinon déduit de __FILE__.
 * La détection est essayée sur de vrais dossiers temporaires : victoire ancienne ignorée, victoire fraîche détectée.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- traces : appels injectés, journal, sortie capturée ------------------------------------------------------- */
typedef struct { char *p; size_t n, cap; } tampon;
static void t_ajouter(tampon *t, int nl, const char *fmt, va_list ap)       /* nl : fin de ligne ajoutée */
{
    va_list aq; va_copy(aq, ap);
    int k = vsnprintf(NULL, 0, fmt, aq); va_end(aq);
    if (k < 0) return;
    if (t->n + (size_t)k + 2 > t->cap) {
        size_t c = t->cap ? t->cap : 4096;
        while (t->n + (size_t)k + 2 > c) c *= 2;
        char *q = realloc(t->p, c); if (!q) return;
        t->p = q; t->cap = c;
    }
    vsnprintf(t->p + t->n, (size_t)k + 1, fmt, ap);
    t->n += (size_t)k;
    if (nl) t->p[t->n++] = '\n';
    t->p[t->n] = 0;
}
static void t_vider(tampon *t) { t->n = 0; if (t->p) t->p[0] = 0; }
static void t_printf(tampon *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void t_printf(tampon *t, const char *fmt, ...) { va_list ap; va_start(ap, fmt); t_ajouter(t, 1, fmt, ap); va_end(ap); }
static const char *t_str(const tampon *t) { return t->p ? t->p : ""; }
static void t_copier(tampon *d, const tampon *s)
{
    t_vider(d);
    if (!s->n) return;
    char *q = realloc(d->p, s->n + 1); if (!q) return;
    d->p = q; d->cap = s->n + 1; memcpy(d->p, s->p, s->n + 1); d->n = s->n;
}
static tampon g_appels, g_journal, g_sortie;
static int g_bavard = 1;                            /* journal du module recopié sur la sortie du test */
static uint32_t g_tour_sim;                          /* tour simulé : préfixe des appels tracés */

/* ---- bouchons du runtime (le vrai sc2_stop ne rend pas la main ; ici, message gardé et retour) ---- */
static int g_activations, g_souris, g_boutons_bas, g_touches_bas, g_arrets, g_molettes, g_dern_code;
static int32_t g_dern_x, g_dern_y;
static char g_dern_arret[256];
void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sc2_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); t_ajouter(&g_journal, 1, fmt, ap); va_end(ap);
    if (!g_bavard) return;
    fputs("[journal-test] ", stdout);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n');
}
void sc2_input_activer(void) { g_activations++; t_printf(&g_appels, "%u activer", g_tour_sim); }
void sc2_input_touche(uint32_t vk, int bas) { if (bas) g_touches_bas++; t_printf(&g_appels, "%u touche 0x%02X %d", g_tour_sim, vk, bas); }
void sc2_input_souris(int32_t x, int32_t y) { g_souris++; g_dern_x = x; g_dern_y = y; t_printf(&g_appels, "%u souris %d %d", g_tour_sim, x, y); }
void sc2_input_bouton(int bouton, int bas) { if (bas) g_boutons_bas++; t_printf(&g_appels, "%u bouton %d %d", g_tour_sim, bouton, bas); }
void sc2_input_molette(int32_t delta, int h) { g_molettes++; g_dern_x = delta; t_printf(&g_appels, "%u molette %d %d", g_tour_sim, delta, h); }
void sc2_stop(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sc2_stop(int code, const char *fmt, ...)
{
    va_list ap;
    g_arrets++; g_dern_code = code;
    va_start(ap, fmt); vsnprintf(g_dern_arret, sizeof g_dern_arret, fmt, ap); va_end(ap);
    t_printf(&g_appels, "%u stop %d %s", g_tour_sim, code, g_dern_arret);
}

/* ---- substitutions le temps des inclusions ---------------------------------------------------------------------- */
static double g_sim;                                 /* horloge monotone simulée, secondes */
static int test_horloge(struct timespec *t)
{
    t->tv_sec = (time_t)g_sim;
    t->tv_nsec = (long)((g_sim - (double)(time_t)g_sim) * 1e9 + 0.5);
    if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; }
    return 0;
}
static void (*g_bilan_enregistre)(void);
static int test_atexit(void (*f)(void)) { g_bilan_enregistre = f; return 0; }
static int test_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int test_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); t_ajouter(&g_sortie, 0, fmt, ap); va_end(ap); return 0; }
#undef printf
#define printf test_printf
#define atexit test_atexit
#define clock_gettime(horloge, t) test_horloge(t)

/* ---- ANCIEN MODULE (format 1), renommé ---------------------------------------------------------------------- */
#define B V1
#define banc_evt v1_banc_evt
#define B_DOWN V1_B_DOWN
#define B_UP V1_B_UP
#define B_CLIC V1_B_CLIC
#define B_MOVE V1_B_MOVE
#define B_TOUCHE V1_B_TOUCHE
#define B_TBAS V1_B_TBAS
#define B_THAUT V1_B_THAUT
#define B_QUIT V1_B_QUIT
#define banc_secondes v1_banc_secondes
#define banc_trier v1_banc_trier
#define banc_element v1_banc_element
#define BANC_SUFFIXE V1_BANC_SUFFIXE
#define banc_scruter_dossier v1_banc_scruter_dossier
#define banc_victoire_detectee v1_banc_victoire_detectee
#define banc_bilan v1_banc_bilan
#define sc2_banc_entrees_init v1_sc2_banc_entrees_init
#define sc2_banc_entrees_tour v1_sc2_banc_entrees_tour
/* ==== COPIE VERBATIM : git show 08853c3:recomp-sc2/runtime/sc2_entrees_banc.c | sed -n '39,248p' ==== */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define BANC_MAX 256
#define BANC_CHEMIN 4096
typedef struct { double t; uint32_t tour; int par_tour; int relatif; int action; int32_t x, y; uint32_t vk; } banc_evt;
enum { B_DOWN, B_UP, B_CLIC, B_MOVE, B_TOUCHE, B_TBAS, B_THAUT, B_QUIT };
static struct {
    int pret, n, i, n_abs;                         /* n_abs : éléments absolus (toujours en tête du tableau) */
    banc_evt ev[BANC_MAX];
    struct timespec t0;
    uint32_t tour;
    int relacher_bouton; uint32_t relacher_vk;     /* CLIC / ESC / KEY : relâchement au tour suivant */
    int appliques, rejetes;
    int quand_victoire;                            /* QUAND:VICTOIRE vu : scrutation + suite relative */
    char dossier_saves[BANC_CHEMIN];               /* vide : pas de scrutation (SC2_ROOT absent) */
    time_t depart_mur;                             /* mtime des victoires ≥ démarrage du module (heure murale) */
    double t_victoire;                             /* < 0 : pas (encore) de victoire ; sinon l'instant de détection */
    double prochaine_scrutation;
    char fichier_victoire[256];
    int annules;                                   /* éléments absolus annulés à la détection */
} B;

static double banc_secondes(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - B.t0.tv_sec) + (double)(t.tv_nsec - B.t0.tv_nsec) / 1e9;
}

/* ordre d'application d'un segment : instants croissants (les éléments « f » gardent leur rang relatif) */
static void banc_trier(int deb, int fin)
{
    for (int a = deb + 1; a < fin; a++)
        for (int b = a; b > deb && !B.ev[b].par_tour && !B.ev[b - 1].par_tour && B.ev[b].t < B.ev[b - 1].t; b--) {
            banc_evt t = B.ev[b]; B.ev[b] = B.ev[b - 1]; B.ev[b - 1] = t;
        }
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
    if (!strcasecmp(s, "QUAND") && !strcasecmp(p, "VICTOIRE")) { B.quand_victoire = 1; return; }
    if (B.quand_victoire && *s != '+') {
        B.rejetes++; sc2_log("[INPUT] banc : élément relatif sans « + » rejeté « %s:%s » (après QUAND:VICTOIRE)", s, p); return;
    }
    if (!B.quand_victoire && *s == '+') {
        B.rejetes++; sc2_log("[INPUT] banc : « + » sans QUAND:VICTOIRE — élément rejeté « %s:%s »", s, p); return;
    }
    if (B.quand_victoire) { e.relatif = 1; e.t = strtod(s + 1, NULL); }   /* « +6 » : secondes après la victoire */
    else if (*s == 'f' || *s == 'F') { e.par_tour = 1; e.tour = (uint32_t)strtoul(s + 1, NULL, 10); }
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
    else if (!strcasecmp(p, "QUIT")) e.action = B_QUIT;
    else { B.rejetes++; sc2_log("[INPUT] banc : action rejetée « %s%s%s »", p, q ? ":" : "", q ? q : ""); return; }
    B.ev[B.n++] = e;
}

/* Victoire dans <dossier> : un fichier « …Victory.SC2Save » dont le mtime est postérieur ou égal au démarrage du
 * module (celles des runs précédents ne comptent pas ; stat suit les liens). Rend 1 si trouvée, nom gardé. */
static const char BANC_SUFFIXE[] = "Victory.SC2Save";
static int banc_scruter_dossier(const char *dossier)
{
    DIR *d = opendir(dossier);
    if (!d) return 0;                                    /* absent ou pas encore créé : rien à voir */
    struct dirent *f;
    int vu = 0;
    while (!vu && (f = readdir(d))) {
        size_t n = strlen(f->d_name), ns = sizeof BANC_SUFFIXE - 1;
        if (n < ns || strcmp(f->d_name + n - ns, BANC_SUFFIXE)) continue;
        char chemin[BANC_CHEMIN];
        if (snprintf(chemin, sizeof chemin, "%s/%s", dossier, f->d_name) >= (int)sizeof chemin) continue;
        struct stat st;
        if (!stat(chemin, &st) && st.st_mtime >= B.depart_mur) {
            snprintf(B.fichier_victoire, sizeof B.fichier_victoire, "%s", f->d_name);
            vu = 1;
        }
    }
    closedir(d);
    return vu;
}

/* La victoire vient d'être vue à l'instant t : annulation des éléments absolus non joués, les relatifs
 * partent de t. Les frappes de secours d'un scénario minuté ne viennent plus gêner le « Continue ». */
static void banc_victoire_detectee(double t)
{
    B.t_victoire = t;
    B.annules = B.n_abs > B.i ? B.n_abs - B.i : 0;
    B.i = B.n_abs;
    sc2_log("[INPUT] banc t=%.2fs tour %u : VICTOIRE détectée (%s)", t, B.tour, B.fichier_victoire);
    sc2_log("[INPUT] banc t=%.2fs : %d élément(s) absolu(s) restant(s) annulé(s) — la suite est relative à la victoire",
            t, B.annules);
}

static void banc_bilan(void)
{
    printf("[BILAN] [INPUT] banc (SC2_BANC_ENTREES) : %d/%d événement(s) appliqué(s), %d rejeté(s), %u tours de pompe, %.1f s\n",
           B.appliques, B.n, B.rejetes, B.tour, banc_secondes());
    if (B.quand_victoire) {
        char v[320];
        if (B.t_victoire >= 0) snprintf(v, sizeof v, "t=%.2fs (%s)", B.t_victoire, B.fichier_victoire);
        else snprintf(v, sizeof v, "pas de victoire");
        printf("[BILAN] [INPUT] banc victoire (QUAND:VICTOIRE) : %s ; %d élément(s) absolu(s) annulé(s)\n", v, B.annules);
    }
}

/* Au premier import résolu (sc2_input_find) : lecture de la variable, activation du module d'entrées. */
static void sc2_banc_entrees_init(void)
{
    const char *v = getenv("SC2_BANC_ENTREES");
    if (!v || !*v || B.pret) return;
    B.pret = 1;
    clock_gettime(CLOCK_MONOTONIC, &B.t0);
    B.t_victoire = -1;
    B.depart_mur = time(NULL);                           /* heure murale : comparée aux mtime des sauvegardes */
    B.prochaine_scrutation = 1.0;                        /* 1re scrutation à t ≈ 1 s, puis une par seconde */
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
    /* absolus en tête (garanti par l'analyse : sans « + » après QUAND:VICTOIRE, « + » avant : rejetés) */
    for (B.n_abs = 0; B.n_abs < B.n && !B.ev[B.n_abs].relatif; B.n_abs++) ;
    banc_trier(0, B.n_abs);
    banc_trier(B.n_abs, B.n);
    if (B.quand_victoire) {
        const char *racine = getenv("SC2_ROOT");
        if (!racine || !*racine || snprintf(B.dossier_saves, sizeof B.dossier_saves,
                "%s/Users/sc2/Documents/StarCraft II/Saves/Unsaved", racine) >= (int)sizeof B.dossier_saves) {
            B.dossier_saves[0] = 0;
            sc2_log("[INPUT] banc : SC2_ROOT absent — victoire non scrutée, les éléments relatifs ne partiront jamais");
        } else
            sc2_log("[INPUT] banc : victoire scrutée dans %s (…Victory.SC2Save écrit pendant le run, ~1 fois/s)",
                    B.dossier_saves);
    }
    sc2_input_activer();
    atexit(banc_bilan);
    sc2_log("[INPUT] banc : SC2_BANC_ENTREES = %d événement(s), %d rejeté(s) ; horloge = secondes depuis l'activation (f<n> : tours de pompe)",
            B.n, B.rejetes);
}

/* À chaque tour de la pompe du jeu, hors verrou : relâchements en attente, scrutation (~1/s) si armée,
 * puis événements échus — absolus par instant, relatifs après une victoire détectée. */
static void sc2_banc_entrees_tour(void)
{
    if (!B.pret) return;
    B.tour++;
    double t = banc_secondes();
    if (B.relacher_bouton) { sc2_input_bouton(0, 0); B.relacher_bouton = 0; sc2_log("[INPUT] banc t=%.2fs tour %u : bouton gauche relâché (fin de CLIC)", t, B.tour); }
    if (B.relacher_vk) { sc2_input_touche(B.relacher_vk, 0); sc2_log("[INPUT] banc t=%.2fs tour %u : VK 0x%02X relâchée", t, B.tour, B.relacher_vk); B.relacher_vk = 0; }
    if (B.quand_victoire && B.t_victoire < 0 && B.dossier_saves[0] && t >= B.prochaine_scrutation) {
        B.prochaine_scrutation = t + 1.0;
        if (banc_scruter_dossier(B.dossier_saves)) banc_victoire_detectee(t);
    }
    while (B.i < B.n) {
        banc_evt *e = &B.ev[B.i];
        if (e->relatif) { if (B.t_victoire < 0 || t - B.t_victoire < e->t) break; }
        else if (e->par_tour ? B.tour < e->tour : t < e->t) break;
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
        case B_QUIT:
            sc2_log("[INPUT] banc t=%.2fs tour %u : QUIT — arrêt volontaire du run", t, B.tour);
            sc2_stop(0, "SC2_BANC_ENTREES : QUIT à t=%.1fs", t);
            return;                                    /* sc2_stop ne rend pas la main (bouchon du test excepté) */
        }
        if (B.relacher_bouton) return;                 /* le relâchement du CLIC passe avant l'événement suivant */
    }
}
/* ==== FIN DE LA COPIE VERBATIM ==== */
#undef B
#undef banc_evt
#undef B_DOWN
#undef B_UP
#undef B_CLIC
#undef B_MOVE
#undef B_TOUCHE
#undef B_TBAS
#undef B_THAUT
#undef B_QUIT
#undef banc_secondes
#undef banc_trier
#undef banc_element
#undef BANC_SUFFIXE
#undef banc_scruter_dossier
#undef banc_victoire_detectee
#undef banc_bilan
#undef sc2_banc_entrees_init
#undef sc2_banc_entrees_tour
#undef BANC_MAX
#undef BANC_CHEMIN

/* ---- MODULE ACTUEL (format 2) ----------------------------------------------------------------------------------- */
#include "../sc2_entrees_banc.c"

#undef printf
#undef atexit
#undef clock_gettime

/* références faibles du module : bouchons forts (taille du client, masque GlueUI, images présentées) */
static uint32_t g_cli_w = 1280, g_cli_h = 720, g_masque, g_frames;
void sc2_d3d9_taille(uint32_t *w, uint32_t *h) { *w = g_cli_w; *h = g_cli_h; }
uint32_t sc2_glue_masque(void) { return g_masque; }
uint32_t sc2_frames_presentees(void) { return g_frames; }

static int g_pass, g_fail;
#define CHECK(cond, msg) do { if (cond) { g_pass++; printf("PASS  %s\n", msg); } \
                              else { g_fail++; printf("FAIL  %s\n", msg); } } while (0)
#define CHECKF(cond, ...) do { char m_[512]; snprintf(m_, sizeof m_, __VA_ARGS__); CHECK(cond, m_); } while (0)

/* banc_element réécrit son argument : toujours passer une copie modifiable. */
static void elem(const char *s)
{
    char b[256];
    snprintf(b, sizeof b, "%s", s);
    banc_element(b);
}

static void reinit(void)
{
    banc_raz();
    g_activations = g_souris = g_boutons_bas = g_touches_bas = g_arrets = g_molettes = g_dern_code = 0;
    g_dern_x = g_dern_y = 0;
    g_dern_arret[0] = 0;
    g_sim = 0; g_tour_sim = 0; g_masque = 0; g_frames = 0; g_cli_w = 1280; g_cli_h = 720;
    t_vider(&g_appels); t_vider(&g_journal); t_vider(&g_sortie);
    while (sc2_banc_capture_prendre()) ;
}

/* L'horloge du module vaut age : t0 = 0 (banc_raz) et g_sim = age. */
static void horloge(double age) { g_sim = age; }

/* scénario en texte (« ; » ou fins de ligne), puis départ comme à l'init : segment 0 joué, segment 1 armé à t = 0 */
static void charger(const char *texte)
{
    static char b[65536];
    snprintf(b, sizeof b, "%s", texte);
    banc_charger_texte(b);
    B.pret = 1;
    B.prochaine_scrutation = 1.0;
    banc_armer(1, 0.0);
}
static void demarrer(void) { banc_finir(); B.pret = 1; banc_armer(1, 0.0); }
static int n_abs(void) { return B.nseg > 1 ? B.seg[1].deb : B.n; }
static void tour_a(double t) { g_sim = t; g_tour_sim++; sc2_banc_entrees_tour(); }
static int journal_a(const char *motif) { return strstr(t_str(&g_journal), motif) != NULL; }

/* ---- analyse des éléments ----------------------------------------------------------- */
static void tests_analyse(void)
{
    reinit();
    elem("30:CLIC:640,360");
    elem("f700:ESC");
    elem("300:QUIT");
    elem("12.5:KEYDOWN:0x12");
    CHECK(B.n == 4 && B.ev[0].t == 30 && !B.ev[0].relatif && B.ev[0].action == B_CLIC && B.ev[0].x == 640 && B.ev[0].y == 360,
          "analyse : CLIC absolu (instant en secondes, coordonnées client)");
    CHECK(B.ev[1].par_tour && B.ev[1].tour == 700 && B.ev[1].action == B_TOUCHE && B.ev[1].vk == 0x1B,
          "analyse : f700:ESC = tours de pompe (comportement d'avant)");
    CHECK(B.ev[2].action == B_QUIT && !B.ev[2].relatif && B.ev[2].t == 300,
          "analyse : 300:QUIT = action QUIT absolue (garde-fou)");
    CHECK(B.ev[3].action == B_TBAS && B.ev[3].vk == 0x12 && B.ev[3].t == 12.5,
          "analyse : 12.5:KEYDOWN:0x12 inchangé");
    CHECK(B.rejetes == 0 && B.quand_victoire == 0 && B.nseg == 1 && !B.format2,
          "analyse : sans QUAND:VICTOIRE, rien d'armé ni rejeté (format 1)");

    reinit();
    elem("40:CLIC:953,588");
    elem("QUAND:VICTOIRE");
    elem("+6:CLIC:998,674");
    elem("+120:QUIT");
    elem("QUAND:VICTOIRE");                             /* répété nu : sans effet (format 1) */
    CHECK(B.quand_victoire == 1 && B.n == 3 && B.nseg == 2 && !B.format2,
          "QUAND:VICTOIRE arme la victoire sans compter pour un événement (répété : sans effet)");
    CHECK(B.ev[0].t == 40 && !B.ev[0].relatif,
          "l'élément d'avant le marqueur garde son instant ABSOLU");
    CHECK(B.ev[1].relatif && B.ev[1].t == 6 && B.ev[1].action == B_CLIC && B.ev[1].x == 998 && B.ev[1].y == 674,
          "« +6:CLIC:998,674 » = relatif, 6 s après la détection");
    CHECK(B.ev[2].relatif && B.ev[2].action == B_QUIT && B.ev[2].t == 120,
          "« +120:QUIT » = QUIT relatif");

    reinit();
    elem("QUAND:VICTOIRE");
    elem("5:CLIC:1,1");
    elem("3:ESC");
    CHECK(B.n == 0 && B.rejetes == 2,
          "après QUAND:VICTOIRE : un élément sans « + » est rejeté (2 rejets)");
    reinit();
    elem("+5:CLIC:1,1");
    CHECK(B.n == 0 && B.rejetes == 1,
          "un « + » AVANT QUAND:VICTOIRE est rejeté");

    /* format 2 : marqueurs, options, en-têtes, actions */
    reinit();
    elem("REF:1280x720");
    elem("GARDE:240");
    elem("ECHELLE:scene");
    elem("QUAND:CARTE:TStory01 MAX:60 SINON:QUIT");
    elem("QUAND:PRET MAX:90");
    elem("+f30:CLIC:270,510");
    elem("+2:RCLIC:10,20@ui");
    elem("+3:CLIC:50%,25%");
    elem("+4:DRAG:1,2,3,4,0.25@lin");
    elem("+5:WAIT");
    elem("+6:CAPTURE:bar-pret");
    elem("+7:TEXTE:Ab 1");
    elem("+8:MOLETTE:-120");
    elem("QUAND:FICHIER:*Victory.SC2Save ECRITURE MAX:600 SINON:SAUTE");
    elem("QUAND:CHARGEMENT");
    elem("QUAND:GLUE:0x11");
    elem("QUAND:FRAME:500");
    elem("QUAND:STABLE:2.5");
    banc_finir();
    CHECK(B.rejetes == 0 && B.format2 && B.ref_w == 1280 && B.ref_h == 720 && B.garde == 240 && B.mode_defaut == M_SCENE,
          "format 2 : en-têtes REF:, GARDE:, ECHELLE: lus, 0 rejet");
    CHECK(B.nseg == 8 && B.seg[1].decl == Q_CARTE && !strcmp(B.seg[1].arg, "TStory01") && B.seg[1].max == 60 && B.seg[1].sinon == S_QUIT
          && B.seg[2].decl == Q_PRET && B.seg[2].max == 90 && B.seg[2].sinon == S_SUITE,
          "format 2 : QUAND:CARTE:TStory01 MAX:60 SINON:QUIT, QUAND:PRET MAX:90 (SINON:SUITE par défaut)");
    CHECK(B.seg[3].decl == Q_FICHIER && B.seg[3].ecriture && B.seg[3].sinon == S_SAUTE && B.seg[4].decl == Q_CHARGEMENT
          && B.seg[5].decl == Q_GLUE && B.seg[5].val == 0x11 && B.seg[6].decl == Q_FRAME && B.seg[6].val == 500
          && B.seg[7].decl == Q_STABLE && B.seg[7].sec == 2.5,
          "format 2 : FICHIER (ECRITURE, SAUTE), CHARGEMENT, GLUE:0x11, FRAME:500, STABLE:2.5");
    banc_evt *e = &B.ev[B.seg[2].deb];
    CHECK(B.seg[2].fin - B.seg[2].deb == 11 && e[0].par_tour && e[0].tour == 30 && e[0].seg == 2 && e[0].relatif,
          "format 2 : « +f30 » = 30 tours après le déclenchement (segment 2)");
    CHECK(e[1].action == B_RCLIC && e[1].mode == M_UI && e[2].mode == M_POURCENT && e[2].px == 50 && e[2].py == 25
          && e[3].action == B_DRAG && e[3].x2 == 3 && e[3].y2 == 4 && e[3].duree == 0.25 && e[3].mode == M_LIN
          && e[4].action == B_WAIT && e[5].action == B_CAPTURE && !strcmp(e[5].nom, "bar-pret"),
          "format 2 : RCLIC@ui, CLIC en %, DRAG,0.25@lin, WAIT, CAPTURE:bar-pret");
    CHECK(e[6].action == B_TOUCHE && e[6].vk == 0x41 && e[6].t == 7 && e[7].vk == 0x42 && e[7].t > 7.29 && e[7].t < 7.31
          && e[8].vk == 0x20 && e[9].vk == 0x31 && e[9].t > 7.89 && e[9].t < 7.91 && e[10].action == B_MOLETTE && e[10].x == -120,
          "format 2 : TEXTE:Ab 1 = 4 KEY (A B espace 1) espacées de 0,3 s ; MOLETTE:-120");

    reinit();
    elem("QUAND:INCONNU:3");
    elem("+1:CLIC:1,1");
    elem("QUAND:CARTE:");
    elem("QUAND:PRET MAX:abc");
    elem("+1:TEXTE:a;b");                               /* « ; » n'atteint jamais un élément (découpe) : ici « a;b » brut */
    elem("+1:TEXTE:é");
    elem("+1:CAPTURE:../x");
    elem("+1:CAPTURE:a/b");
    elem("+1:CAPTURE:.cache");
    elem("+1:CAPTURE:abcdefghijabcdefghijabcdefghijabcdefghijabcdefgh");   /* 48 caractères */
    elem("+1:CLIC:1,2@nulle");
    elem("+1:DRAG:1,2,3");
    elem("+1:CLIC:120%,5%");
    elem("REF:12x7");
    elem("ECHELLE:biais");
    elem("GARDE:0");
    CHECKF(B.rejetes == 15 && B.nseg == 4 && B.seg[1].decl == Q_INVALIDE && B.seg[2].decl == Q_INVALIDE && B.seg[3].decl == Q_INVALIDE
           && B.n == 1 && B.ev[0].seg == 1,
           "format 2 : 3 marqueurs invalides (segments MORTS : leurs « + » ne glissent pas avant), 9 actions et 3 en-têtes rejetés (%d rejets)",
           B.rejetes);
}

static void tests_tri(void)
{
    reinit();
    elem("300:QUIT");
    elem("40:CLIC:953,588");
    elem("5:ESC");
    elem("QUAND:VICTOIRE");
    elem("+120:QUIT");
    elem("+6:CLIC:998,674");
    banc_finir();
    CHECK(n_abs() == 3 && B.ev[0].t == 5 && B.ev[1].t == 40 && B.ev[2].t == 300,
          "tri : absolus par instants croissants, relatifs rejetés du segment");
    CHECK(B.ev[3].relatif && B.ev[3].t == 6 && B.ev[4].relatif && B.ev[4].t == 120,
          "tri : relatifs triés à part, placés après les absolus");
    reinit();
    elem("QUAND:PRET");
    elem("+9:ESC");
    elem("+f100:WAIT");
    elem("+2:ESC");
    elem("QUAND:FRAME:3");
    elem("+5:ESC");
    elem("+1:ESC");
    banc_finir();
    CHECK(B.ev[0].t == 9 && B.ev[1].par_tour && B.ev[2].t == 2 && B.ev[3].t == 1 && B.ev[4].t == 5,
          "tri : chaque segment trié à part, un élément « +f » garde son rang");
}

/* ---- détection dans un dossier temporaire ------------------------------------------- */
static void tests_detection(void)
{
    char mod[] = "/tmp/sc2-test-banc-XXXXXX";
    if (!mkdtemp(mod)) { CHECK(0, "mkdtemp du dossier de sauvegardes"); return; }
    reinit();
    B.depart_mur = time(NULL) - 10;
    char vieux[300], autre[300], neuf[300];
    snprintf(vieux, sizeof vieux, "%s/Liberation Day Victory.SC2Save", mod);   /* run précédent */
    snprintf(autre, sizeof autre, "%s/auto.SC2Save", mod);                     /* pas une victoire */
    snprintf(neuf, sizeof neuf, "%s/The Outlaws Victory.SC2Save", mod);        /* la nôtre */
    for (char **p = (char *[]){ vieux, autre, NULL }; *p; p++) { FILE *f = fopen(*p, "w"); if (f) { fputs("x", f); fclose(f); } }
    struct timeval tv[2] = { { .tv_sec = B.depart_mur - 3600, .tv_usec = 0 }, { .tv_sec = B.depart_mur - 3600, .tv_usec = 0 } };
    utimes(vieux, tv);
    CHECK(banc_scruter_dossier(mod) == 0 && B.fichier_victoire[0] == 0,
          "détection : victoire d'un run précédent (mtime ancien) et non-victoire ignorées");
    FILE *f = fopen(neuf, "w");
    if (f) { fputs("x", f); fclose(f); }
    CHECK(banc_scruter_dossier(mod) == 1 && !strcmp(B.fichier_victoire, "The Outlaws Victory.SC2Save"),
          "détection : …Victory.SC2Save écrit pendant le run détecté (nom gardé)");
    CHECK(banc_scruter_dossier("/tmp/sc2-test-banc-inexistant-42") == 0,
          "détection : dossier absent → simplement rien");
    unlink(vieux); unlink(autre); unlink(neuf); rmdir(mod);
}

static void tests_annulation(void)
{
    reinit();
    elem("5:ESC");
    elem("50:CLIC:1,1");
    elem("300:QUIT");
    elem("QUAND:VICTOIRE");
    elem("+6:CLIC:998,674");
    elem("+120:QUIT");
    demarrer();
    B.i = 1;                                            /* le 5:ESC a déjà été joué */
    banc_declencher(1, 42.0, 42.0, "test", 0);
    CHECK(B.t_victoire == 42.0 && B.annules == 2 && B.i == n_abs() && B.seg_joue == 1,
          "victoire : 2 absolus restants annulés, curseur posé aux relatifs");
}

/* ---- boucle de tour : scrutation, annulation, relatifs, QUIT ------------------------ */
static void tests_tour(void)
{
    char mod[] = "/tmp/sc2-test-banc-XXXXXX";
    if (!mkdtemp(mod)) { CHECK(0, "mkdtemp du dossier de sauvegardes (tour)"); return; }
    char neuf[300];
    snprintf(neuf, sizeof neuf, "%s/Mission X Victory.SC2Save", mod);

    /* sans QUAND:VICTOIRE : aucune scrutation, même un dossier plein de victoires fraîches */
    reinit();
    horloge(10.0);
    B.depart_mur = time(NULL) - 10;
    snprintf(B.dossier_saves, sizeof B.dossier_saves, "%s", mod);
    FILE *f = fopen(neuf, "w");
    if (f) { fputs("x", f); fclose(f); }
    elem("100:CLIC:1,1");
    demarrer();
    B.prochaine_scrutation = 0;
    sc2_banc_entrees_tour();
    CHECK(B.t_victoire == -1 && B.appliques == 0 && g_souris == 0,
          "tour : sans QUAND:VICTOIRE, pas de scrutation (comportement d'avant, bit à bit)");

    /* avec QUAND:VICTOIRE : détection au tour → absolus restants annulés, relatifs à leur heure */
    reinit();
    horloge(10.0);
    B.depart_mur = time(NULL) - 10;
    snprintf(B.dossier_saves, sizeof B.dossier_saves, "%s", mod);
    elem("50:CLIC:1,1");                                /* pas encore dû à t=10 : annulé par la victoire */
    elem("5:ESC");                                      /* dû mais non joué : le tour a détecté AVANT d'appliquer */
    elem("QUAND:VICTOIRE");
    elem("+6:CLIC:998,674");
    elem("+120:QUIT");
    demarrer();
    B.prochaine_scrutation = 0;
    sc2_banc_entrees_tour();
    CHECK(B.t_victoire >= 0 && B.annules == 2 && B.i == n_abs(),
          "tour : victoire détectée (~1 fois/s), les 2 absolus restants sont annulés");
    CHECK(B.appliques == 0 && g_touches_bas == 0 && g_souris == 0,
          "tour : aucun absolu n'est appliqué après la détection");
    horloge(16.5);                                      /* t − t_victoire = 6,5 s ≥ 6 s */
    sc2_banc_entrees_tour();
    CHECK(g_souris == 1 && g_dern_x == 998 && g_dern_y == 674 && g_boutons_bas == 1,
          "tour : +6:CLIC:998,674 appliqué 6 s après la victoire (et pas avant)");
    CHECK(g_arrets == 0,
          "tour : le QUIT relatif (+120) n'est pas encore dû");
    horloge(200.0);                                     /* largement après t_victoire + 120 */
    sc2_banc_entrees_tour();
    CHECK(g_arrets == 1 && g_dern_code == 0 && strstr(g_dern_arret, "SC2_BANC_ENTREES : QUIT à t=") == g_dern_arret,
          "tour : +120:QUIT → sc2_stop(0) volontaire (« SC2_BANC_ENTREES : QUIT à t=…s »)");

    /* relatifs jamais joués sans victoire */
    reinit();
    horloge(1000.0);
    elem("QUAND:VICTOIRE");
    elem("+1:CLIC:9,9");
    demarrer();
    sc2_banc_entrees_tour();
    CHECK(B.appliques == 0 && g_souris == 0 && B.t_victoire == -1,
          "tour : sans victoire (dossier non armé), les relatifs ne sont jamais joués");

    unlink(neuf);
    rmdir(mod);
}

/* ---- identité du format 1 : ancien et nouveau module sur les scénarios du dépôt ------------------------------- */
static char g_depot[1024];
static int trouver_depot(void)
{
    const char *e = getenv("SC2_DEPOT_TEST");
    if (e && *e) snprintf(g_depot, sizeof g_depot, "%s", e);
    else {
        snprintf(g_depot, sizeof g_depot, "%s", __FILE__);
        char *p = strstr(g_depot, "/runtime/tests/");
        if (!p) return 0;
        *p = 0;
    }
    char c[1200]; struct stat st;
    snprintf(c, sizeof c, "%s/scripts/entrees", g_depot);
    return !stat(c, &st) && S_ISDIR(st.st_mode);
}

typedef struct { int n, rejetes; banc_evt ev[BANC_MAX]; } analyse_t;
static analyse_t g_ana1, g_ana2;
static tampon g_ap1, g_jo1, g_so1;

/* journal sans la ligne propre au format 2 (« analyseur format 2 … ») */
static void filtrer_journal(const char *src, tampon *dst)
{
    t_vider(dst);
    for (const char *l = src; *l; ) {
        const char *f = strchr(l, '\n'); size_t n = f ? (size_t)(f - l) : strlen(l);
        if (strncmp(l, "[INPUT] banc : analyseur format 2", 33)) t_printf(dst, "%.*s", (int)n, l);
        l += n + (f ? 1 : 0);
    }
}

/* Rejeu sur horloge simulée : pas de 1/60 s, arrêts de 2 à 7 s (≈ 0,3 % des tours), victoire écrite à t = 130 s,
 * arrêt au premier sc2_stop ou à t = 400 s. v1 : ancien module, sinon le module actuel. */
static void rejouer(int v1, const char *racine, const char *victoire)
{
    uint32_t lcg = 0x5C2u; int ecrite = 0;
    for (g_tour_sim = 1; g_tour_sim < 400000 && !g_arrets && g_sim < 400.0; g_tour_sim++) {
        lcg = lcg * 1103515245u + 12345u;
        unsigned r = (lcg >> 16) % 1000;
        g_sim += r < 3 ? 2.0 + 2.5 * r : 1.0 / 60;
        if (!ecrite && g_sim >= 130.0) {
            ecrite = 1;
            FILE *f = fopen(victoire, "w"); if (f) { fputs("x", f); fclose(f); }
        }
        if (v1) v1_sc2_banc_entrees_tour(); else sc2_banc_entrees_tour();
    }
    (void)racine;
}

static void tests_identite_format1(void)
{
    if (!trouver_depot()) { CHECKF(0, "identité format 1 : dossier scripts/entrees introuvable (dépôt « %s », SC2_DEPOT_TEST)", g_depot); return; }
    char racine[] = "/tmp/sc2-test-banc-XXXXXX";
    if (!mkdtemp(racine)) { CHECK(0, "identité format 1 : mkdtemp"); return; }
    char dossier[600], victoire[700], cmd[700];
    snprintf(dossier, sizeof dossier, "%s/Users/sc2/Documents/StarCraft II/Saves/Unsaved", racine);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dossier);
    if (system(cmd)) { CHECK(0, "identité format 1 : dossier de sauvegardes"); return; }
    snprintf(victoire, sizeof victoire, "%s/Mission X Victory.SC2Save", dossier);
    setenv("SC2_ROOT", racine, 1);
    const char *sous[] = { "scripts/entrees", "scripts/entrees/diag" };
    int compares = 0, hors = 0, appels_total = 0;
    for (int d = 0; d < 2; d++) {
        char rep[1200]; snprintf(rep, sizeof rep, "%s/%s", g_depot, sous[d]);
        DIR *dir = opendir(rep);
        if (!dir) continue;
        char noms[64][256]; int nn = 0;
        for (struct dirent *f; (f = readdir(dir)) && nn < 64; ) {
            size_t l = strlen(f->d_name);
            if (l > 4 && !strcmp(f->d_name + l - 4, ".txt")) snprintf(noms[nn++], 256, "%s", f->d_name);
        }
        closedir(dir);
        qsort(noms, (size_t)nn, sizeof noms[0], (int (*)(const void *, const void *))strcmp);
        for (int k = 0; k < nn; k++) {
            char chemin[1500], var[1600];
            snprintf(chemin, sizeof chemin, "%s/%s", rep, noms[k]);
            snprintf(var, sizeof var, "@%s", chemin);
            setenv("SC2_BANC_ENTREES", var, 1);
            g_bavard = 0;
            /* ancien module */
            memset(&V1, 0, sizeof V1);
            g_sim = 0; g_tour_sim = 0; g_arrets = 0; unlink(victoire);
            t_vider(&g_appels); t_vider(&g_journal); t_vider(&g_sortie);
            v1_sc2_banc_entrees_init();
            g_ana1.n = V1.n; g_ana1.rejetes = V1.rejetes;
            for (int i = 0; i < V1.n; i++) {
                banc_evt *x = &g_ana1.ev[i]; memset(x, 0, sizeof *x);
                x->t = V1.ev[i].t; x->tour = V1.ev[i].tour; x->par_tour = V1.ev[i].par_tour; x->relatif = V1.ev[i].relatif;
                x->action = V1.ev[i].action; x->x = V1.ev[i].x; x->y = V1.ev[i].y; x->vk = V1.ev[i].vk;
            }
            rejouer(1, racine, victoire);
            g_bilan_enregistre();
            t_copier(&g_ap1, &g_appels);
            filtrer_journal(t_str(&g_journal), &g_jo1);
            t_copier(&g_so1, &g_sortie);
            if (V1.rejetes) {                           /* fichier du format 2 (ou erroné) pour l'ancien : hors identité */
                g_bavard = 1; hors++;
                printf("info  identité format 1 : %s/%s hors comparaison (%d rejet(s) par l'ancien analyseur : format 2)\n",
                       sous[d] + 8, noms[k], V1.rejetes);
                continue;
            }
            /* module actuel */
            reinit(); g_arrets = 0; unlink(victoire);
            sc2_banc_entrees_init();
            g_ana2.n = B.n; g_ana2.rejetes = B.rejetes;
            int ev_ok = g_ana1.n == g_ana2.n && g_ana1.rejetes == g_ana2.rejetes && !B.format2, i_diff = -1;
            for (int i = 0; ev_ok && i < B.n; i++) {
                banc_evt *x = &g_ana1.ev[i], *y = &B.ev[i];
                if (x->t != y->t || x->tour != y->tour || x->par_tour != y->par_tour || x->relatif != y->relatif
                    || x->action != y->action || x->x != y->x || x->y != y->y || x->vk != y->vk || y->relatif != (y->seg > 0))
                    { ev_ok = 0; i_diff = i; }
            }
            rejouer(0, racine, victoire);
            g_bilan_enregistre();
            tampon jo2 = { 0 };
            filtrer_journal(t_str(&g_journal), &jo2);
            int ap_ok = !strcmp(t_str(&g_ap1), t_str(&g_appels)), jo_ok = !strcmp(t_str(&g_jo1), t_str(&jo2)),
                so_ok = !strcmp(t_str(&g_so1), t_str(&g_sortie));
            int n_appels = 0; for (const char *c = t_str(&g_appels); *c; c++) n_appels += *c == '\n';
            g_bavard = 1;
            compares++; appels_total += n_appels;
            CHECKF(ev_ok && ap_ok && jo_ok && so_ok && n_appels > 0,
                   "identité format 1 : %s/%s — %d événement(s), %d appel(s) sc2_input_*/sc2_stop, journal et bilan identiques "
                   "(événements %s%s, appels %s, journal %s, bilan %s)", sous[d] + 8, noms[k], B.n, n_appels,
                   ev_ok ? "ok" : "DIFF", ev_ok ? "" : (i_diff >= 0 ? " à l'indice" : ""), ap_ok ? "ok" : "DIFF", jo_ok ? "ok" : "DIFF",
                   so_ok ? "ok" : "DIFF");
            if (!ap_ok || !jo_ok || !so_ok) {
                FILE *f1 = fopen("identite-ancien.txt", "w"), *f2 = fopen("identite-nouveau.txt", "w");
                if (f1 && f2) { fprintf(f1, "%s\n----\n%s\n----\n%s", t_str(&g_ap1), t_str(&g_jo1), t_str(&g_so1));
                                fprintf(f2, "%s\n----\n%s\n----\n%s", t_str(&g_appels), t_str(&jo2), t_str(&g_sortie)); }
                if (f1) fclose(f1);
                if (f2) fclose(f2);
                printf("      (détail : identite-ancien.txt / identite-nouveau.txt dans le dossier courant)\n");
            }
            free(jo2.p);
        }
    }
    CHECKF(compares >= 16, "identité format 1 : %d scénario(s) du format 1 comparé(s) (≥ 16 attendus), %d appel(s) au total, "
           "%d fichier(s) du format 2 hors comparaison", compares, appels_total, hors);
    unlink(victoire);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", racine);
    if (system(cmd)) printf("info  nettoyage de %s impossible\n", racine);
    unsetenv("SC2_BANC_ENTREES"); unsetenv("SC2_ROOT");
}

/* ---- déclencheurs ------------------------------------------------------------------------------------------------ */
static void tests_carte(void)
{
    reinit();
    charger("QUAND:CARTE:TRaynor02\n+1:CLIC:10,10");
    tour_a(1.0);
    sc2_banc_mpq_lu("Maps\\Campaign\\TRaynor02B.SC2Map\\DocumentHeader");          /* autre carte (préfixe commun) */
    sc2_banc_mpq_lu("Maps\\Campaign\\TRaynor02.SC2Map\\Objects");                  /* autre fichier de la carte */
    sc2_banc_mpq_lu("Maps\\Campaign\\TRaynor0.SC2Map\\DocumentHeader");            /* nom plus court */
    sc2_banc_mpq_lu("Mods\\Campaign\\TRaynor02.SC2Map\\DocumentHeader");           /* hors Maps\ */
    tour_a(1.5);
    CHECK(B.seg[1].etat == E_ARME && B.seg_joue == 0,
          "CARTE : TRaynor02B, …\\Objects, TRaynor0, hors Maps\\ → rien");
    horloge(2.0);
    sc2_banc_mpq_lu("Maps\\Campaign\\TRaynor02.SC2Map\\DocumentHeader");          /* ligne [MPQ] simulée */
    CHECK(B.seg[1].etat == E_ARME, "CARTE : le rappel ne fait que déposer (rien n'est joué hors du tour)");
    tour_a(2.5);
    CHECK(B.seg[1].etat == E_DECLENCHE && B.seg_joue == 1 && B.seg[1].t_decl == 2.0 && g_souris == 0,
          "CARTE : Maps\\Campaign\\TRaynor02.SC2Map\\DocumentHeader → segment déclenché au tour suivant, horodaté au rappel (t=2,0)");
    tour_a(2.9);
    CHECK(g_souris == 0, "CARTE : +1:CLIC pas avant t_rappel + 1 s");
    tour_a(3.05);
    CHECK(g_souris == 1 && g_dern_x == 10 && g_boutons_bas == 1, "CARTE : +1:CLIC à t_rappel + 1 s");
    int sans_ext = 1;
    for (const char *l = t_str(&g_journal); (l = strstr(l, "[INPUT] banc")); l++) {
        const char *f = strchr(l, '\n'); size_t n = f ? (size_t)(f - l) : strlen(l);
        char ligne[600]; snprintf(ligne, sizeof ligne, "%.*s", (int)(n < 599 ? n : 599), l);
        if (strstr(ligne, ".SC2Map") || strstr(ligne, "Loading") || strstr(ligne, "→ Maps\\")) sans_ext = 0;
    }
    CHECK(journal_a("QUAND:CARTE:TRaynor02 déclenché (DocumentHeader de TRaynor02 lu)") && sans_ext,
          "CARTE : journal « QUAND:CARTE:TRaynor02 déclenché … », sans « .SC2Map », « Loading » ni « → Maps\\ »");

    /* lecture AVANT l'armement : rien ; une 2e lecture après l'armement déclenche ; casse ignorée */
    reinit();
    charger("QUAND:FRAME:5\nQUAND:CARTE:traynor02\n+0:WAIT");
    sc2_banc_mpq_lu("Maps\\Campaign\\TRaynor02.SC2Map\\DocumentHeader");
    tour_a(1.0);
    g_frames = 5;
    tour_a(1.1);
    tour_a(1.2);
    tour_a(1.3);
    CHECK(B.seg[1].etat == E_DECLENCHE && B.seg[2].etat == E_ARME,
          "CARTE : DocumentHeader lu AVANT l'armement (segment FRAME en cours) → rien");
    sc2_banc_mpq_lu("maps\\campaign\\TRAYNOR02.sc2map\\documentheader");
    tour_a(1.4);
    CHECK(B.seg[2].etat == E_DECLENCHE && journal_a("WAIT (jalon)"),
          "CARTE : nouvelle lecture après l'armement (casse indifférente) → déclenché, +0:WAIT joué");

    /* deux segments CARTE de suite : la carte du second lue pendant que le premier attend → rien pour le second */
    reinit();
    charger("QUAND:CARTE:TStory01\nQUAND:CARTE:TRaynor02\n+0:WAIT");
    sc2_banc_mpq_lu("Maps\\Campaign\\TRaynor02.SC2Map\\DocumentHeader");
    tour_a(1.0);
    sc2_banc_mpq_lu("Maps\\Campaign\\TStory01.SC2Map\\DocumentHeader");
    tour_a(1.1);
    tour_a(1.2);
    CHECK(B.seg[1].etat == E_DECLENCHE && B.seg[2].etat == E_ARME,
          "CARTE puis CARTE : TRaynor02 lu pendant l'attente de TStory01 → le segment TRaynor02 ne part pas à son armement");
    sc2_banc_mpq_lu("Maps\\Campaign\\TRaynor02.SC2Map\\DocumentHeader");
    tour_a(1.3);
    CHECK(B.seg[2].etat == E_DECLENCHE, "CARTE puis CARTE : TRaynor02 relu après l'armement → déclenché");
}

static void tests_pret(void)
{
    reinit();
    charger("QUAND:PRET MAX:30 SINON:QUIT\n+0.5:KEY:0x20");
    g_masque = 0x00; tour_a(1.0);
    g_masque = 0x01; tour_a(1.1);
    CHECK(B.seg[1].etat == E_ARME, "PRET : masque 0 puis 0x01 sans Loading → rien");
    g_masque = 0x10; tour_a(1.2);
    g_masque = 0x11; tour_a(1.3);
    CHECK(B.seg[1].etat == E_ARME, "PRET : Loading (0x10) posé → pas encore");
    g_masque = 0x01; tour_a(4.0);
    CHECK(B.seg[1].etat == E_DECLENCHE && B.seg[1].t_decl == 4.0 && journal_a("QUAND:PRET déclenché (masque GlueUI 0x01)"),
          "PRET : masque 0 → 0x10 → 0 (bit retombé) → déclenché");
    tour_a(4.6);
    CHECK(g_touches_bas == 1 && g_arrets == 0, "PRET : +0.5:KEY:0x20 joué, pas d'arrêt (MAX non échu)");

    reinit();
    charger("QUAND:PRET");
    g_masque = 0x10; tour_a(1.0);
    reinit();                                            /* Loading déjà posé AVANT l'armement : il faut le voir après */
    g_masque = 0x10;
    charger("QUAND:FRAME:2\nQUAND:PRET");
    tour_a(1.0);
    g_frames = 2; tour_a(1.1);
    g_masque = 0x00; tour_a(1.2);
    CHECK(B.seg[2].etat == E_ARME, "PRET : Loading vu seulement avant l'armement → retombé ensuite ne suffit pas");
    g_masque = 0x10; tour_a(1.3);
    g_masque = 0x00; tour_a(1.4);
    CHECK(B.seg[2].etat == E_DECLENCHE, "PRET : posé puis retombé après l'armement → déclenché");

    reinit();
    charger("QUAND:CHARGEMENT\n+0:WAIT\nQUAND:GLUE:0x11\n+0:WAIT\nQUAND:FRAME:100\n+0:WAIT");
    g_masque = 0x10; tour_a(1.0);
    CHECK(B.seg[1].etat == E_ARME, "CHARGEMENT : Loading déjà posé au 1er tour (pas de front) → rien");
    g_masque = 0x00; tour_a(1.1);
    g_masque = 0x10; tour_a(1.2);
    CHECK(B.seg[1].etat == E_DECLENCHE, "CHARGEMENT : front montant du bit Loading → déclenché");
    g_masque = 0x10; tour_a(1.3);
    CHECK(B.seg[2].etat == E_ARME, "GLUE:0x11 : masque 0x10 → rien");
    g_masque = 0x31; tour_a(1.4);
    CHECK(B.seg[2].etat == E_DECLENCHE, "GLUE:0x11 : masque 0x31 → déclenché");
    g_frames = 99; tour_a(1.5);
    CHECK(B.seg[3].etat == E_ARME, "FRAME:100 : 99 images → rien");
    g_frames = 100; tour_a(1.6);
    CHECK(B.seg[3].etat == E_DECLENCHE, "FRAME:100 : 100 images → déclenché");

    reinit();
    charger("QUAND:STABLE:2\n+0:WAIT");
    tour_a(1.0);
    sc2_banc_mpq_lu("Mods\\Core.SC2Mod\\Base.SC2Data\\GameData\\UnitData.xml"); /* lecture à t = 1,0 */
    tour_a(2.9);
    CHECK(B.seg[1].etat == E_ARME, "STABLE:2 : lecture MPQ à t=1,0 → rien à t=2,9");
    tour_a(3.05);
    CHECK(B.seg[1].etat == E_DECLENCHE, "STABLE:2 : aucune lecture depuis 2 s → déclenché");
}

static void tests_fichier(void)
{
    static const char SAVE[] = "C:\\Users\\sc2\\Documents\\StarCraft II\\Saves\\Unsaved\\The Outlaws Victory.SC2Save";
    reinit();
    charger("QUAND:FICHIER:*victory.sc2save ECRITURE\n+0:WAIT");
    sc2_banc_fichier(SAVE, 0);                          /* lecture : ignorée (ECRITURE) */
    sc2_banc_fichier("C:\\x\\Banks\\TCampaign.SC2Bank", 1);
    tour_a(1.0);
    CHECK(B.seg[1].etat == E_ARME, "FICHIER ECRITURE : ouverture en lecture, ou autre fichier → rien");
    horloge(1.5);
    sc2_banc_fichier(SAVE, 1);
    tour_a(2.0);
    CHECK(B.seg[1].etat == E_DECLENCHE && B.seg[1].t_decl == 1.5 && journal_a("CreateFileW en écriture"),
          "FICHIER ECRITURE : CreateFileW en écriture de …Victory.SC2Save (glob sans casse) → déclenché, horodaté au rappel");
    reinit();
    charger("QUAND:FICHIER:C:\\Users\\sc2\\Documents\\StarCraft?II\\Saves\\*\\*.SC2Save\n+0:WAIT");
    sc2_banc_fichier(SAVE, 0);
    tour_a(1.0);
    CHECK(B.seg[1].etat == E_DECLENCHE, "FICHIER sans ECRITURE : lecture acceptée, « ? » et « * » (traverse « \\ »)");
}

static void tests_finpartie(void)
{
    reinit();
    charger("QUAND:FINPARTIE MAX:600 SINON:SUITE\n+2:CAPTURE:fin\n+12:QUIT");
    CHECK(B.nseg == 2 && B.seg[1].decl == Q_FINPARTIE && B.seg[1].max == 600 && B.seg[1].sinon == S_SUITE && B.rejetes == 0,
          "FINPARTIE : marqueur accepté avec MAX et SINON, aucun rejet");
    horloge(10.0);
    sc2_banc_fin_partie("L_00B80810(1) cadre +0x3C4");
    tour_a(10.5);
    CHECK(B.seg[1].etat == E_DECLENCHE && B.seg[1].t_decl == 10.0 && journal_a("fin de partie : L_00B80810(1) cadre +0x3C4"),
          "FINPARTIE : rappel après l'armement → déclenché au tour suivant, horodaté au rappel");
    tour_a(12.5);
    CHECK(sc2_banc_capture_prendre() != NULL, "FINPARTIE : +2:CAPTURE:fin part du déclenchement");
    reinit();
    sc2_banc_fin_partie("L_00B7DC50(1) depuis L_00A28EA0");   /* BX.arme = 0 : le rappel d'avant l'armement est ignoré */
    charger("QUAND:FINPARTIE MAX:5 SINON:SUITE\n+0:WAIT");
    tour_a(4.9);
    CHECK(B.seg[1].etat == E_ARME, "FINPARTIE : un rappel d'avant l'armement ne compte pas, MAX pas encore échu");
    tour_a(5.0);
    CHECK(B.seg[1].etat == E_DECLENCHE && journal_a("QUAND:FINPARTIE déclenché au délai MAX"),
          "FINPARTIE : sans rappel, MAX:5 SINON:SUITE déclenche (forme du texte du bilan)");
}

static void tests_max_sinon(void)
{
    reinit();
    charger("QUAND:CARTE:X MAX:5\n+1:CLIC:1,1");
    tour_a(4.9);
    CHECK(B.seg[1].etat == E_ARME, "MAX:5 : pas échu à t=4,9");
    tour_a(5.0);
    CHECK(B.seg[1].etat == E_DECLENCHE && journal_a("déclenché au délai MAX"), "MAX:5 SINON:SUITE (défaut) : déclenché à t=5");
    tour_a(6.1);
    CHECK(g_souris == 1, "MAX:5 SINON:SUITE : ses relatifs partent du délai");

    reinit();
    charger("QUAND:PRET MAX:5 SINON:QUIT\n+0:CLIC:1,1");
    tour_a(5.2);
    CHECK(g_arrets == 1 && g_dern_code == 3 && strstr(g_dern_arret, "QUAND:PRET non déclenché en MAX:5s (SINON:QUIT)") && g_souris == 0,
          "MAX:5 SINON:QUIT : sc2_stop(3, « … QUAND:PRET non déclenché en MAX:5s (SINON:QUIT) … »)");

    reinit();
    charger("QUAND:PRET MAX:5 SINON:SAUTE\n+0:CLIC:1,1\nQUAND:FRAME:10\n+0:CLIC:2,2");
    tour_a(5.0);
    CHECK(B.seg[1].etat == E_SAUTE && B.seg[2].etat == E_ARME && g_souris == 0, "MAX:5 SINON:SAUTE : segment sauté, le suivant armé");
    g_frames = 10; tour_a(5.1);
    tour_a(5.2);
    CHECK(B.seg[2].etat == E_DECLENCHE && g_souris == 1 && g_dern_x == 2, "SINON:SAUTE : les éléments du segment sauté ne sont jamais joués");

    /* déclenchement : restes du segment joué annulés, touches et boutons maintenus relâchés */
    reinit();
    charger("1:CLIC:1,1\n2:KEYDOWN:0x10\n3:DOWN:5,5\n50:CLIC:7,7\nQUAND:FRAME:3\n+0:WAIT");
    tour_a(1.0); tour_a(1.1); tour_a(2.0); tour_a(3.0);
    t_vider(&g_appels);
    g_frames = 3; tour_a(10.0);
    CHECK(B.seg[1].etat == E_DECLENCHE && B.seg[1].annules == 1 && B.annules == 1,
          "déclenchement : l'absolu restant (50:CLIC) est annulé");
    CHECK(strstr(t_str(&g_appels), "touche 0x10 0") && strstr(t_str(&g_appels), "bouton 0 0"),
          "déclenchement : KEYDOWN:0x10 et DOWN maintenus → relâchés");
    tour_a(60.0);
    CHECK(g_dern_x != 7, "déclenchement : le 50:CLIC annulé n'est jamais joué");

    reinit();
    charger("GARDE:30\n100:QUIT\nQUAND:PRET\n+1:QUIT");
    tour_a(29.9);
    CHECK(g_arrets == 0, "GARDE:30 : rien à t=29,9");
    tour_a(30.0);
    CHECK(g_arrets == 1 && g_dern_code == 0 && strstr(g_dern_arret, "GARDE:30 atteinte"), "GARDE:30 : arrêt volontaire à t=30, quel que soit le segment");
}

/* ---- échelle (inventaire §6.2, §11.3) --------------------------------------------------------------------------- */
static void tests_echelle(void)
{
    static const struct { const char *ref; uint32_t w, h; const char *xy; int32_t x, y; } T[] = {
        { "REF:1280x720", 1280, 1024, "725,627@ui",    753, 836 },   /* Launch : boîte 4:3 en haut (mesure Fold 752,836) */
        { "REF:1280x720", 1280, 1024, "270,510@scene", 270, 662 },   /* Mission : scène, y + 152 */
        { "REF:1280x720", 1280, 1024, "640,360@lin",   640, 512 },
        { "REF:1280x720", 1280, 1024, "725,627@brut",  725, 627 },
        { "REF:1280x720", 1280, 1024, "725,627",       753, 836 },   /* défaut : ui */
        { "REF:1280x720", 1280,  720, "725,627@ui",    725, 627 },   /* même taille : identité dans chaque mode */
        { "REF:1280x720", 1280,  720, "270,510@scene", 270, 510 },
        { "REF:1280x720", 1280,  720, "725,627@lin",   725, 627 },
        { "REF:1280x720", 1920, 1080, "725,627@ui",   1088, 941 },   /* 16:9 : les trois modes = étirement linéaire */
        { "REF:1280x720", 1920, 1080, "270,510@scene", 405, 765 },
        { "REF:1280x720", 1920, 1080, "725,627@lin",  1088, 941 },
        { "REF:1280x720", 2560, 1080, "640,0@ui",     1280,   0 },   /* 21:9 : boîte centrée */
        { "REF:1280x720", 1280, 1024, "50%,25%",       640, 256 },   /* % du client réel */
        { "",             1280, 1024, "725,627",       725, 627 },   /* sans REF : pixels bruts (format 1) */
        { "",             1280, 1024, "725,627@ui",    725, 627 },   /* sans REF, @ui : bruts (avertissement à l'init) */
        { "",             1280, 1024, "50%,25%",       640, 256 },
    };
    int n = (int)(sizeof T / sizeof T[0]);
    for (int k = 0; k < n; k++) {
        reinit();
        g_cli_w = T[k].w; g_cli_h = T[k].h;
        char s[256]; snprintf(s, sizeof s, "%s%s1:CLIC:%s", T[k].ref, *T[k].ref ? "\n" : "", T[k].xy);
        charger(s);
        tour_a(1.0);
        int bon = B.rejetes == 0 && g_dern_x == T[k].x && g_dern_y == T[k].y;
        CHECKF(bon, "échelle : %s client %ux%u, (%s) → (%d, %d) attendu (%d, %d)", *T[k].ref ? T[k].ref : "sans REF", T[k].w, T[k].h,
               T[k].xy, g_dern_x, g_dern_y, T[k].x, T[k].y);
    }
    reinit();
    g_cli_w = 1280; g_cli_h = 1024;
    charger("REF:1280x720\nECHELLE:scene\n1:CLIC:270,510\n2:DRAG:0,0,100,100,1@ui");
    tour_a(1.0);
    CHECK(g_dern_x == 270 && g_dern_y == 662, "échelle : ECHELLE:scene change le défaut");
    tour_a(1.1); tour_a(2.0);
    CHECK(g_dern_x == 640 - 853 && g_dern_y == 0, "échelle : DRAG@ui, point de départ ramené au client");
}

/* ---- plafonds : 1 024 événements, fichier > 16 Kio ---------------------------------------------------------------- */
static void tests_plafonds(void)
{
    char fic[] = "/tmp/sc2-test-banc-XXXXXX";
    int fd = mkstemp(fic);
    if (fd < 0) { CHECK(0, "plafonds : mkstemp"); return; }
    FILE *f = fdopen(fd, "w");
    for (int i = 0; i < 1025; i++) fprintf(f, "%d:WAIT\n", i + 1);
    fclose(f);
    char var[600]; snprintf(var, sizeof var, "@%s", fic);
    setenv("SC2_BANC_ENTREES", var, 1);
    reinit(); g_bavard = 0;
    sc2_banc_entrees_init();
    g_bavard = 1;
    CHECKF(B.n == 1024 && B.rejetes == 1 && B.ev[1023].t == 1024, "plafonds : 1 024 événements acceptés, le 1 025e rejeté (%d, %d rejet)", B.n, B.rejetes);
    f = fopen(fic, "w");
    for (int i = 0; i < 1024; i++) fprintf(f, "%d:WAIT\n", i + 1);
    fclose(f);
    reinit(); g_bavard = 0;
    sc2_banc_entrees_init();
    g_bavard = 1;
    CHECK(B.n == 1024 && B.rejetes == 0, "plafonds : exactement 1 024 événements, 0 rejet");
    f = fopen(fic, "w");
    for (int i = 0; i < 400; i++) fprintf(f, "# ligne de commentaire de remplissage n°%03d ................................\n", i);
    fprintf(f, "QUAND:PRET\n+5:CLIC:7,7\n");
    long taille = ftell(f);
    fclose(f);
    reinit(); g_bavard = 0;
    sc2_banc_entrees_init();
    memset(&V1, 0, sizeof V1);
    v1_sc2_banc_entrees_init();
    g_bavard = 1;
    CHECKF(taille > 16384 && B.n == 1 && B.ev[0].x == 7 && B.nseg == 2 && B.rejetes == 0 && V1.n == 0,
           "plafonds : fichier de %ld octets (> 16 Kio) lu EN ENTIER (l'ancien analyseur perdait sa fin : %d événement)", taille, V1.n);
    reinit();
    /* ~10,1 Kio de texte (11 + 10 110 + 19 octets + NUL) : tampon de 16 Kio, chaque écriture bornée au reste. */
    static char gros[16384];
    size_t pos = 0;
    int ecrit = snprintf(gros, sizeof gros, "QUAND:PRET\n");
    int deborde = ecrit < 0 || (size_t)ecrit >= sizeof gros;
    if (!deborde) pos = (size_t)ecrit;
    for (int i = 0; i < 1020 && !deborde; i++) {
        ecrit = snprintf(gros + pos, sizeof gros - pos, "+%d:WAIT\n", i);
        if (ecrit < 0 || (size_t)ecrit >= sizeof gros - pos) deborde = 1; else pos += (size_t)ecrit;
    }
    if (!deborde) {
        ecrit = snprintf(gros + pos, sizeof gros - pos, "+2000:TEXTE:abcdef\n");
        if (ecrit < 0 || (size_t)ecrit >= sizeof gros - pos) deborde = 1; else pos += (size_t)ecrit;
    }
    CHECKF(!deborde, "plafonds : scénario TEXTE de %zu octets construit sans déborder son tampon de %zu", pos, sizeof gros);
    if (!deborde) charger(gros);
    CHECK(!deborde && B.n == 1020 && B.rejetes == 1, "plafonds : TEXTE qui dépasserait 1 024 événements → rejeté en entier");
    unlink(fic);
    unsetenv("SC2_BANC_ENTREES");
}

/* ---- CAPTURE et actions ----------------------------------------------------------------------------------------- */
static void tests_capture_actions(void)
{
    reinit();
    charger("1:CAPTURE:traynor02-chargement\n1:WAIT\n2:CAPTURE:a.b_c-9");
    CHECK(sc2_banc_capture_prendre() == NULL, "CAPTURE : rien de demandé avant le tour");
    g_frames = 41; tour_a(1.0);
    const char *c1 = sc2_banc_capture_prendre(), *c2 = sc2_banc_capture_prendre();
    CHECK(c1 && !strcmp(c1, "traynor02-chargement") && c2 == NULL && journal_a("CAPTURE traynor02-chargement demandée (Present suivant le n°41)"),
          "CAPTURE : nom transmis UNE fois à sc2_banc_capture_prendre (Present suivant), puis NULL");
    CHECK(!journal_a("WAIT"), "CAPTURE : termine le tour (une capture par Present)");
    tour_a(2.0);
    c1 = sc2_banc_capture_prendre();
    CHECK(c1 && !strcmp(c1, "a.b_c-9") && journal_a("WAIT (jalon)"), "CAPTURE : deuxième nom, WAIT joué au tour suivant");

    reinit();
    charger("1:RCLIC:30,40\n2:DRAG:10,10,110,60,1\n5:MOLETTE:-120\n6:TEXTE:z9");
    tour_a(1.0);
    CHECK(strstr(t_str(&g_appels), "souris 30 40\n1 bouton 2 1") != NULL, "RCLIC : bouton droit enfoncé en (30, 40)");
    tour_a(1.1);
    CHECK(strstr(t_str(&g_appels), "bouton 2 0") != NULL && journal_a("bouton droit relâché (fin de RCLIC)"), "RCLIC : relâché au tour suivant");
    t_vider(&g_appels);
    tour_a(2.0);
    tour_a(2.5);
    CHECK(g_dern_x == 60 && g_dern_y == 35, "DRAG : à mi-course (t0 + 0,5 s), curseur à mi-chemin (60, 35)");
    tour_a(3.2);
    CHECK(g_dern_x == 110 && g_dern_y == 60 && strstr(t_str(&g_appels), "bouton 0 1") && strstr(t_str(&g_appels), "bouton 0 0")
          && B.drag.actif == 0, "DRAG : enfoncé au départ, relâché à l'arrivée (110, 60)");
    tour_a(5.0);
    CHECK(g_molettes == 1 && g_dern_x == -120, "MOLETTE:-120 → sc2_input_molette(-120, 0)");
    t_vider(&g_appels);
    tour_a(6.0); tour_a(6.1); tour_a(6.35);
    CHECK(strstr(t_str(&g_appels), "touche 0x5A 1") && strstr(t_str(&g_appels), "touche 0x39 1"), "TEXTE:z9 → KEY 0x5A puis 0x39");
}

/* ---- le scénario d'acceptation lui-même : analyse et déroulé simulé, en 1280×720 et 1280×1024 ------------------ */
static void tests_scenario_evt(void)
{
    if (!g_depot[0] && !trouver_depot()) { CHECK(0, "scénario evt : dépôt introuvable"); return; }
    char chemin[1200]; snprintf(chemin, sizeof chemin, "%s/scripts/entrees/bar-lancer-mission-evt.txt", g_depot);
    FILE *f = fopen(chemin, "r");
    if (!f) { CHECKF(0, "scénario evt : %s illisible", chemin); return; }
    static char texte[65536]; size_t n = fread(texte, 1, sizeof texte - 1, f); texte[n] = 0; fclose(f);
    static const uint32_t H[2] = { 720, 1024 };
    for (int k = 0; k < 2; k++) {
        reinit(); g_bavard = 0;
        g_cli_h = H[k];
        charger(texte);
        int ana = B.rejetes == 0 && B.nseg == 4 && B.seg[1].decl == Q_CARTE && !strcmp(B.seg[1].arg, "TStory01")
                  && B.seg[2].decl == Q_PRET && B.seg[3].decl == Q_CARTE && !strcmp(B.seg[3].arg, "TRaynor02") && B.ref_w == 1280;
        /* déroulé : bar lu, chargé (Loading posé puis retombé), Mission, Launch, carte de la mission lue */
        double t = 0.1;
        tour_a(t);
        sc2_banc_mpq_lu("Maps\\Campaign\\TStory01.SC2Map\\DocumentHeader");
        g_masque = 0x10;
        for (int i = 0; i < 60; i++) tour_a(t += 0.05);
        g_masque = 0x00;
        for (int i = 0; i < 20000 && B.i < B.seg[2].fin; i++) tour_a(t += 1.0 / 60);   /* Échap, Mission, Launch joués */
        int lance = B.seg[2].etat == E_DECLENCHE && B.seg[3].etat == E_ARME && B.i == B.seg[2].fin;
        for (int i = 0; i < 600; i++) tour_a(t += 1.0 / 60);
        sc2_banc_mpq_lu("Maps\\Campaign\\TRaynor02.SC2Map\\DocumentHeader");
        double t_carte = g_sim;
        for (int i = 0; i < 2000 && !g_arrets; i++) tour_a(t += 1.0 / 60);
        char mission[64], launch[64];
        snprintf(mission, sizeof mission, "souris 270 %d\n", k ? 662 : 510);
        snprintf(launch, sizeof launch, "souris %d %d\n", k ? 753 : 725, k ? 836 : 627);
        g_bavard = 1;
        CHECKF(ana && lance && strstr(t_str(&g_appels), mission) && strstr(t_str(&g_appels), launch) && g_arrets == 1 && g_dern_code == 0
               && g_sim - t_carte > 9.999 && g_sim - t_carte < 10.1 && B.seg[3].etat == E_DECLENCHE,
               "scénario bar-lancer-mission-evt.txt en 1280x%u : 0 rejet, CARTE:TStory01 → PRET → Mission (%s) → Launch (%s) → "
               "CARTE:TRaynor02 → QUIT 10 s après", H[k], k ? "270, 662" : "270, 510", k ? "753, 836" : "725, 627");
    }
}

/* ---- rappels depuis un autre fil pendant les tours (verrou de la boîte aux lettres) --------------------------- */
static volatile int g_fin_fil;
static void *fil_lecteur(void *x)
{
    (void)x;
    for (unsigned i = 0; !g_fin_fil; i++) {
        sc2_banc_mpq_lu(i % 1000 == 999 ? "Maps\\Campaign\\TStory01.SC2Map\\DocumentHeader" : "Maps\\Campaign\\TStory01.SC2Map\\Objects");
        sc2_banc_fichier("C:\\x\\y.SC2Bank", i & 1);
    }
    return NULL;
}
static void tests_fils(void)
{
    reinit(); g_bavard = 0;
    static char texte[8192]; char *p = texte;
    for (int k = 0; k < 40; k++) p += sprintf(p, "QUAND:CARTE:TStory01\n+0:WAIT\n");
    charger(texte);
    pthread_t th; g_fin_fil = 0;
    int ok = !pthread_create(&th, NULL, fil_lecteur, NULL);
    for (int i = 0; ok && i < 2000000 && B.seg_arme < B.nseg; i++) tour_a(1.0 + i * 1e-4);
    g_fin_fil = 1;
    if (ok) pthread_join(th, NULL);
    g_bavard = 1;
    CHECKF(ok && B.seg_arme == B.nseg && B.seg[40].etat == E_DECLENCHE,
           "fils : 40 segments CARTE déclenchés par des rappels venus d'un autre fil (%d/40)", B.seg_arme - 1);
}

int main(void)
{
    tests_analyse();
    tests_tri();
    tests_detection();
    tests_annulation();
    tests_tour();
    tests_identite_format1();
    tests_carte();
    tests_pret();
    tests_fichier();
    tests_finpartie();
    tests_max_sinon();
    tests_echelle();
    tests_plafonds();
    tests_capture_actions();
    tests_scenario_evt();
    tests_fils();
    printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
