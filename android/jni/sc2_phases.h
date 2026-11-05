/* sc2_phases.h — phases d'un run Android (passe 7r) : démarrage → titre → menu → chargement, repérées
 * dans le journal du runtime (stdout → logcat, fil_log de sc2_android_main.c), pour mesurer la cadence
 * PAR PHASE (baseline de perf du Fold) et déclencher les captures du banc (android/outils/banc-7r.sh).
 *
 *   - « démarrage » : du lancement à la 1re frame affichée ;
 *   - « titre »     : dès la 1re frame affichée (en mode -run, c'est l'écran de chargement initial) ;
 *   - puis les règles « nom=motif » dans l'ordre : la phase passe à la règle i dès qu'une ligne du journal
 *     contient son motif, et n'avance que vers l'avant (une ligne d'une phase passée ne fait pas reculer).
 *
 * Règles par défaut (motifs relevés sur les runs 7m/7n/7j, journaux build-7n-valider et build-7j-valider) :
 *   menu       = « ui_camapaignmenu »  1re lecture d'une texture du menu de campagne (faute de frappe d'origine)
 *   chargement = « → Maps\ »           1re lecture d'un fichier d'une carte (« Base.SC2Maps → Maps\Campaign\… »)
 * Remplacées par SC2_PHASES="nom=motif;nom=motif;…" (sc2.env), au plus SC2_PHASES_MAX règles.
 * Pur calcul, sans Android : testé sur l'hôte (android/outils/test-phases.c). */
#ifndef SC2_PHASES_H
#define SC2_PHASES_H
#include <stdio.h>
#include <string.h>

#define SC2_PHASES_MAX 8
#define SC2_PHASES_DEFAUT "menu=ui_camapaignmenu;chargement=\xe2\x86\x92 Maps\\"

typedef struct {
    int n;                                   /* phases : 0 démarrage, 1 titre, 2.. règles */
    char nom[SC2_PHASES_MAX + 2][24];
    char motif[SC2_PHASES_MAX + 2][96];
    int courante;
    double t_debut[SC2_PHASES_MAX + 2];      /* secondes depuis le lancement ; < 0 : phase non atteinte */
    unsigned k_debut[SC2_PHASES_MAX + 2];    /* frames affichées au début de la phase */
    double t_fin[SC2_PHASES_MAX + 2];
    unsigned k_fin[SC2_PHASES_MAX + 2];
} sc2_phases;

static inline void sc2_phases_init(sc2_phases *p, const char *spec)
{
    memset(p, 0, sizeof *p);
    strcpy(p->nom[0], "démarrage"); strcpy(p->nom[1], "titre");
    p->n = 2;
    if (!spec || !*spec) spec = SC2_PHASES_DEFAUT;
    while (*spec && p->n < SC2_PHASES_MAX + 2) {
        const char *fin = strchr(spec, ';'), *eg = strchr(spec, '=');
        size_t l = fin ? (size_t)(fin - spec) : strlen(spec);
        if (eg && (size_t)(eg - spec) < l && eg > spec && (size_t)(eg - spec) < sizeof p->nom[0]
            && l - (size_t)(eg - spec) - 1 < sizeof p->motif[0] && l - (size_t)(eg - spec) - 1 > 0) {
            memcpy(p->nom[p->n], spec, (size_t)(eg - spec));
            memcpy(p->motif[p->n], eg + 1, l - (size_t)(eg - spec) - 1);
            p->n++;
        }
        spec += l; if (*spec == ';') spec++;
    }
    for (int i = 0; i < p->n; i++) p->t_debut[i] = p->t_fin[i] = -1;
    p->t_debut[0] = 0; p->courante = 0;
}

/* Passe à la phase i (> courante) à l'instant t, k frames affichées. Rend 1 si la phase a changé. */
static inline int sc2_phases_aller(sc2_phases *p, int i, double t, unsigned k)
{
    if (i <= p->courante || i >= p->n) return 0;
    p->t_fin[p->courante] = t; p->k_fin[p->courante] = k;
    p->courante = i; p->t_debut[i] = t; p->k_debut[i] = k;
    return 1;
}

/* Ligne du journal : rend l'indice de la NOUVELLE phase, ou -1. On prend la règle la plus avancée qui
 * correspond (une ligne peut sauter une phase). */
static inline int sc2_phases_ligne(sc2_phases *p, const char *ligne, double t, unsigned k)
{
    for (int i = p->n - 1; i > p->courante && i >= 2; i--)
        if (strstr(ligne, p->motif[i])) return sc2_phases_aller(p, i, t, k) ? i : -1;
    return -1;
}

/* Frame affichée n° k (1, 2, …) : la 1re ouvre la phase « titre ». */
static inline int sc2_phases_frame(sc2_phases *p, double t, unsigned k)
{
    return k >= 1 && p->courante == 0 ? (sc2_phases_aller(p, 1, t, k - 1) ? 1 : -1) : -1;
}

/* Cadence moyenne de la phase i, close ou en cours (t, k = maintenant). Rend la durée, -1 si non atteinte. */
static inline double sc2_phases_mesure(const sc2_phases *p, int i, double t, unsigned k, unsigned *frames, double *fps)
{
    if (p->t_debut[i] < 0) return -1;
    double tf = p->t_fin[i] >= 0 ? p->t_fin[i] : t;
    unsigned kf = p->t_fin[i] >= 0 ? p->k_fin[i] : k;
    double d = tf - p->t_debut[i];
    *frames = kf - p->k_debut[i];
    *fps = d > 0 ? *frames / d : 0;
    return d;
}

/* Ligne « [PHASE] … » au passage dans la phase courante (bilan de la phase quittée). */
static inline void sc2_phases_texte_passage(const sc2_phases *p, char *s, size_t n)
{
    int i = p->courante, a = i - 1;
    while (a > 0 && p->t_debut[a] < 0) a--;
    unsigned f = 0; double fps = 0, d = sc2_phases_mesure(p, a, 0, 0, &f, &fps);
    snprintf(s, n, "[PHASE] %s à %.1f s (frame affichée %u) ; %s : %u images en %.1f s = %.1f images/s",
             p->nom[i], p->t_debut[i], p->k_debut[i] + 1, p->nom[a], f, d, fps);
}

/* Lignes « [BILAN] [FPS] » : une par phase atteinte (à la sortie). Rend le nombre de lignes écrites. */
static inline int sc2_phases_bilan(const sc2_phases *p, double t, unsigned k, char lignes[][160], int max)
{
    int m = 0;
    for (int i = 0; i < p->n && m < max; i++) {
        unsigned f = 0; double fps = 0, d = sc2_phases_mesure(p, i, t, k, &f, &fps);
        if (d < 0) continue;
        snprintf(lignes[m++], 160, "[BILAN] [FPS] phase %-10s : %6u images en %7.1f s = %5.1f images/s (de %.1f s%s)",
                 p->nom[i], f, d, fps, p->t_debut[i], p->t_fin[i] >= 0 ? "" : ", en cours");
    }
    return m;
}

#endif
