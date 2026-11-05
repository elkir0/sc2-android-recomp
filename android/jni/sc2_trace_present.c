/* sc2_trace_present.c — passe 7p3 : mesure du chemin de présentation (cf. sc2_trace_present.h).
 * Un seul fil appelle ces fonctions (le fil du jeu, qui fait ses Present) : pas de verrou. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sc2_trace_present.h"

void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static const char *const NOM[TP_N] = { "jeu", "backend", "acquisition", "enreg", "soumission", "present", "barriere", "reste" };

#define NB_CASES 8000                      /* histogramme du cycle par 0,25 ms : 0 → 2 s (au-delà : dernière case) */
typedef struct { uint64_t n; double s[TP_N], cyc, cmax; uint32_t h[NB_CASES]; } cumul;

static int g_actif = -1;
static unsigned g_fenetre = 600;
static double g_debut[TP_N], g_cur[TP_N], g_sortie_prec, g_entree;   /* g_cur : phases de la frame en cours */
static cumul g_fen, g_tout;
static uint64_t g_frame;

static double maintenant(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;   /* ms */
}

int sc2tp_actif(void)
{
    if (g_actif < 0) {
        const char *e = getenv("SC2_TRACE_PRESENT"), *f = getenv("SC2_TRACE_PRESENT_FENETRE");
        g_actif = e && *e == '1';
        if (f && atoi(f) > 0) g_fenetre = (unsigned)atoi(f);
        if (g_actif) sc2_log("[PRESENT] SC2_TRACE_PRESENT=1 : temps par phase, une ligne toutes les %u frames", g_fenetre);
    }
    return g_actif;
}

void sc2tp_debut(int p) { if (sc2tp_actif()) g_debut[p] = maintenant(); }
void sc2tp_fin(int p)
{
    if (!sc2tp_actif() || !g_debut[p]) return;
    g_cur[p] += maintenant() - g_debut[p]; g_debut[p] = 0;
}

void sc2tp_entree_present(void)
{
    if (!sc2tp_actif()) return;
    g_entree = maintenant();
    memset(g_debut, 0, sizeof g_debut);              /* une phase restée ouverte (sortie d'erreur) n'est pas comptée */
    if (g_sortie_prec) g_cur[TP_JEU] = g_entree - g_sortie_prec;
    g_debut[TP_BACKEND] = g_entree;
}

static double centile(const cumul *c, double q)
{
    uint64_t cible = (uint64_t)(q * (double)c->n), k = 0;
    for (int i = 0; i < NB_CASES; i++) { k += c->h[i]; if (k > cible) return (i + 1) * 0.25; }
    return NB_CASES * 0.25;
}

static int texte(const cumul *c, char *o, size_t cap)
{
    if (!c->n) { snprintf(o, cap, "aucune frame mesurée"); return 0; }
    double moy = c->cyc / (double)c->n;
    int k = snprintf(o, cap, "%llu frames, cycle moyen %.2f ms = %.2f images/s (p50 %.2f, p95 %.2f, max %.1f ms) ; ms/frame :",
                     (unsigned long long)c->n, moy, moy > 0 ? 1000.0 / moy : 0.0, centile(c, 0.50), centile(c, 0.95), c->cmax);
    for (int p = 0; p < TP_N && k > 0 && (size_t)k < cap; p++)
        k += snprintf(o + k, cap - (size_t)k, " %s %.2f (%.0f %%)", NOM[p], c->s[p] / (double)c->n,
                      c->cyc > 0 ? 100.0 * c->s[p] / c->cyc : 0.0);
    return k;
}

void sc2tp_sortie_present(void)
{
    if (!sc2tp_actif()) return;
    double t = maintenant();
    /* RESTE : la part du Present de la plate-forme qui n'est ni le backend ni le présentateur */
    double reste = t - g_entree;
    for (int p = TP_BACKEND; p <= TP_BARRIERE; p++) reste -= g_cur[p];
    g_cur[TP_RESTE] = reste > 0 ? reste : 0;
    g_frame++;
    if (g_sortie_prec) {                              /* la 1re frame n'a pas de cycle : non comptée */
        double cyc = t - g_sortie_prec;
        int i = (int)(cyc / 0.25); if (i >= NB_CASES) i = NB_CASES - 1; if (i < 0) i = 0;
        cumul *cs[2] = { &g_fen, &g_tout };
        for (int q = 0; q < 2; q++) {
            cs[q]->n++; cs[q]->cyc += cyc; cs[q]->h[i]++; if (cyc > cs[q]->cmax) cs[q]->cmax = cyc;
            for (int p = 0; p < TP_N; p++) cs[q]->s[p] += g_cur[p];
        }
    }
    memset(g_cur, 0, sizeof g_cur);
    g_sortie_prec = t;
    if (g_fen.n >= g_fenetre) {
        char l[640]; texte(&g_fen, l, sizeof l);
        sc2_log("[PRESENT] frames %llu→%llu : %s", (unsigned long long)(g_frame - g_fen.n), (unsigned long long)g_frame, l);
        memset(&g_fen, 0, sizeof g_fen);
    }
}

void sc2tp_bilan(char *o, size_t cap)
{
    if (!sc2tp_actif()) { if (cap) o[0] = 0; return; }
    texte(&g_tout, o, cap);
}
